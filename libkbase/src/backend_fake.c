// SPDX-License-Identifier: MIT
/* Fake kbase backend ("KB_FAKE" mode) for offline stream/descriptor checking. NO GPU, NO phone.
 *
 * Linked ONLY into out-fake test binaries (see check.sh). It defines open/ioctl/mmap/munmap so the real libkbase
 * sources (ctx.c, bo.c, queue.c, ...) run unmodified against heap memory with deterministic fake GPU VAs
 * (bump allocator from 0x5f00000000, never reused, like a SAME_VA zone). The "GPU" never executes anything:
 * CS_QUEUE_KICK snapshots the new ring words plus every live region to $KB_FAKE_DUMP (JSON lines) and sets
 * CS_EXTRACT = CS_INSERT; fences/KCPU/dma-buf sync files are always already signalled.
 *
 * Env: KB_FAKE_DUMP=path (required, else no dump)   KB_FAKE_ARCH=11|13 (default 11; sets gpu_id + uAPI 1.21/1.30)
 *      KB_FAKE_MUTATE=idvs38|pixfmt688 (re-introduce an old bug into the captured stream, to prove the checker catches it;
 *      the test itself and the real backend are untouched).
 * Not modelled: any execution, cache behaviour, MMU, timing, readback contents (tests will report failures; ignore them). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kbase_uapi.h"
#include <stdbool.h>

#define FAKE_BASE 0x5f00000000ull
#define MAXFD 4096
enum { FD_NONE = 0, FD_MALI, FD_HEAP, FD_DMABUF };
static unsigned char fdt[MAXFD];

enum { R_BO = 1, R_IMPORT, R_USERIO, R_HEAPCTX, R_CHUNK, R_TRACK };
/* shadow: the fake "GPU cache" copy of a BO, see kb_fake_gpu_write() */
struct region { uint64_t va; size_t len; int kind; uint32_t flags; int live; int q; uint64_t freed_seq; void *cpu; void *shadow; };
static struct region reg[1024];
static int nreg;
static uint64_t next_va = FAKE_BASE, next_exec = 0x20000000ull, cookie_n;
struct pend { uint64_t cookie, size, flags; int kind; int used; };
static struct pend pend[1024];
static int npend;
struct fq { uint64_t ring_va; uint32_t size; int bound; uint64_t prev_insert; void *io; uint64_t io_cookie; };
static struct fq fq[16];
static int nfq, ngroup;
static unsigned arch = 11;
static FILE *dump;
static unsigned kick_no;
static const char *mutate;

static long rsys(long n, long a, long b, long c, long d, long e, long f) { return syscall(n, a, b, c, d, e, f); }
static void *real_mmap(void *a, size_t l, int p, int f, int fd, off_t o) { return (void *)rsys(SYS_mmap, (long)a, (long)l, p, f, fd, o); }

static void init_once(void)
{
	static int done;
	if (done) return;
	done = 1;
	const char *a = getenv("KB_FAKE_ARCH");
	if (a) arch = (unsigned)atoi(a);
	mutate = getenv("KB_FAKE_MUTATE");
	const char *d = getenv("KB_FAKE_DUMP");
	if (d) dump = fopen(d, "w");
}

static struct region *find(uint64_t va)
{
	for (int i = 0; i < nreg; i++) if (reg[i].live && va >= reg[i].va && va < reg[i].va + reg[i].len) return &reg[i];
	return NULL;
}

static void *new_region(size_t len, int kind, uint32_t flags, int exec)
{
	len = (len + 4095) & ~(size_t)4095;
	uint64_t va = exec ? next_exec : next_va;
	void *p = real_mmap(exec ? NULL : (void *)(uintptr_t)va, len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | (exec ? 0 : MAP_FIXED_NOREPLACE), -1, 0);
	if (p == MAP_FAILED || (long)p < 0) return MAP_FAILED;
	if (exec) { next_exec += (len + 0xfffff) & ~0xfffffull; }
	else next_va += len + 4096; /* guard page; VAs never reused */
	if (nreg < 1024) reg[nreg++] = (struct region){ exec ? va : (uint64_t)(uintptr_t)p, len, kind, flags, 1, -1, 0, p, NULL };
	return p;
}

static void emit_hex(const uint8_t *p, size_t n)
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { fputc(d[p[i] >> 4], dump); fputc(d[p[i] & 15], dump); }
}

static void do_kick(uint64_t ring_va)
{
	int qi = -1;
	for (int i = 0; i < nfq; i++) if (fq[i].ring_va == ring_va) qi = i;
	if (qi < 0 || !fq[qi].io) return;
	volatile uint64_t *ins = (volatile uint64_t *)((char *)fq[qi].io + 4096), *ext = (volatile uint64_t *)((char *)fq[qi].io + 8192);
	uint64_t cur = *ins, prev = fq[qi].prev_insert;
	struct region *rr = find(ring_va);
	kick_no++;
	if (dump && rr && cur > prev) {
		fprintf(dump, "{\"ev\":\"kick\",\"n\":%u,\"q\":%d}\n", kick_no, qi);
		/* regions first (the analyzer needs them to follow CALLs), non-zero 4 KiB pages only, skipping the kernel-owned kinds */
		for (int i = 0; i < nreg; i++) {
			struct region *r = &reg[i];
			if (!r->live || r->kind == R_USERIO || r->kind == R_TRACK || r->kind == R_IMPORT) continue;
			fprintf(dump, "{\"ev\":\"region\",\"va\":\"0x%llx\",\"len\":%zu,\"kind\":%d,\"flags\":%u}\n", (unsigned long long)r->va, r->len, r->kind, r->flags);
			for (size_t o = 0; o < r->len; o += 4096) {
				const uint8_t *pg = (const uint8_t *)r->cpu + o;
				size_t k = 0;
				while (k < 4096 && !pg[k]) k++;
				if (k == 4096) continue;
				uint8_t cp[4096];
				memcpy(cp, pg, 4096);
				if (mutate && !strcmp(mutate, "pixfmt688")) /* old bug: blend conversion format = attribute pixfmt 0x000bb688 instead of 0x000ed000 */
					for (size_t j = 0; j < 4096; j += 4) { uint32_t v; memcpy(&v, cp + j, 4); if (v == 0x000ed000u) { v = 0x000bb688u; memcpy(cp + j, &v, 4); } }
				fprintf(dump, "{\"ev\":\"mem\",\"va\":\"0x%llx\",\"hex\":\"", (unsigned long long)(r->va + o));
				emit_hex(cp, 4096);
				fputs("\"}\n", dump);
			}
		}
		for (int i = 0; i < nreg; i++)
			if (!reg[i].live && reg[i].kind != R_USERIO && reg[i].kind != R_TRACK && reg[i].kind != R_IMPORT)
				fprintf(dump, "{\"ev\":\"freed\",\"va\":\"0x%llx\",\"len\":%zu,\"seq\":%llu}\n", (unsigned long long)reg[i].va, reg[i].len, (unsigned long long)reg[i].freed_seq);
		size_t n = (size_t)((cur - prev) / 8), mask = fq[qi].size - 1;
		uint64_t *w = malloc(n * 8);
		for (size_t i = 0; i < n; i++) w[i] = ((uint64_t *)rr->cpu)[((prev + i * 8) & mask) / 8];
		for (size_t i = 0; mutate && i < n; i++) { /* optional bug re-introduction for checker self-test */
			if (!strcmp(mutate, "idvs38") && (w[i] >> 56) == 6) w[i] &= ~(1ull << 38);
		}
		fprintf(dump, "{\"ev\":\"ring\",\"va\":\"0x%llx\",\"size\":%u,\"start\":%llu,\"insert\":%llu,\"hex\":\"", (unsigned long long)ring_va, fq[qi].size, (unsigned long long)prev, (unsigned long long)cur);
		emit_hex((const uint8_t *)w, n * 8);
		fputs("\"}\n", dump);
		free(w);
		fflush(dump);
	}
	fq[qi].prev_insert = cur;
	*ext = cur; /* the fake GPU "finishes" instantly */
}

int open(const char *path, int flags, ...)
{
	init_once();
	mode_t m = 0;
	if (flags & O_CREAT) { va_list ap; va_start(ap, flags); m = (mode_t)va_arg(ap, int); va_end(ap); }
	int kind = FD_NONE;
	if (!strncmp(path, "/dev/mali", 9)) kind = FD_MALI;
	else if (!strncmp(path, "/dev/dma_heap/", 14)) kind = FD_HEAP;
	if (kind) {
		int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); /* never signalled: poll() on the "kbase fd" times out like an idle GPU */
		if (fd >= 0 && fd < MAXFD) fdt[fd] = (unsigned char)kind;
		return fd;
	}
	long r = rsys(SYS_openat, AT_FDCWD, (long)path, flags, m, 0, 0);
	if (r < 0) { errno = (int)-r; return -1; }
	return (int)r;
}

int close(int fd)
{
	if (fd >= 0 && fd < MAXFD) fdt[fd] = 0;
	long r = rsys(SYS_close, fd, 0, 0, 0, 0, 0);
	if (r < 0) { errno = (int)-r; return -1; }
	return 0;
}

static int mkprops(void *buf, size_t cap)
{
	uint8_t b[512];
	size_t o = 0;
	struct { unsigned key; uint64_t v; } kv[] = {
		{ KBASE_GPUPROP_PRODUCT_ID, arch >= 13 ? 0xd800 : 0xb803 }, { KBASE_GPUPROP_MAX_THREADS, 2048 }, { KBASE_GPUPROP_MAX_REGISTERS, 65536 },
		{ KBASE_GPUPROP_RAW_SHADER_PRESENT, arch >= 13 ? 0xee0057 : 0x5 }, { KBASE_GPUPROP_RAW_TILER_PRESENT, 1 }, { KBASE_GPUPROP_RAW_L2_PRESENT, 1 },
		{ KBASE_GPUPROP_RAW_MMU_FEATURES, 0x2830 }, { KBASE_GPUPROP_RAW_AS_PRESENT, 0xff },
		{ KBASE_GPUPROP_RAW_GPU_ID, arch >= 13 ? 0xd8000000u : 0xb8030000u }, { KBASE_GPUPROP_COHERENCY_NUM_GROUPS, 1 },
	};
	for (size_t i = 0; i < sizeof kv / sizeof *kv; i++) {
		uint32_t h = (kv[i].key << 2) | 3;
		memcpy(b + o, &h, 4); o += 4;
		memcpy(b + o, &kv[i].v, 8); o += 8;
	}
	if (buf && cap >= o) memcpy(buf, b, o);
	return (int)o;
}

static void *map_new_bo(uint64_t flags_unused, size_t len, uint32_t kind, uint32_t fl, int exec) { (void)flags_unused; return new_region(len, (int)kind, fl, exec); }

/* ---- incoherence model for the MEM_SYNC policy tests (t_memsync.c) ----
 * The fake kernel is otherwise fully coherent, so a missing MEM_SYNC cannot be observed. kb_fake_gpu_write()
 * creates a shadow copy of a BO on first use (initialised from the CPU mapping) and writes into the shadow:
 * the CPU mapping keeps the old bytes until MEM_SYNC type 2 (device->CPU) copies shadow -> mapping. MEM_SYNC
 * type 1 (CPU->device) copies mapping -> shadow. A skipped sync therefore reads as stale data. */
static struct region *find_region(uint64_t va)
{
	for (int i = 0; i < nreg; i++)
		if (reg[i].live && reg[i].va == va)
			return &reg[i];
	return NULL;
}

int kb_fake_gpu_write(uint64_t va, size_t off, const void *src, size_t len)
{
	struct region *r = find_region(va);
	if (!r || !r->cpu || off + len > r->len) return -1;
	if (!r->shadow) {
		r->shadow = real_mmap(NULL, r->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (r->shadow == MAP_FAILED) { r->shadow = NULL; return -1; }
		memcpy(r->shadow, r->cpu, r->len);
	}
	memcpy((uint8_t *)r->shadow + off, src, len);
	return 0;
}

bool kb_fake_incoherent(uint64_t va) { struct region *r = find_region(va); return r && r->shadow; }

struct synccnt { uint64_t va; unsigned n; };
/* per-VA MEM_SYNC counts: open-addressing hash (t_synclists syncs thousands of distinct BOs) */
#define NSYNC_SLOTS 65536u
static struct synccnt syncs[NSYNC_SLOTS];
static struct synccnt *sync_slot(uint64_t va, bool add)
{
	for (unsigned h = (unsigned)((va >> 12) * 2654435761u) & (NSYNC_SLOTS - 1), n = 0; n < NSYNC_SLOTS; n++, h = (h + 1) & (NSYNC_SLOTS - 1)) {
		if (syncs[h].va == va) return &syncs[h];
		if (!syncs[h].va) { if (!add) return NULL; syncs[h].va = va; return &syncs[h]; }
	}
	return NULL;
}
static unsigned n_mem_alloc, n_mem_free;
unsigned kb_fake_mem_alloc_count(void) { return n_mem_alloc; }   /* test hooks (t_bopool.c) */
unsigned kb_fake_mem_free_count(void) { return n_mem_free; }
unsigned kb_fake_memsync_count(uint64_t va);   /* test hook (t_memsync.c) */
unsigned kb_fake_memsync_count(uint64_t va) { struct synccnt *c = sync_slot(va, false); return c ? c->n : 0; }

int ioctl(int fd, unsigned long req, ...)
{
	init_once();
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	unsigned type = _IOC_TYPE(req), nr = _IOC_NR(req), sz = _IOC_SIZE(req);
	if (fd < 0 || fd >= MAXFD || !fdt[fd]) {
		long r = rsys(SYS_ioctl, fd, (long)req, (long)arg, 0, 0, 0);
		if (r < 0) { errno = (int)-r; return -1; }
		return (int)r;
	}
	if (fdt[fd] == FD_HEAP) { /* dma-heap alloc: memfd */
		struct kb_dma_heap_alloc *a = arg;
		int m = (int)rsys(SYS_memfd_create, (long)"fake-dmabuf", 1 /*CLOEXEC*/, 0, 0, 0, 0);
		if (m < 0 || ftruncate(m, (off_t)a->len) < 0) { errno = ENOMEM; return -1; }
		fdt[m] = FD_DMABUF;
		a->fd = (uint32_t)m;
		return 0;
	}
	if (fdt[fd] == FD_DMABUF) { /* DMA_BUF_IOCTL_SYNC / import / export sync_file */
		if (nr == 2 && type == 'b') {
			struct kb_dma_buf_export_sync_file *e = arg;
			e->fd = eventfd(1, EFD_CLOEXEC);
		}
		return 0;
	}
	errno = 0;
	if (type != 0x80) { errno = ENOTTY; return -1; }
#define OK return 0
	switch (nr) {
	case 52: { struct kbase_ioctl_version_check *v = arg; v->major = 1; v->minor = arch >= 13 ? 30 : 21; OK; }
	case 0: errno = ENOTTY; return -1;
	case 1: case 14: case 38: OK;
	case 3: { struct kbase_ioctl_get_gpuprops *g = arg; int n = mkprops((void *)(uintptr_t)g->buffer, g->size); if (g->size == 0) return n; return n; }
	case 13: { struct kbase_ioctl_get_ddk_version *d = arg; if (d->version_buffer && d->size) snprintf((char *)(uintptr_t)d->version_buffer, d->size, "fake-ddk"); return 8; }
	case 51: {
		union kbase_ioctl_cs_get_glb_iface *g = arg;
		if (g->in.groups_ptr) {
			struct basep_cs_group_control *gc = (void *)(uintptr_t)g->in.groups_ptr;
			for (uint32_t i = 0; i < g->in.max_group_num; i++) { gc[i].stream_num = 4; gc[i].suspend_size = 4096; }
			OK;
		}
		memset(&g->out, 0, sizeof g->out);
		g->out.group_num = 4; g->out.total_stream_num = 16; g->out.glb_version = 0x04010000;
		OK;
	}
	case 59: {
		union kbase_ioctl_mem_alloc_ex *m = arg;
		n_mem_alloc++;
		uint64_t fl = m->in.flags, pages = m->in.va_pages;
		int same = (fl & BASE_MEM_SAME_VA) != 0;
		struct pend *p = &pend[npend++ % 1024];
		p->size = pages * 4096; p->flags = fl; p->kind = R_BO; p->used = 0;
		p->cookie = same ? ((0x1000 + ++cookie_n) << 12) : next_exec;
		m->out.flags = fl; m->out.gpu_va = p->cookie;
		OK;
	}
	case 7: { struct kbase_ioctl_mem_free *f = arg; struct region *r = find(f->gpu_addr); if (!r) { errno = EINVAL; return -1; } r->live = 0; r->freed_seq = kick_no; OK; }
	case 6: { union kbase_ioctl_mem_query *q = arg; if (!find(q->in.gpu_addr)) { errno = EINVAL; return -1; } q->out.value = 0; OK; }
	case 22: { union kbase_ioctl_mem_import *m = arg; struct pend *p = &pend[npend++ % 1024]; p->size = 0; p->kind = R_IMPORT; p->used = 0; p->cookie = (0x1000 + ++cookie_n) << 12; p->flags = (uint32_t)m->in.flags; uint64_t fl_ = m->in.flags; int ifd = *(int *)(uintptr_t)m->in.phandle; m->out.flags = fl_; m->out.gpu_va = p->cookie; { struct stat st; m->out.va_pages = fstat(ifd, &st) == 0 ? (uint64_t)st.st_size / 4096 : 1; } OK; }
	case 36: { struct kbase_ioctl_cs_queue_register *r = arg; if (nfq < 16) { fq[nfq].ring_va = r->buffer_gpu_addr; fq[nfq].size = r->buffer_size; nfq++; } OK; }
	case 58: { union kbase_ioctl_cs_queue_group_create *g = arg; g->out.group_handle = (uint8_t)ngroup; g->out.group_uid = 100u + (uint32_t)ngroup; ngroup++; OK; }
	case 39: {
		union kbase_ioctl_cs_queue_bind *b = arg;
		for (int i = 0; i < nfq; i++) if (fq[i].ring_va == b->in.buffer_gpu_addr) { fq[i].bound = 1; fq[i].io_cookie = (0x2000 + (uint64_t)i) << 12; b->out.mmap_handle = fq[i].io_cookie; OK; }
		errno = EINVAL; return -1;
	}
	case 37: { struct kbase_ioctl_cs_queue_kick *k = arg; do_kick(k->buffer_gpu_addr); OK; }
	case 41: case 43: case 49: case 46: OK;
	case 48: {
		uint64_t *o = arg; /* both layouts: out at offset 0 */
		uint32_t chunk = *(uint32_t *)arg;
		void *h = new_region(4096, R_HEAPCTX, 0, 0);
		void *c = new_region(chunk ? chunk : 4096, R_CHUNK, 0, 0);
		if (h == MAP_FAILED || c == MAP_FAILED) { errno = ENOMEM; return -1; }
		o[0] = (uint64_t)(uintptr_t)h; o[1] = (uint64_t)(uintptr_t)c; (void)sz;
		OK;
	}
	case 45: { struct kbase_ioctl_kcpu_queue_new *n = arg; n->id = 0; OK; }
	case 47: {
		struct kbase_ioctl_kcpu_queue_enqueue *e = arg;
		struct base_kcpu_command *c = (void *)(uintptr_t)e->addr;
		for (uint32_t i = 0; i < e->nr_commands; i++)
			if (c[i].type == BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL) {
				struct base_fence *f = (void *)(uintptr_t)c[i].info.fence;
				f->fd = eventfd(1, EFD_CLOEXEC);
			}
		OK;
	}
	case 15: { /* MEM_SYNC: counted per gpu_va for tests (kb_fake_memsync_count) */
		struct { uint64_t handle, user_addr, size; uint8_t type; } *m = arg;
		struct region *r = find_region(m->handle);
		if (r && r->shadow && r->cpu) {
			/* type 1 = CPU -> device (clean), type 2 = device -> CPU (invalidate) */
			if (m->type == 2) memcpy(r->cpu, r->shadow, r->len);
			else if (m->type == 1) memcpy(r->shadow, r->cpu, r->len);
		}
		{ struct synccnt *c = sync_slot(m->handle, true); if (c) c->n++; }
		OK;
	}
	default: errno = ENOTTY; return -1;
	}
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	init_once();
	if (fd >= 0 && fd < MAXFD && fdt[fd] == FD_MALI) {
		if ((uint64_t)off == BASE_MEM_MAP_TRACKING_HANDLE) return real_mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		for (int i = 0; i < nfq; i++)
			if (fq[i].io_cookie == (uint64_t)off) { void *p = new_region(len, R_USERIO, 0, 0); if (p != MAP_FAILED) fq[i].io = p; return p; }
		for (int i = 0; i < npend && i < 1024; i++) {
			struct pend *p = &pend[i];
			if (p->used || p->cookie != (uint64_t)off) continue;
			p->used = 1;
			if (p->kind == R_IMPORT) return new_region(len, R_IMPORT, 0, 0);
			return map_new_bo(0, len, R_BO, (uint32_t)p->flags, !(p->flags & BASE_MEM_SAME_VA));
		}
		errno = EINVAL;
		return MAP_FAILED;
	}
	void *r = real_mmap(addr, len, prot, flags, fd, off);
	if ((long)r < 0 && (long)r > -4096) { errno = (int)-(long)r; return MAP_FAILED; }
	return r;
}

int munmap(void *addr, size_t len)
{
	for (int i = 0; i < nreg; i++)
		if (reg[i].live && reg[i].cpu == addr) { n_mem_free++; reg[i].live = 0; reg[i].freed_seq = kick_no; break; }
	long r = rsys(SYS_munmap, (long)addr, (long)len, 0, 0, 0, 0);
	if (r < 0) { errno = (int)-r; return -1; }
	return 0;
}
