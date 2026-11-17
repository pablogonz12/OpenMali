/* SPDX-License-Identifier: MIT */
/* LD_PRELOAD tracer for /dev/mali0 traffic. One JSON line per event to $IOCTL_TRACE_OUT
 * (default /tmp/ioctl_trace.jsonl). No-op for processes that never open /dev/mali*.
 * Build: aarch64 glibc, -shared -fPIC -O2 -ldl. */
#define _GNU_SOURCE
#undef _FORTIFY_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <linux/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define MAXFD 4096
static unsigned char is_mali[MAXFD];
static int out_fd = -1;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int busy;

static const char *kname(unsigned nr)
{
	switch (nr) {
	case 0: return "VERSION_CHECK_JM"; case 1: return "SET_FLAGS"; case 3: return "GET_GPUPROPS";
	case 5: return "MEM_ALLOC"; case 6: return "MEM_QUERY"; case 7: return "MEM_FREE";
	case 13: return "GET_DDK_VERSION"; case 22: return "MEM_IMPORT"; case 34: return "MEM_ALIAS";
	case 36: return "CS_QUEUE_REGISTER"; case 37: return "CS_QUEUE_KICK"; case 38: return "MEM_EXEC_INIT"; case 40: return "CS_QUEUE_REGISTER_EX"; case 14: return "MEM_JIT_INIT"; case 15: return "MEM_SYNC"; case 12: return "MEM_COMMIT?";
	case 39: return "CS_QUEUE_BIND";  case 41: return "CS_QUEUE_TERMINATE";
	case 42: return "CS_QUEUE_GROUP_CREATE_1_6"; case 43: return "CS_QUEUE_GROUP_TERMINATE"; case 44: return "CS_CPU_QUEUE_DUMP";
	case 45: return "KCPU_QUEUE_CREATE"; case 46: return "KCPU_QUEUE_DELETE"; case 47: return "KCPU_QUEUE_ENQUEUE";
	case 48: return "CS_TILER_HEAP_INIT"; case 49: return "CS_TILER_HEAP_TERM"; case 50: return "KBASE_IOCTL_CS_GET_GLB_IFACE_OLD";
	case 51: return "CS_GET_GLB_IFACE"; case 52: return "VERSION_CHECK_CSF"; case 53: return "CS_TILER_HEAP_INIT_1_13";
	case 54: return "CS_QUEUE_GROUP_CREATE_1_18"; case 58: return "CS_QUEUE_GROUP_CREATE"; case 59: return "MEM_ALLOC_EX";
	case 60: return "CS_QUEUE_GROUP_CREATE?"; case 61: return "KBASE_SET_LIMITED_CORE_COUNT";
	default: return NULL;
	}
}

static void emit(const char *s, size_t n)
{
	if (out_fd < 0) {
		const char *p = getenv("IOCTL_TRACE_OUT");
		out_fd = (int)syscall(SYS_openat, AT_FDCWD, p ? p : "/tmp/ioctl_trace.jsonl", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (out_fd < 0) return;
	}
	ssize_t r = write(out_fd, s, n);
	(void)r;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static char *hex(char *o, const void *p, size_t n)
{
	static const char d[] = "0123456789abcdef";
	const unsigned char *b = p;
	for (size_t i = 0; i < n; i++) { *o++ = d[b[i] >> 4]; *o++ = d[b[i] & 15]; }
	*o = 0;
	return o;
}

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if ((size_t)n > sizeof buf - 2) n = sizeof buf - 2;
	buf[n++] = '\n';
	pthread_mutex_lock(&mu);
	emit(buf, (size_t)n);
	pthread_mutex_unlock(&mu);
}

/* CS ring buffers registered via CS_QUEUE_REGISTER (nr 36 / 40): dumped (used part, <=16 KiB) on each CS_QUEUE_KICK (nr 37).
 * The kbase CPU mapping is SAME_VA, so the GPU VA is directly readable in this process. */
static struct { unsigned long long va; unsigned size; } rings[16];
static int nrings;
/* Follow CALL (op 0x20) targets found in the ring (address/length come from the preceding MOVE48/MOVE32 into the
 * CALL's two source registers) and dump the called CS chunk; then dump (depth <= 2) every 256-byte block that a
 * qword of 0x5000000000..0x61ffffffff points at, as long as the pages are mapped (mincore). */
static unsigned long long seen[1024];
static int nseen;
static int readable(unsigned long long a, size_t n)
{
	unsigned long long p = a & ~4095ull, e = (a + n + 4095) & ~4095ull;
	unsigned char v[8];
	for (; p < e; p += 4096)
		if (mincore((void *)(uintptr_t)p, 4096, v) != 0) return 0;
	return 1;
}
static void blk(const char *kind, unsigned long long va, size_t n, int depth)
{
	if (nseen >= 1024 || !readable(va, n)) return;
	for (int i = 0; i < nseen; i++) if (seen[i] == va) return;
	seen[nseen++] = va;
	char *b = malloc(n * 2 + 256);
	if (!b) return;
	int o = sprintf(b, "{\"t\":%.6f,\"ev\":\"%s\",\"va\":\"0x%llx\",\"len\":%zu,\"hex\":\"", now(), kind, va, n);
	hex(b + o, (const void *)(uintptr_t)va, n);
	strcat(b, "\"}\n");
	pthread_mutex_lock(&mu);
	emit(b, strlen(b));
	pthread_mutex_unlock(&mu);
	free(b);
	if (depth >= 2) return;
	const unsigned long long *q = (const unsigned long long *)(uintptr_t)va;
	for (size_t k = 0; k < n / 8; k++) {
		unsigned long long x = q[k];
		if (x >= 0x5000000000ull && x < 0x6200000000ull) blk("blk", x & ~15ull, 256, depth + 1);
		/* MOVE48 pairs inside chunks have the pointer in the low 48 bits of the instruction */
		unsigned long long lo = x & 0xffffffffffffull;
		if ((x >> 56) == 1 && lo >= 0x5000000000ull && lo < 0x6200000000ull) blk("blk", lo & ~15ull, 256, depth + 1);
	}
}
static void follow_calls(const unsigned long long *q, unsigned n)
{
	unsigned long long m48[256] = { 0 }, m32[256] = { 0 };
	for (unsigned i = 0; i < n; i++) {
		unsigned op = (unsigned)(q[i] >> 56), r = (unsigned)((q[i] >> 48) & 0xff);
		if (op == 1) m48[r] = q[i] & 0xffffffffffffull;
		else if (op == 2) m32[r] = q[i] & 0xffffffffull;
		else if (op == 0x20) {
			unsigned ra = (unsigned)((q[i] >> 40) & 0xff), rb = (unsigned)((q[i] >> 32) & 0xff);
			if (m48[ra] && m32[rb] && m32[rb] <= 65536) blk("chunk", m48[ra], m32[rb], 0);
		}
	}
}
static void ring_dump(unsigned long long va)
{
	for (int i = 0; i < nrings; i++) {
		if (rings[i].va != va) continue;
		const unsigned long long *q = (const unsigned long long *)(uintptr_t)va;
		unsigned n = rings[i].size / 8, last = 0;
		for (unsigned k = 0; k < n; k++) if (q[k]) last = k + 1;
		if (last > 2048) last = 2048;
		char *b = malloc((size_t)last * 16 + 256);
		if (!b) return;
		int o = sprintf(b, "{\"t\":%.6f,\"tid\":%ld,\"ev\":\"ring\",\"va\":\"0x%llx\",\"qwords\":%u,\"hex\":\"", now(), (long)syscall(SYS_gettid), va, last);
		hex(b + o, q, (size_t)last * 8);
		strcat(b, "\"}\n");
		pthread_mutex_lock(&mu);
		emit(b, strlen(b));
		pthread_mutex_unlock(&mu);
		free(b);
		follow_calls(q, last);
		return;
	}
}

/* CPU views of kbase BOs (cookie mmaps, off >= 0x41000, SAME_VA) and USER_IO mappings; snapshotted at every kick into
 * "<IOCTL_TRACE_OUT>.mem" as records { u64 magic 0x4d454d53, u64 kick_no, u64 va, u64 len, u64 prot, bytes[len] }. */
static int readable(unsigned long long a, size_t n);
static struct { unsigned long long va, len; int prot; } regs[512];
static int nregs;
static int mem_fd = -2;
static unsigned long long kick_no;
static void mem_snapshot(void)
{
	if (mem_fd == -2) {
		const char *p = getenv("IOCTL_TRACE_OUT");
		char path[512];
		snprintf(path, sizeof path, "%s.mem", p ? p : "/tmp/ioctl_trace.jsonl");
		mem_fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	}
	if (mem_fd < 0) return;
	kick_no++;
	for (int i = 0; i < nregs; i++) {
		if (!regs[i].len || regs[i].len > (1u << 20) || regs[i].len < 4096 || !(regs[i].prot & PROT_WRITE)) continue;
		if (!readable(regs[i].va, regs[i].len)) continue;
		unsigned long long h[5] = { 0x4d454d53ull, kick_no, regs[i].va, regs[i].len, (unsigned long long)regs[i].prot };
		ssize_t r = write(mem_fd, h, sizeof h);
		r = write(mem_fd, (const void *)(uintptr_t)regs[i].va, regs[i].len);
		(void)r;
	}
}

static int mali_path(const char *p) { return p && strncmp(p, "/dev/mali", 9) == 0; }
static void track(int fd, const char *path, const char *fn)
{
	if (fd < 0 || fd >= MAXFD || !mali_path(path)) return;
	is_mali[fd] = 1;
	line("{\"t\":%.6f,\"tid\":%ld,\"ev\":\"%s\",\"path\":\"%s\",\"fd\":%d}", now(), (long)syscall(SYS_gettid), fn, path, fd);
}

typedef int (*open_t)(const char *, int, ...);
typedef int (*openat_t)(int, const char *, int, ...);
typedef int (*close_t)(int);
typedef int (*ioctl_t)(int, unsigned long, ...);
typedef void *(*mmap_t)(void *, size_t, int, int, int, off_t);
typedef int (*munmap_t)(void *, size_t);
#define REAL(name, T) static T real; if (!real) real = (T)dlsym(RTLD_NEXT, #name)

int open(const char *path, int flags, ...)
{
	REAL(open, open_t);
	mode_t m = 0;
	if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
	int fd = real(path, flags, m);
	track(fd, path, "open");
	return fd;
}
int open64(const char *path, int flags, ...)
{
	REAL(open64, open_t);
	mode_t m = 0;
	if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
	int fd = real(path, flags, m);
	track(fd, path, "open");
	return fd;
}
int openat(int dfd, const char *path, int flags, ...)
{
	REAL(openat, openat_t);
	mode_t m = 0;
	if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
	int fd = real(dfd, path, flags, m);
	track(fd, path, "openat");
	return fd;
}
int openat64(int dfd, const char *path, int flags, ...)
{
	REAL(openat64, openat_t);
	mode_t m = 0;
	if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
	int fd = real(dfd, path, flags, m);
	track(fd, path, "openat");
	return fd;
}

int close(int fd)
{
	REAL(close, close_t);
	if (fd >= 0 && fd < MAXFD && is_mali[fd]) {
		is_mali[fd] = 0;
		line("{\"t\":%.6f,\"tid\":%ld,\"ev\":\"close\",\"fd\":%d}", now(), (long)syscall(SYS_gettid), fd);
	}
	return real(fd);
}

int ioctl(int fd, unsigned long req, ...)
{
	REAL(ioctl, ioctl_t);
	va_list a;
	va_start(a, req);
	void *arg = va_arg(a, void *);
	va_end(a);
	if (fd < 0 || fd >= MAXFD || !is_mali[fd] || busy) return real(fd, req, arg);
	busy = 1;
	unsigned nr = _IOC_NR(req), sz = _IOC_SIZE(req), dir = _IOC_DIR(req);
	size_t n = sz > 256 ? 256 : sz;
	static __thread char in[600], outh[600];
	in[0] = 0;
	if ((dir & _IOC_WRITE) && arg) hex(in, arg, n);
	double t0 = now();
	if (_IOC_TYPE(req) == 0x80 && nr == 37 && arg) { mem_snapshot(); ring_dump(*(unsigned long long *)arg); }
	int r = real(fd, req, arg);
	if (_IOC_TYPE(req) == 0x80 && (nr == 36 || nr == 40) && r == 0 && arg && nrings < 16) {
		rings[nrings].va = ((unsigned long long *)arg)[0];
		rings[nrings].size = ((unsigned *)arg)[2];
		nrings++;
	}
	int e = errno;
	outh[0] = 0;
	if ((dir & _IOC_READ) && arg && r >= 0) hex(outh, arg, n);
	const char *nm = (_IOC_TYPE(req) == 0x80) ? kname(nr) : NULL;
	line("{\"t\":%.6f,\"dt\":%.6f,\"tid\":%ld,\"ev\":\"ioctl\",\"fd\":%d,\"type\":%u,\"nr\":%u,\"size\":%u,\"dir\":%u,\"name\":\"%s\",\"in\":\"%s\",\"ret\":%d,\"errno\":%d,\"out\":\"%s\"}",
	     t0, now() - t0, (long)syscall(SYS_gettid), fd, (unsigned)_IOC_TYPE(req), nr, sz, dir, nm ? nm : "?", in, r, r < 0 ? e : 0, outh);
	busy = 0;
	errno = e;
	return r;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	REAL(mmap, mmap_t);
	void *r = real(addr, len, prot, flags, fd, off);
	int e = errno;
	if (fd >= 0 && fd < MAXFD && is_mali[fd] && r != MAP_FAILED && off >= 0x30000 && nregs < 512) {
		regs[nregs].va = (unsigned long long)(uintptr_t)r; regs[nregs].len = len; regs[nregs].prot = prot; nregs++;
	}
	if (fd >= 0 && fd < MAXFD && is_mali[fd])
		line("{\"t\":%.6f,\"tid\":%ld,\"ev\":\"mmap\",\"fd\":%d,\"addr\":\"%p\",\"len\":%zu,\"prot\":%d,\"flags\":%d,\"off\":\"0x%llx\",\"ret\":\"%p\",\"errno\":%d}",
		     now(), (long)syscall(SYS_gettid), fd, addr, len, prot, flags, (unsigned long long)off, r, r == MAP_FAILED ? e : 0);
	errno = e;
	return r;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return mmap(addr, len, prot, flags, fd, off); }

int munmap(void *addr, size_t len)
{
	REAL(munmap, munmap_t);
	int r = real(addr, len);
	int e = errno;
	for (int i = 0; i < nregs; i++) if (regs[i].va == (unsigned long long)(uintptr_t)addr) regs[i].len = 0;
	if (out_fd >= 0)
		line("{\"t\":%.6f,\"tid\":%ld,\"ev\":\"munmap\",\"addr\":\"%p\",\"len\":%zu,\"ret\":%d}", now(), (long)syscall(SYS_gettid), addr, len, r);
	errno = e;
	return r;
}
