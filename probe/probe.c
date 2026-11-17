// SPDX-License-Identifier: MIT
/*
 * probe: kernel-interface probe for Arm mali_kbase (/dev/mali0), from userspace.
 * One JSON object per line on stdout, every step independently guarded.
 * Does not create queue groups, submit work or kick queues. See README.md.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "kbase_uapi_min.h"

typedef unsigned long long ull;

/* ------------------------------------------------------------ JSON output */
static void js(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			putchar('\\');
			putchar(c);
		} else if (c < 0x20 || c == 0x7f) {
			printf("\\u%04x", c);
		} else {
			putchar(c);
		}
	}
	putchar('"');
}
static void jb(const char *test) { printf("{\"test\":"); js(test); }
static void jk(const char *k) { putchar(','); js(k); putchar(':'); }
static void jok(int ok) { jk("ok"); fputs(ok ? "true" : "false", stdout); }
static void jstr(const char *k, const char *v) { jk(k); js(v); }
static void ji(const char *k, long long v) { jk(k); printf("%lld", v); }
static void ju(const char *k, ull v) { jk(k); printf("%llu", v); }
static void jx(const char *k, ull v) { jk(k); printf("\"0x%llx\"", v); }
static void jd(const char *k, double v) { jk(k); printf("%.4f", v); }
static void jbool(const char *k, int v) { jk(k); fputs(v ? "true" : "false", stdout); }
static void jerr(int e)
{
	ji("errno", e);
	jstr("error", e ? strerror(e) : "");
}
static void jend(void) { fputs("}\n", stdout); fflush(stdout); }
static void jskip(const char *test, const char *why)
{
	jb(test); jok(0); jstr("skipped", why); jend();
}

/* ------------------------------------------------------------ fault guard */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_guard;
static void on_fault(int sig)
{
	if (g_guard)
		siglongjmp(g_jmp, sig);
	_exit(70);
}
static void install_guards(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_fault;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------ globals */
static int g_fd = -1;
static int g_csf = -1;     /* 1 = CSF handshake succeeded, 0 = JM, -1 unknown */
static unsigned g_maj, g_min;
static int g_setup_done;
static long g_page;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}
static void jhex(const char *k, const unsigned char *p, size_t n)
{
	jk(k);
	putchar('"');
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
	putchar('"');
}
static int fd_ready(void) { return g_fd >= 0 && g_setup_done; }

/* ------------------------------------------------------------ step 0/1: nodes + open */
static void step_nodes(void)
{
	static const char *paths[] = {
		"/dev/mali0", "/dev/mali", "/dev/dri", "/dev/dri/card0", "/dev/dri/renderD128",
		"/dev/dma_heap", "/dev/udmabuf", "/dev/ion", "/dev/sw_sync",
		"/sys/kernel/debug/sync/sw_sync", "/dev/ashmem", "/dev/kgsl-3d0",
	};
	for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
		struct stat st;
		jb("devnode"); jstr("path", paths[i]);
		if (stat(paths[i], &st) == 0) {
			jok(1);
			jx("mode", st.st_mode);
			ju("uid", st.st_uid); ju("gid", st.st_gid);
			ju("rdev_major", major(st.st_rdev)); ju("rdev_minor", minor(st.st_rdev));
			jbool("rw_access", access(paths[i], R_OK | W_OK) == 0);
		} else {
			int e = errno;
			jok(0); jerr(e);
		}
		jend();
	}
}

static void read_small(const char *path, char *buf, size_t n)
{
	buf[0] = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	ssize_t r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		r = 0;
	buf[r] = 0;
	for (ssize_t i = 0; i < r; i++)
		if (buf[i] == 0 || buf[i] == '\n')
			buf[i] = 0;
}

static void step_open(void)
{
	char ctx[256], groups[256] = "";
	read_small("/proc/self/attr/current", ctx, sizeof ctx);
	gid_t gl[32];
	int ng = getgroups(32, gl);
	size_t o = 0;
	for (int i = 0; i < ng && o + 12 < sizeof groups; i++)
		o += (size_t)snprintf(groups + o, sizeof groups - o, "%s%u", i ? "," : "", (unsigned)gl[i]);

	g_fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	int e = errno;
	jb("open"); jok(g_fd >= 0);
	if (g_fd < 0)
		jerr(e);
	ju("uid", getuid()); ju("gid", getgid()); jstr("groups", groups);
	jstr("selinux_context", ctx);
	jend();
}

/* ------------------------------------------------------------ step 2: handshake */
static void step_handshake(void)
{
	if (g_fd < 0) { jskip("version_check", "no fd"); jskip("set_flags", "no fd"); return; }
	struct kbase_ioctl_version_check v;
	int e_csf = 0, e_jm = 0;

	memset(&v, 0, sizeof v);
	int r_csf = ioctl(g_fd, KBASE_IOCTL_VERSION_CHECK_CSF, &v);
	if (r_csf != 0)
		e_csf = errno;
	jb("version_check_csf"); jok(r_csf == 0);
	if (r_csf == 0) { ju("major", v.major); ju("minor", v.minor); g_csf = 1; g_maj = v.major; g_min = v.minor; }
	else jerr(e_csf);
	jend();

	if (r_csf != 0) {
		memset(&v, 0, sizeof v);
		int r_jm = ioctl(g_fd, KBASE_IOCTL_VERSION_CHECK_JM, &v);
		if (r_jm != 0)
			e_jm = errno;
		jb("version_check_jm"); jok(r_jm == 0);
		if (r_jm == 0) { ju("major", v.major); ju("minor", v.minor); g_csf = 0; g_maj = v.major; g_min = v.minor; }
		else jerr(e_jm);
		jend();
		if (r_jm != 0) { jskip("set_flags", "handshake failed"); return; }
	}
	jb("handshake_summary"); jok(1); jstr("flavour", g_csf ? "CSF" : "JM");
	ju("major", g_maj); ju("minor", g_min); jend();

	struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
	int r = ioctl(g_fd, KBASE_IOCTL_SET_FLAGS, &sf);
	int e = errno;
	jb("set_flags"); jok(r == 0);
	if (r == 0) g_setup_done = 1; else jerr(e);
	jend();
}

/* ------------------------------------------------------------ step 3: gpuprops */
static const char *propname(unsigned key, char *buf, size_t n)
{
	static const char *const names[] = {
		[1] = "product_id", [2] = "version_status", [3] = "minor_revision", [4] = "major_revision",
		[6] = "gpu_freq_khz_max", [8] = "log2_program_counter_size",
		[9] = "texture_features_0", [10] = "texture_features_1", [11] = "texture_features_2",
		[12] = "gpu_available_memory_size", [13] = "l2_log2_line_size", [14] = "l2_log2_cache_size",
		[15] = "l2_num_l2_slices", [16] = "tiler_bin_size_bytes", [17] = "tiler_max_active_levels",
		[18] = "max_threads", [19] = "max_workgroup_size", [20] = "max_barrier_size",
		[21] = "max_registers", [22] = "max_task_queue", [23] = "max_thread_group_split",
		[24] = "impl_tech", [25] = "raw_shader_present", [26] = "raw_tiler_present",
		[27] = "raw_l2_present", [28] = "raw_stack_present", [29] = "raw_l2_features",
		[30] = "raw_core_features", [31] = "raw_mem_features", [32] = "raw_mmu_features",
		[33] = "raw_as_present", [34] = "raw_js_present", [51] = "raw_tiler_features",
		[52] = "raw_texture_features_0", [53] = "raw_texture_features_1", [54] = "raw_texture_features_2",
		[55] = "raw_gpu_id", [56] = "raw_thread_max_threads", [57] = "raw_thread_max_workgroup_size",
		[58] = "raw_thread_max_barrier_size", [59] = "raw_thread_features", [60] = "raw_coherency_mode",
		[61] = "coherency_num_groups", [62] = "coherency_num_core_groups", [63] = "coherency_coherency",
		[80] = "texture_features_3", [81] = "raw_texture_features_3", [82] = "num_exec_engines",
		[83] = "raw_thread_tls_alloc", [84] = "tls_alloc", [85] = "raw_gpu_features",
	};
	if (key >= 35 && key <= 50) snprintf(buf, n, "raw_js_features_%u", key - 35);
	else if (key >= 64 && key <= 79) snprintf(buf, n, "coherency_group_%u", key - 64);
	else if (key < sizeof names / sizeof *names && names[key]) snprintf(buf, n, "%s", names[key]);
	else snprintf(buf, n, "key_%u", key);
	return buf;
}

static void step_gpuprops(void)
{
	if (!fd_ready()) { jskip("gpuprops", "no handshake"); return; }
	struct kbase_ioctl_get_gpuprops gp = { 0 };
	int sz = ioctl(g_fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
	if (sz <= 0) {
		int e = errno;
		jb("gpuprops"); jok(0); jstr("phase", "size_query"); ji("ret", sz); jerr(e); jend();
		return;
	}
	unsigned char *buf = calloc(1, (size_t)sz);
	if (!buf) { jb("gpuprops"); jok(0); jerr(ENOMEM); jend(); return; }
	gp.buffer = (uint64_t)(uintptr_t)buf; gp.size = (uint32_t)sz; gp.flags = 0;
	int r = ioctl(g_fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
	if (r < 0) {
		int e = errno;
		jb("gpuprops"); jok(0); jstr("phase", "read"); ju("size", (ull)sz); jerr(e); jend();
		free(buf);
		return;
	}
	size_t len = (size_t)(r > 0 && r <= sz ? r : sz);
	uint64_t gpu_id = 0, shader = 0, l2f = 0, memf = 0, mmuf = 0, tilf = 0, asp = 0, l2p = 0;
	int have_gpu_id = 0;

	jb("gpuprops"); jok(1); ju("blob_size", (ull)len); ju("ioctl_ret", (ull)r);
	jk("props"); putchar('{');
	size_t off = 0; int first = 1;
	while (off + 4 <= len) {
		uint32_t hdr; memcpy(&hdr, buf + off, 4); off += 4;
		unsigned key = hdr >> 2, szc = hdr & 3;
		size_t vs = (size_t)1 << szc;
		if (off + vs > len) break;
		uint64_t val = 0;
		memcpy(&val, buf + off, vs);   /* little-endian host assumed (aarch64/x86-64) */
		off += vs;
		char nb[48];
		propname(key, nb, sizeof nb);
		if (!first) putchar(',');
		first = 0;
		js(nb); putchar(':');
		if (szc == 3) printf("\"0x%llx\"", (ull)val); else printf("%llu", (ull)val);
		switch (key) {
		case KBASE_GPUPROP_RAW_GPU_ID: gpu_id = val; have_gpu_id = 1; break;
		case KBASE_GPUPROP_RAW_SHADER_PRESENT: shader = val; break;
		case KBASE_GPUPROP_RAW_L2_PRESENT: l2p = val; break;
		case KBASE_GPUPROP_RAW_L2_FEATURES: l2f = val; break;
		case KBASE_GPUPROP_RAW_MEM_FEATURES: memf = val; break;
		case KBASE_GPUPROP_RAW_MMU_FEATURES: mmuf = val; break;
		case KBASE_GPUPROP_RAW_TILER_FEATURES: tilf = val; break;
		case KBASE_GPUPROP_RAW_AS_PRESENT: asp = val; break;
		default: break;
		}
	}
	putchar('}');
	ju("parsed_bytes", (ull)off);
	jk("derived"); putchar('{');
	if (have_gpu_id) {
		printf("\"gpu_id\":\"0x%llx\",\"arch_major\":%llu,\"arch_minor\":%llu,\"arch_rev\":%llu,\"product_major\":%llu,",
		       (ull)gpu_id, (ull)((gpu_id >> 28) & 0xf), (ull)((gpu_id >> 24) & 0xf),
		       (ull)((gpu_id >> 20) & 0xf), (ull)((gpu_id >> 16) & 0xf));
	}
	printf("\"shader_cores\":%d,\"l2_present\":\"0x%llx\",\"l2_features\":\"0x%llx\",\"mem_features\":\"0x%llx\","
	       "\"mmu_features\":\"0x%llx\",\"mmu_va_bits\":%llu,\"mmu_pa_bits\":%llu,\"tiler_features\":\"0x%llx\","
	       "\"as_present\":\"0x%llx\"",
	       __builtin_popcountll(shader), (ull)l2p, (ull)l2f, (ull)memf, (ull)mmuf,
	       (ull)(mmuf & 0xff), (ull)((mmuf >> 8) & 0xff), (ull)tilf, (ull)asp);
	putchar('}');
	jhex("blob_hex", buf, len);
	jend();
	free(buf);
}

/* ------------------------------------------------------------ memory helpers */
struct region {
	uint64_t flags_out, va;   /* va: cookie or GPU VA exactly as returned by the kernel */
	uint64_t va_pages;
	int same_va;
	void *map;
	size_t len;
};

static int use_alloc_ex(void) { return g_csf == 1 && (g_maj > 1 || g_min >= 9); }

static int region_release(struct region *r, int *free_err_out)
{
	int free_err = 0;
	struct kbase_ioctl_mem_free fr;
	if (r->map && r->map != MAP_FAILED)
		munmap(r->map, r->len);
	/* SAME_VA regions are torn down by munmap (free-on-close); a MEM_FREE on the
	 * stale address should fail. We still issue it so no region can ever leak;
	 * the result is reported (expected: EINVAL for SAME_VA after munmap). */
	fr.gpu_addr = r->map ? (r->same_va ? (uint64_t)(uintptr_t)r->map : r->va) : r->va;
	int rc = ioctl(g_fd, KBASE_IOCTL_MEM_FREE, &fr);
	if (rc != 0)
		free_err = errno;
	if (free_err_out)
		*free_err_out = free_err;
	r->map = NULL;
	return rc;
}


/* MEM_QUERY on the real address (mapped address for cookie regions). */
static void report_query(const struct region *r, int mapped)
{
	uint64_t a = r->same_va ? (mapped ? (uint64_t)(uintptr_t)r->map : 0) : r->va;
	static const struct { const char *n; uint64_t q; } qs[] = {
		{ "query_flags", KBASE_MEM_QUERY_FLAGS }, { "query_va_size", KBASE_MEM_QUERY_VA_SIZE },
		{ "query_commit_size", KBASE_MEM_QUERY_COMMIT_SIZE } };
	if (!a) { jstr("query", "skipped: unmapped cookie"); return; }
	for (size_t i = 0; i < 3; i++) {
		union kbase_ioctl_mem_query q;
		memset(&q, 0, sizeof q);
		q.in.gpu_addr = a; q.in.query = qs[i].q;
		if (ioctl(g_fd, KBASE_IOCTL_MEM_QUERY, &q) == 0) {
			if (i == 0) jx(qs[i].n, q.out.value); else ju(qs[i].n, q.out.value);
		} else {
			char k[48]; snprintf(k, sizeof k, "%s_errno", qs[i].n); ji(k, errno);
		}
	}
}

static void report_free(struct region *r, int mapped)
{
	int ferr = 0;
	int same = r->same_va;
	int fr = region_release(r, &ferr);
	/* A mapped cookie (SAME_VA) region is torn down by munmap; MEM_FREE afterwards must fail with EINVAL. */
	int by_munmap = same && mapped && fr != 0 && ferr == EINVAL;
	jbool("freed_by_munmap", by_munmap);
	jbool("free_ok", fr == 0 || by_munmap);
	if (fr != 0 && !by_munmap) ji("free_errno", ferr);
}

static int prot_for(uint64_t flags)
{
	int p = 0;
	if (flags & BASE_MEM_PROT_CPU_RD) p |= PROT_READ;
	if (flags & BASE_MEM_PROT_CPU_WR) p |= PROT_WRITE;
	return p;
}

/* Time 64 KiB memset + readback; returns 0 ok, -1 on fault. */
static int time_map(void *p, size_t len, double *wr, double *rd, int *verify_ok)
{
	enum { ITERS = 16 };
	volatile uint64_t sink = 0;
	g_guard = 1;
	int sig = sigsetjmp(g_jmp, 1);
	if (sig) { g_guard = 0; return -1; }
	uint64_t t0 = now_ns();
	for (int i = 0; i < ITERS; i++)
		memset(p, 0x5a + i, len);
	uint64_t t1 = now_ns();
	int ok = 1;
	volatile uint64_t *w = (volatile uint64_t *)p;
	uint64_t want = 0x0101010101010101ull * (uint64_t)(unsigned char)(0x5a + ITERS - 1);
	for (int i = 0; i < ITERS; i++) {
		uint64_t s = 0;
		for (size_t k = 0; k < len / 8; k++) {
			uint64_t v = w[k];
			s += v;
			if (i == 0 && v != want) ok = 0;
		}
		sink += s;
	}
	uint64_t t2 = now_ns();
	g_guard = 0;
	*wr = (double)(t1 - t0) / ((double)ITERS * (double)len);
	*rd = (double)(t2 - t1) / ((double)ITERS * (double)len);
	*verify_ok = ok;
	(void)sink;
	return 0;
}

/* ------------------------------------------------------------ step 4: memory */
struct memcase { const char *name; uint64_t flags; uint64_t commit_div; uint64_t ext; };

static void step_memory(void)
{
	if (!fd_ready()) { jskip("mem_alloc", "no handshake"); return; }
	const uint64_t RW = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	const struct memcase cases[] = {
		{ "rw", RW, 1, 0 },
		{ "rw_cached_cpu", RW | BASE_MEM_CACHED_CPU, 1, 0 },
		{ "rw_coherent_system", RW | BASE_MEM_COHERENT_SYSTEM, 1, 0 },
		{ "rw_cached_coherent_system", RW | BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM, 1, 0 },
		{ "rw_coherent_local", RW | BASE_MEM_COHERENT_LOCAL, 1, 0 },
		{ "rw_cached_coherent_local", RW | BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_LOCAL, 1, 0 },
		{ "rw_same_va", RW | BASE_MEM_SAME_VA, 1, 0 },
		{ "rw_same_va_cached", RW | BASE_MEM_SAME_VA | BASE_MEM_CACHED_CPU, 1, 0 },
		{ "rw_uncached_gpu", RW | BASE_MEM_UNCACHED_GPU, 1, 0 },
		{ "cpu_wr_gpu_rd", BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD, 1, 0 },
		{ "gpu_only_rw", BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR, 1, 0 },
		{ "grow_on_gpf", RW | BASE_MEM_GROW_ON_GPF, 4, 4 },
		{ "gpu_ex", BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX, 1, 0 },
	};
	const uint64_t va_pages = 16;
	const size_t len = (size_t)(va_pages * (uint64_t)g_page);

	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		const struct memcase *c = &cases[i];
		jb("mem_alloc"); jstr("case", c->name); jx("flags_in", c->flags); ju("va_pages", va_pages);
		ju("page_size", (ull)g_page);

		union kbase_ioctl_mem_alloc_ex ex;
		union kbase_ioctl_mem_alloc lg;
		const char *api = "legacy";
		int r = -1, e = 0;
		uint64_t fl_out = 0, va = 0;
		uint64_t commit = va_pages / c->commit_div;
		if (use_alloc_ex()) {
			memset(&ex, 0, sizeof ex);
			ex.in.va_pages = va_pages; ex.in.commit_pages = commit; ex.in.extension = c->ext; ex.in.flags = c->flags;
			r = ioctl(g_fd, KBASE_IOCTL_MEM_ALLOC_EX, &ex);
			e = errno;
			api = "alloc_ex";
			if (r == 0) { fl_out = ex.out.flags; va = ex.out.gpu_va; }
			else if (e == ENOTTY) { r = -1; api = "legacy"; }
			else { jok(0); jstr("api", api); jerr(e); jend(); continue; }
		}
		if (r != 0 && strcmp(api, "legacy") == 0) {
			memset(&lg, 0, sizeof lg);
			lg.in.va_pages = va_pages; lg.in.commit_pages = commit; lg.in.extension = c->ext; lg.in.flags = c->flags;
			r = ioctl(g_fd, KBASE_IOCTL_MEM_ALLOC, &lg);
			e = errno;
			if (r == 0) { fl_out = lg.out.flags; va = lg.out.gpu_va; }
		}
		if (r != 0) { jok(0); jstr("api", api); jerr(e); jend(); continue; }

		struct region rg = { .flags_out = fl_out, .va = va, .va_pages = va_pages, .len = len,
				     .same_va = (fl_out & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP)) != 0 };
		jok(1); jstr("api", api); jx("flags_out", fl_out); jx("flags_dropped", c->flags & ~fl_out);
		jx("flags_added", fl_out & ~c->flags);
		jx("gpu_va_or_cookie", va); jbool("same_va", rg.same_va); jbool("need_mmap", (fl_out & BASE_MEM_NEED_MMAP) != 0);

		int prot = prot_for(fl_out);
		int timed = 0;
		if (prot && !(c->flags & BASE_MEM_GROW_ON_GPF)) {
			void *m = mmap(NULL, len, prot, MAP_SHARED, g_fd, (off_t)va);
			if (m == MAP_FAILED) {
				jbool("mmap_ok", 0); ji("mmap_errno", errno);
			} else {
				rg.map = m;
				jbool("mmap_ok", 1);
				if (rg.same_va) jbool("mmap_addr_is_gpu_va", 1);
				if ((prot & (PROT_READ | PROT_WRITE)) == (PROT_READ | PROT_WRITE)) {
					double wr = 0, rd = 0; int vok = 0;
					if (time_map(m, len, &wr, &rd, &vok) == 0) {
						jd("write_ns_per_byte", wr); jd("read_ns_per_byte", rd); jbool("readback_ok", vok);
						timed = 1;
					} else {
						jbool("fault_during_access", 1);
					}
				}
			}
		}
		(void)timed;
		int mapped = rg.map != NULL;
		report_query(&rg, mapped);
		report_free(&rg, mapped);
		jend();
	}
}

/* ------------------------------------------------------------ step 5: dma-buf */
#define MAX_DMABUFS 12
static struct { char name[300]; int fd; } g_dmabuf[MAX_DMABUFS];
static int g_ndmabuf;

static void add_dmabuf(const char *name, int fd)
{
	if (g_ndmabuf >= MAX_DMABUFS) { close(fd); return; }
	snprintf(g_dmabuf[g_ndmabuf].name, sizeof g_dmabuf[g_ndmabuf].name, "%s", name);
	g_dmabuf[g_ndmabuf].fd = fd;
	g_ndmabuf++;
}

static void step_dma_heaps(void)
{
	DIR *d = opendir("/dev/dma_heap");
	if (!d) {
		int e = errno;
		jb("dma_heap_list"); jok(0); jerr(e); jend();
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		if (strstr(de->d_name, "prot") || strstr(de->d_name, "svp") || strstr(de->d_name, "wfd") ||
		    strstr(de->d_name, "sapu") || strstr(de->d_name, "secure") || strstr(de->d_name, "protected") ||
		    strstr(de->d_name, "tui") || strstr(de->d_name, "tee")) {
			jb("dma_heap_alloc"); jstr("heap", de->d_name); jok(0);
			jstr("skipped", "secure/firmware heap, deliberately not touched"); jend();
			continue;
		}
		char path[300];
		snprintf(path, sizeof path, "/dev/dma_heap/%s", de->d_name);
		jb("dma_heap_alloc"); jstr("heap", de->d_name);
		int hfd = open(path, O_RDWR | O_CLOEXEC);
		const char *mode = "O_RDWR";
		if (hfd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
			hfd = open(path, O_RDONLY | O_CLOEXEC);
			mode = "O_RDONLY";
		}
		if (hfd < 0) { int e = errno; jok(0); jstr("phase", "open"); jerr(e); jend(); continue; }
		struct dma_heap_allocation_data_min a;
		memset(&a, 0, sizeof a);
		a.len = 4u << 20; a.fd_flags = O_RDWR | O_CLOEXEC;
		int r = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC_MIN, &a);
		int e = errno;
		close(hfd);
		if (r != 0) { jok(0); jstr("phase", "alloc"); jstr("open_mode", mode); jerr(e); jend(); continue; }
		jok(1); jstr("open_mode", mode); ju("len", a.len); ji("fd", (int)a.fd);
		void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, (int)a.fd, 0);
		jbool("cpu_mmap_ok", m != MAP_FAILED);
		if (m != MAP_FAILED) munmap(m, 4096);
		/* CPU cost of this heap's own mapping (64 KiB memset + readback), same method as mem_alloc */
		void *m2 = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, (int)a.fd, 0);
		if (m2 != MAP_FAILED) {
			double wr = 0, rd = 0; int vok = 0;
			if (time_map(m2, 65536, &wr, &rd, &vok) == 0) { jd("write_ns_per_byte", wr); jd("read_ns_per_byte", rd); jbool("readback_ok", vok); }
			else jbool("fault_during_access", 1);
			munmap(m2, 65536);
		}
		jend();
		add_dmabuf(de->d_name, (int)a.fd);
	}
	closedir(d);
}

static void step_udmabuf(void)
{
	jb("udmabuf");
	int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (ufd < 0) { int e = errno; jok(0); jstr("phase", "open"); jerr(e); jend(); return; }
	int mfd = (int)syscall(SYS_memfd_create, "probe", 2u /* MFD_ALLOW_SEALING */);
	if (mfd < 0) { int e = errno; close(ufd); jok(0); jstr("phase", "memfd_create"); jerr(e); jend(); return; }
	if (ftruncate(mfd, 4 << 20) != 0) { int e = errno; close(mfd); close(ufd); jok(0); jstr("phase", "ftruncate"); jerr(e); jend(); return; }
	(void)fcntl(mfd, 1033 /* F_ADD_SEALS */, 0x0002 /* F_SEAL_SHRINK, required by udmabuf */);
	struct udmabuf_create_min c = { .memfd = (uint32_t)mfd, .flags = UDMABUF_FLAGS_CLOEXEC_MIN, .offset = 0, .size = 4u << 20 };
	int r = ioctl(ufd, UDMABUF_CREATE_MIN, &c);
	int e = errno;
	close(mfd); close(ufd);
	if (r < 0) { jok(0); jstr("phase", "create"); jerr(e); jend(); return; }
	jok(1); ji("fd", r); jend();
	add_dmabuf("udmabuf", r);
}

static void step_import(void)
{
	if (g_ndmabuf == 0) { jskip("dmabuf_import", "no dma-buf fd could be obtained"); return; }
	if (!fd_ready()) { jskip("dmabuf_import", "no handshake"); return; }
	const uint64_t RW = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	const struct { const char *n; uint64_t f; } var[] = {
		{ "rw_shared_coherent_system", RW | BASE_MEM_IMPORT_SHARED | BASE_MEM_COHERENT_SYSTEM },
		{ "rw_shared", RW | BASE_MEM_IMPORT_SHARED },
		{ "rw", RW },
		{ "gpu_rw_only", BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR },
		{ "gpu_rd_only", BASE_MEM_PROT_GPU_RD },
		{ "rw_shared_sync_on_map", RW | BASE_MEM_IMPORT_SHARED | BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP },
	};
	for (int b = 0; b < g_ndmabuf; b++) {
		for (size_t v = 0; v < sizeof var / sizeof *var; v++) {
			int fdv = g_dmabuf[b].fd;
			union kbase_ioctl_mem_import im;
			memset(&im, 0, sizeof im);
			im.in.flags = var[v].f; im.in.phandle = (uint64_t)(uintptr_t)&fdv; im.in.type = BASE_MEM_IMPORT_TYPE_UMM;
			jb("dmabuf_import"); jstr("source", g_dmabuf[b].name); jstr("variant", var[v].n); jx("flags_in", var[v].f);
			int r = ioctl(g_fd, KBASE_IOCTL_MEM_IMPORT, &im);
			int e = errno;
			if (r != 0) { jok(0); jerr(e); jend(); continue; }
			struct region rg = { .flags_out = im.out.flags, .va = im.out.gpu_va, .va_pages = im.out.va_pages,
					     .same_va = (im.out.flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP)) != 0 };
			rg.len = (size_t)(rg.va_pages * (uint64_t)g_page);
			jok(1); jx("flags_out", im.out.flags); jx("gpu_va_or_cookie", im.out.gpu_va); ju("va_pages", im.out.va_pages);
			jbool("same_va", rg.same_va); jbool("need_mmap", (im.out.flags & BASE_MEM_NEED_MMAP) != 0);
			/* UMM imports are GPU-mapped at import time unless IMPORT_SYNC_ON_MAP_UNMAP is set
			 * (then a KCPU MAP_IMPORT command is needed; not exercised here). */
			int prot = prot_for(im.out.flags);
			if (prot && rg.len) {
				void *m = mmap(NULL, rg.len, prot, MAP_SHARED, g_fd, (off_t)im.out.gpu_va);
				if (m == MAP_FAILED) { jbool("cpu_mmap_ok", 0); ji("cpu_mmap_errno", errno); }
				else {
					rg.map = m; jbool("cpu_mmap_ok", 1);
					if (prot & PROT_READ) {
						g_guard = 1;
						int sg = sigsetjmp(g_jmp, 1);
						if (sg == 0) { volatile unsigned char x = *(volatile unsigned char *)m; (void)x; jbool("cpu_read_ok", 1); }
						else { jbool("cpu_read_ok", 0); ji("cpu_read_signal", sg); }
						g_guard = 0;
					}
				}
			}
			int mapped = rg.map != NULL;
			report_query(&rg, mapped);
			report_free(&rg, mapped);
			jend();
		}
	}
}

/* ------------------------------------------------------------ step 6: sync */
static void step_sync(void)
{
	/* KCPU queue: fence signal command gives a sync_file fd without touching the GPU. */
	if (!fd_ready()) jskip("kcpu_fence", "no handshake");
	else if (g_csf != 1) jskip("kcpu_fence", "not a CSF driver");
	else {
		struct kbase_ioctl_kcpu_queue_new qn;
		memset(&qn, 0, sizeof qn);
		jb("kcpu_fence");
		if (ioctl(g_fd, KBASE_IOCTL_KCPU_QUEUE_CREATE, &qn) != 0) {
			int e = errno; jok(0); jstr("phase", "queue_create"); jerr(e); jend();
		} else {
			ju("queue_id", qn.id);
			struct base_fence f = { .basep = { .fd = -1, .stream_fd = -1 } };
			struct base_kcpu_command cmd;
			memset(&cmd, 0, sizeof cmd);
			cmd.type = BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL;
			cmd.info.fence = (uint64_t)(uintptr_t)&f;
			struct kbase_ioctl_kcpu_queue_enqueue en;
			memset(&en, 0, sizeof en);
			en.addr = (uint64_t)(uintptr_t)&cmd; en.nr_commands = 1; en.id = qn.id;
			int r = ioctl(g_fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &en);
			int e = errno;
			if (r != 0) { jok(0); jstr("phase", "enqueue_fence_signal"); jerr(e); }
			else {
				jok(f.basep.fd >= 0); ji("sync_file_fd", f.basep.fd); ji("stream_fd", f.basep.stream_fd);
				if (f.basep.fd >= 0) {
					char link[64], tgt[128];
					snprintf(link, sizeof link, "/proc/self/fd/%d", f.basep.fd);
					ssize_t n = readlink(link, tgt, sizeof tgt - 1);
					if (n > 0) { tgt[n] = 0; jstr("fd_target", tgt); }
					struct pollfd pf = { .fd = f.basep.fd, .events = POLLIN };
					int pr = poll(&pf, 1, 100);
					jbool("signalled_within_100ms", pr == 1 && (pf.revents & POLLIN));
					close(f.basep.fd);
				}
				if (f.basep.stream_fd >= 0) close(f.basep.stream_fd);
			}
			struct kbase_ioctl_kcpu_queue_delete qd;
			memset(&qd, 0, sizeof qd); qd.id = qn.id;
			int dr = ioctl(g_fd, KBASE_IOCTL_KCPU_QUEUE_DELETE, &qd);
			jbool("queue_delete_ok", dr == 0);
			jend();
		}
	}

	/* sw_sync timeline (generic kernel facility) */
	static const char *const sw[] = { "/dev/sw_sync", "/sys/kernel/debug/sync/sw_sync" };
	for (size_t i = 0; i < 2; i++) {
		jb("sw_sync"); jstr("path", sw[i]);
		int tfd = open(sw[i], O_RDWR | O_CLOEXEC);
		if (tfd < 0) { int e = errno; jok(0); jstr("phase", "open"); jerr(e); jend(); continue; }
		struct sw_sync_create_fence_data_min cf;
		memset(&cf, 0, sizeof cf);
		cf.value = 1; snprintf(cf.name, sizeof cf.name, "probe");
		cf.fence = -1;
		int r = ioctl(tfd, SW_SYNC_IOC_CREATE_FENCE_MIN, &cf);
		int e = errno;
		if (r != 0) { jok(0); jstr("phase", "create_fence"); jerr(e); close(tfd); jend(); continue; }
		struct pollfd pf = { .fd = cf.fence, .events = POLLIN };
		int before = poll(&pf, 1, 0);
		uint32_t one = 1;
		int ir = ioctl(tfd, SW_SYNC_IOC_INC_MIN, &one);
		pf.revents = 0;
		int after = poll(&pf, 1, 50);
		jok(ir == 0 && before == 0 && after == 1);
		jbool("pending_before_inc", before == 0); jbool("signalled_after_inc", after == 1);
		close(cf.fence); close(tfd);
		jend();
	}
}

/* ------------------------------------------------------------ step 7: CSF basics + misc info */
static void step_csf(void)
{
	if (!fd_ready()) { jskip("csf_glb_iface", "no handshake"); }
	else if (g_csf != 1) { jskip("csf_glb_iface", "not a CSF driver"); }
	else {
		union kbase_ioctl_cs_get_glb_iface gi;
		memset(&gi, 0, sizeof gi);
		int r = ioctl(g_fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &gi);
		int e = errno;
		jb("csf_glb_iface"); jok(r == 0);
		if (r != 0) jerr(e);
		else {
			uint32_t gn = gi.out.group_num, sn = gi.out.total_stream_num;
			jx("glb_version", gi.out.glb_version); jx("features", gi.out.features);
			ju("group_num", gn); ju("total_stream_num", sn);
			jx("prfcnt_size", gi.out.prfcnt_size); jx("instr_features", gi.out.instr_features);
			if (gn && gn <= 64 && sn && sn <= 1024) {
				struct basep_cs_group_control *g = calloc(gn, sizeof *g);
				struct basep_cs_stream_control *s = calloc(sn, sizeof *s);
				if (g && s) {
					union kbase_ioctl_cs_get_glb_iface g2;
					memset(&g2, 0, sizeof g2);
					g2.in.max_group_num = gn; g2.in.max_total_stream_num = sn;
					g2.in.groups_ptr = (uint64_t)(uintptr_t)g; g2.in.streams_ptr = (uint64_t)(uintptr_t)s;
					if (ioctl(g_fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &g2) == 0) {
						ju("group0_stream_num", g[0].stream_num); jx("group0_features", g[0].features);
						ju("group0_suspend_size", g[0].suspend_size); jx("stream0_features", s[0].features);
						unsigned maxs = 0, mins = ~0u;
						for (uint32_t i = 0; i < gn; i++) { if (g[i].stream_num > maxs) maxs = g[i].stream_num; if (g[i].stream_num < mins) mins = g[i].stream_num; }
						ju("streams_per_group_min", mins); ju("streams_per_group_max", maxs);
					} else { ji("fill_errno", errno); }
				}
				free(g); free(s);
			}
		}
		jend();
	}

	if (!fd_ready()) { jskip("ddk_version", "no handshake"); jskip("context_id", "no handshake");
			   jskip("cpu_gpu_timeinfo", "no handshake"); jskip("context_priority_check", "no handshake"); return; }
	{
		char vb[256];
		memset(vb, 0, sizeof vb);
		struct kbase_ioctl_get_ddk_version dv = { .version_buffer = (uint64_t)(uintptr_t)vb, .size = sizeof vb - 1 };
		int r = ioctl(g_fd, KBASE_IOCTL_GET_DDK_VERSION, &dv);
		int e = errno;
		jb("ddk_version"); jok(r >= 0);
		if (r >= 0) jstr("version", vb); else jerr(e);
		jend();
	}
	{
		struct kbase_ioctl_get_context_id ci = { 0 };
		int r = ioctl(g_fd, KBASE_IOCTL_GET_CONTEXT_ID, &ci);
		int e = errno;
		jb("context_id"); jok(r == 0);
		if (r == 0) ju("id", ci.id); else jerr(e);
		jend();
	}
	{
		union kbase_ioctl_get_cpu_gpu_timeinfo ti;
		memset(&ti, 0, sizeof ti);
		ti.in.request_flags = BASE_TIMEINFO_MONOTONIC_FLAG | BASE_TIMEINFO_TIMESTAMP_FLAG;
		int r = ioctl(g_fd, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &ti);
		int e = errno;
		jb("cpu_gpu_timeinfo"); jok(r == 0);
		if (r == 0) { ju("sec", ti.out.sec); ju("nsec", ti.out.nsec); ju("gpu_timestamp", ti.out.timestamp); }
		else jerr(e);
		jend();
	}
	for (unsigned p = 0; p < 3; p++) {
		struct kbase_ioctl_context_priority_check pc = { .priority = (uint8_t)p };
		int r = ioctl(g_fd, KBASE_IOCTL_CONTEXT_PRIORITY_CHECK, &pc);
		int e = errno;
		jb("context_priority_check"); ju("requested", p); jok(r == 0);
		if (r == 0) ju("granted", pc.priority); else jerr(e);
		jend();
	}
}

/* ------------------------------------------------------------ step 8: syscall timing */
static void report_timing(const char *name, uint64_t *s, size_t n, const char *how)
{
	qsort(s, n, sizeof *s, cmp_u64);
	double sum = 0;
	for (size_t i = 0; i < n; i++) sum += (double)s[i];
	jb("syscall_timing"); jstr("what", name); jstr("how", how); jok(1); ju("n", n);
	jd("mean_ns", sum / (double)n); ju("min_ns", s[0]); ju("p50_ns", s[n / 2]);
	ju("p99_ns", s[(n * 99) / 100]); ju("max_ns", s[n - 1]);
	jend();
}

static void step_timing(void)
{
	enum { N = 10000 };
	uint64_t *s = malloc(N * sizeof *s);
	if (!s) { jb("syscall_timing"); jok(0); jerr(ENOMEM); jend(); return; }
	for (int i = 0; i < N; i++) { uint64_t a = now_ns(); uint64_t b = now_ns(); s[i] = b - a; }
	report_timing("clock_gettime_pair_overhead", s, N, "back-to-back clock_gettime");
	for (int i = 0; i < N; i++) { uint64_t a = now_ns(); (void)syscall(SYS_getppid); uint64_t b = now_ns(); s[i] = b - a; }
	report_timing("getppid", s, N, "raw syscall, includes clock overhead");
	if (fd_ready()) {
		struct kbase_ioctl_get_context_id ci;
		int fails = 0;
		for (int i = 0; i < N; i++) {
			uint64_t a = now_ns();
			int r = ioctl(g_fd, KBASE_IOCTL_GET_CONTEXT_ID, &ci);
			uint64_t b = now_ns();
			if (r != 0) fails++;
			s[i] = b - a;
		}
		report_timing("kbase_get_context_id", s, N, "ioctl, includes clock overhead");
		jb("syscall_timing_failures"); jok(fails == 0); ji("failed_ioctls", fails); jend();
	} else {
		jskip("syscall_timing_kbase_get_context_id", "no handshake");
	}
	free(s);
}

/* ------------------------------------------------------------ main */
int main(void)
{
	install_guards();
	setvbuf(stdout, NULL, _IOLBF, 0);
	g_page = sysconf(_SC_PAGESIZE);
	if (g_page <= 0) g_page = 4096;

	jb("probe_info"); jok(1); jstr("version", "0.1"); ju("page_size", (ull)g_page);
	ju("pointer_bits", sizeof(void *) * 8);
	jend();

	step_nodes();
	step_open();
	step_handshake();
	step_gpuprops();
	step_memory();
	step_dma_heaps();
	step_udmabuf();
	step_import();
	step_sync();
	step_csf();
	step_timing();

	for (int i = 0; i < g_ndmabuf; i++) close(g_dmabuf[i].fd);
	if (g_fd >= 0) close(g_fd);
	jb("done"); jok(1); jend();
	return 0;
}
