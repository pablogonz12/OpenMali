/* SPDX-License-Identifier: MIT */
/* Layer A: context, handshake, GPU properties, capability table. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"

static _Thread_local char g_err[256];

void kb_set_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_err, sizeof g_err, fmt, ap);
	va_end(ap);
}
const char *kb_last_error(void) { return g_err; }

#include <time.h>
static struct { unsigned long req; unsigned long n; double ms; } g_ioprof[64];
static int g_ioprof_on = -1;
static void ioprof_dump(void)
{
	for (int i = 0; i < 64 && g_ioprof[i].n; i++)
		fprintf(stderr, "IOPROF type=%c nr=%lu n=%lu total_ms=%.3f avg_us=%.1f\n", (char)_IOC_TYPE(g_ioprof[i].req) >= 32 ? (char)_IOC_TYPE(g_ioprof[i].req) : '?',
			(unsigned long)_IOC_NR(g_ioprof[i].req), g_ioprof[i].n, g_ioprof[i].ms, g_ioprof[i].ms * 1000.0 / g_ioprof[i].n);
}

int kb_ioctl_fd(int fd, unsigned long req, void *arg)
{
	int r;
	if (g_ioprof_on < 0) { g_ioprof_on = getenv("MALI_IOCTL_PROF") != NULL; if (g_ioprof_on) atexit(ioprof_dump); }
	struct timespec t0, t1;
	if (g_ioprof_on) clock_gettime(CLOCK_MONOTONIC, &t0);
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	int e = errno;
	if (g_ioprof_on) {
		clock_gettime(CLOCK_MONOTONIC, &t1);
		int i = 0;
		while (i < 63 && g_ioprof[i].n && g_ioprof[i].req != req) i++;
		g_ioprof[i].req = req; g_ioprof[i].n++;
		g_ioprof[i].ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
	}
	errno = e;
	return r;
}

int kb_ioctl(struct kb_ctx *ctx, unsigned long req, void *arg)
{
	int r = kb_ioctl_fd(ctx->fd, req, arg);
	return r < 0 ? -errno : 0;
}

/* Capability table. Add a row only after the layouts above were verified on that kernel. */
static const struct kb_caps k_caps[] = {
	{ 1, 21, "CMF Phone 1 (MT6878, Mali-G615 MC2), DDK r44p1", true, true, true, true, false, false, false },
	/* Poco X4 GT (MT6895, Mali-G610 MC6), DDK r32p1, uAPI 1.18: probed, but the layouts below are guesses (conservative: 40 B group create,
	 * no CQS operations, no MEM_ALLOC_EX) until a ladder run on the phone confirms them. */
	{ 1, 18, "Poco X4 GT (MT6895, Mali-G610 MC6), DDK r32p1 [layouts verified by t_submit/clear/triangle/uniform/depth/texture]", false, true, false, true, false, false, true },
	/* Poco X8 Pro Max (MT6991, Mali-G925 MC11), DDK r49p1. Layouts from Arm public headers, UNVERIFIED on hardware (see kbase_uapi.h). */
	{ 1, 30, "Poco X8 Pro Max (MT6991, Immortalis-G925 MC11), DDK r49p1 [layouts unverified]", true, false, true, true, true, true, false },
};

const struct kb_caps *kb_caps_lookup(unsigned major, unsigned minor)
{
	for (size_t i = 0; i < sizeof k_caps / sizeof *k_caps; i++)
		if (k_caps[i].major == major && k_caps[i].minor == minor)
			return &k_caps[i];
	return NULL;
}

/* gpuprops blob: repeated { u32 hdr = key << 2 | size_code ; value of 1<<size_code bytes } (little endian). */
static void parse_props(const uint8_t *p, size_t n, struct kb_gpu_props *g)
{
	size_t o = 0;
	while (o + 4 <= n) {
		uint32_t h;
		memcpy(&h, p + o, 4);
		o += 4;
		unsigned key = h >> 2, sz = 1u << (h & 3);
		if (o + sz > n)
			break;
		uint64_t v = 0;
		for (unsigned i = 0; i < sz; i++)
			v |= (uint64_t)p[o + i] << (8 * i);
		o += sz;
		switch (key) {
		case KBASE_GPUPROP_PRODUCT_ID: g->product_id = (uint32_t)v; break;
		case KBASE_GPUPROP_MINOR_REVISION: g->revision_minor = (unsigned)v; break;
		case KBASE_GPUPROP_MAJOR_REVISION: g->revision_major = (unsigned)v; break;
		case KBASE_GPUPROP_GPU_FREQ_KHZ_MAX: g->gpu_freq_khz_max = (uint32_t)v; break;
		case KBASE_GPUPROP_MAX_THREADS: g->max_threads = (uint32_t)v; break;
		case KBASE_GPUPROP_MAX_REGISTERS: g->max_registers = (uint32_t)v; break;
		case KBASE_GPUPROP_RAW_SHADER_PRESENT: g->shader_present = v; break;
		case KBASE_GPUPROP_RAW_TILER_PRESENT: g->tiler_present = v; break;
		case KBASE_GPUPROP_RAW_L2_PRESENT: g->l2_present = v; break;
		case KBASE_GPUPROP_RAW_STACK_PRESENT: g->stack_present = v; break;
		case KBASE_GPUPROP_RAW_L2_FEATURES: g->l2_features = (uint32_t)v; break;
		case KBASE_GPUPROP_RAW_MEM_FEATURES: g->mem_features = (uint32_t)v; break;
		case KBASE_GPUPROP_RAW_MMU_FEATURES: g->mmu_features = (uint32_t)v; break;
		case KBASE_GPUPROP_RAW_AS_PRESENT: g->as_present = (uint32_t)v; break;
		case KBASE_GPUPROP_RAW_TILER_FEATURES: g->tiler_features = (uint32_t)v; break;
		case KBASE_GPUPROP_RAW_GPU_ID: g->gpu_id = (uint32_t)v; break;
		case KBASE_GPUPROP_COHERENCY_NUM_GROUPS: g->coherency_groups = (uint32_t)v; break;
		default: break;
		}
	}
	g->arch_major = g->gpu_id >> 28;
	g->arch = g->arch_major;
	g->arch_minor = (g->gpu_id >> 24) & 0xf;
	g->arch_rev = (g->gpu_id >> 20) & 0xf;
	g->product_major = (g->gpu_id >> 16) & 0xf;
	g->va_bits = g->mmu_features & 0xff;        /* MMU_FEATURES[7:0] VA_BITS, [15:8] PA_BITS */
	g->pa_bits = (g->mmu_features >> 8) & 0xff;
}

static int load_props(struct kb_ctx *c)
{
	struct kbase_ioctl_get_gpuprops gp = { 0 };
	int sz = ioctl(c->fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
	if (sz <= 0) {
		kb_set_error("GET_GPUPROPS size query failed: %s", strerror(errno));
		return -errno ? -errno : -EIO;
	}
	uint8_t *buf = malloc((size_t)sz);
	if (!buf)
		return -ENOMEM;
	gp.buffer = (uintptr_t)buf;
	gp.size = (uint32_t)sz;
	int r = ioctl(c->fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
	if (r < 0) {
		int e = errno;
		free(buf);
		kb_set_error("GET_GPUPROPS failed: %s", strerror(e));
		return -e;
	}
	parse_props(buf, (size_t)r, &c->props);
	kb_v10 = c->props.arch == 10;   /* arch-keyed packer switch (SET_SB_ENTRY, ZSD bit 21); the vk driver sets the same flag */
	free(buf);
	return 0;
}

static int load_csf(struct kb_ctx *c)
{
	union kbase_ioctl_cs_get_glb_iface gi;
	memset(&gi, 0, sizeof gi);
	if (ioctl(c->fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &gi) < 0) {
		kb_set_error("CS_GET_GLB_IFACE failed: %s", strerror(errno));
		return -errno;
	}
	struct kb_gpu_props *g = &c->props;
	g->csf_glb_version = gi.out.glb_version;
	g->csf_features = gi.out.features;
	g->csf_group_num = gi.out.group_num;
	g->csf_prfcnt_size = gi.out.prfcnt_size;
	g->csf_total_streams = gi.out.total_stream_num;
	g->csf_streams_per_group_min = 0;
	if (gi.out.group_num && gi.out.group_num <= 64 && gi.out.total_stream_num <= 1024) {
		struct basep_cs_group_control groups[64];
		struct basep_cs_stream_control *streams = calloc(gi.out.total_stream_num ? gi.out.total_stream_num : 1, sizeof *streams);
		if (!streams)
			return -ENOMEM;
		union kbase_ioctl_cs_get_glb_iface g2;
		memset(&g2, 0, sizeof g2);
		memset(groups, 0, sizeof groups);
		g2.in.max_group_num = gi.out.group_num;
		g2.in.max_total_stream_num = gi.out.total_stream_num;
		g2.in.groups_ptr = (uintptr_t)groups;
		g2.in.streams_ptr = (uintptr_t)streams;
		if (ioctl(c->fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &g2) == 0) {
			uint32_t mn = groups[0].stream_num;
			for (uint32_t i = 1; i < gi.out.group_num; i++)
				if (groups[i].stream_num < mn)
					mn = groups[i].stream_num;
			g->csf_streams_per_group_min = mn;
			g->csf_group0_suspend_size = groups[0].suspend_size;
		}
		free(streams);
	}
	return 0;
}

int kb_open(struct kb_ctx **out, const struct kb_open_opts *opts)
{
	const char *path = opts && opts->path ? opts->path : "/dev/mali0";
	*out = NULL;
	struct kb_ctx *c = calloc(1, sizeof *c);
	if (!c)
		return -ENOMEM;
	c->kcpu_id = -1;
	c->fd = open(path, O_RDWR | O_CLOEXEC);
	if (c->fd < 0) {
		int e = errno;
		kb_set_error("open(%s): %s", path, strerror(e));
		free(c);
		return -e;
	}

	/* The CSF flavour uses nr 52, the JM flavour nr 0; each rejects the other's number. */
	struct kbase_ioctl_version_check v = { 0, 0 };
	if (ioctl(c->fd, KBASE_IOCTL_VERSION_CHECK_CSF, &v) < 0) {
		int e = errno;
		struct kbase_ioctl_version_check vj = { 0, 0 };
		if (ioctl(c->fd, KBASE_IOCTL_VERSION_CHECK_JM, &vj) == 0)
			kb_set_error("%s is a Job-Manager (JM) kbase %u.%u; libkbase supports CSF only", path, vj.major, vj.minor);
		else
			kb_set_error("VERSION_CHECK failed on %s: %s", path, strerror(e));
		close(c->fd);
		free(c);
		return -ENOTSUP;
	}
	c->uapi_major = v.major;
	c->uapi_minor = v.minor;
	const struct kb_caps *cp = kb_caps_lookup(v.major, v.minor);
	if (!cp) {
		if (opts && opts->allow_unknown_uapi) {
			c->caps = k_caps[sizeof k_caps / sizeof *k_caps - 1];
			c->caps.major = v.major;
			c->caps.minor = v.minor;
			c->caps.ddk_note = "UNKNOWN version, using newest known layouts";
		} else {
			kb_set_error("kbase CSF uAPI %u.%u is not supported by libkbase (known: 1.21, 1.30). Refusing to guess struct layouts; "
				     "verify the new version and add a row to the capability table.", v.major, v.minor);
			close(c->fd);
			free(c);
			return -ENOTSUP;
		}
	} else {
		c->caps = *cp;
	}

	struct kbase_ioctl_set_flags sf = { .create_flags = opts ? opts->context_flags : 0 };
	if (ioctl(c->fd, KBASE_IOCTL_SET_FLAGS, &sf) < 0) {
		int e = errno;
		kb_set_error("SET_FLAGS failed: %s", strerror(e));
		close(c->fd);
		free(c);
		return -e;
	}

	/* The kernel wants the tracking page mapped before memory operations; failure is not fatal. */
	void *t = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, c->fd, (off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	c->tracking = t == MAP_FAILED ? NULL : t;

	int r = load_props(c);
	if (r == 0) {
		/* PanVK ground truth (runs/20261115-cmf-libkbase/panvk-trace/): right after GET_GPUPROPS and before GET_GLB_IFACE it issues
		 * MEM_EXEC_INIT{va_pages=0x1000} (nr 38) and MEM_JIT_INIT{va_pages=0x2000000, max_allocations=255, trim=0, group=0,
		 * phys_pages=0x2000000} (nr 14); both return 0. Failures are not fatal here. */
		struct kbase_ioctl_mem_exec_init ei = { .va_pages = 0x1000 };
		if (getenv("KB_SKIP_EXEC_INIT")) c->exec_init_errno = -1;
		else if (ioctl(c->fd, KBASE_IOCTL_MEM_EXEC_INIT, &ei) < 0)
			c->exec_init_errno = errno;
		struct kbase_ioctl_mem_jit_init ji = { .va_pages = 0x2000000, .max_allocations = 255, .phys_pages = 0x2000000 };
		if (getenv("KB_SKIP_JIT_INIT")) c->jit_init_errno = -1;
		else if (ioctl(c->fd, KBASE_IOCTL_MEM_JIT_INIT, &ji) < 0)
			c->jit_init_errno = errno;
		r = load_csf(c);
	}
	if (r < 0) {
		kb_close(c);
		return r;
	}
	char dv[sizeof c->ddk] = { 0 };
	struct kbase_ioctl_get_ddk_version dd = { .version_buffer = (uintptr_t)dv, .size = (uint32_t)sizeof dv - 1 };
	if (ioctl(c->fd, KBASE_IOCTL_GET_DDK_VERSION, &dd) >= 0)
		memcpy(c->ddk, dv, sizeof c->ddk);

	*out = c;
	return 0;
}

void kb_close(struct kb_ctx *c)
{
	if (!c)
		return;
	if (c->live_bos || c->live_queues)
		fprintf(stderr, "libkbase: kb_close with %d live BO(s), %d live queue(s)\n", c->live_bos, c->live_queues);
	kb_kcpu_destroy(c);
	if (c->tracking)
		munmap(c->tracking, 4096);
	if (c->fd >= 0)
		close(c->fd);
	free(c);
}

int kb_fd(const struct kb_ctx *c) { return c->fd; }
const struct kb_gpu_props *kb_props(const struct kb_ctx *c) { return &c->props; }
const struct kb_caps *kb_caps(const struct kb_ctx *c) { return &c->caps; }
void kb_uapi_version(const struct kb_ctx *c, unsigned *ma, unsigned *mi) { *ma = c->uapi_major; *mi = c->uapi_minor; }
const char *kb_ddk_version(const struct kb_ctx *c) { return c->ddk; }
void kb_leaks(const struct kb_ctx *c, struct kb_leaks *o)
{
	o->bos = c->live_bos;
	o->queues = c->live_queues;
	o->kcpu_queues = c->kcpu_id >= 0;
}

int kb_gpu_timestamp(struct kb_ctx *ctx, uint64_t *ticks)
{
	union kbase_ioctl_get_cpu_gpu_timeinfo a;
	memset(&a, 0, sizeof a);
	a.in.request_flags = KBASE_TIMEINFO_TIMESTAMP_FLAG;
	if (kb_ioctl(ctx, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &a))
		return -errno;
	*ticks = a.out.timestamp;
	return 0;
}

uint64_t kb_timestamp_hz(struct kb_ctx *ctx)
{
	union kbase_ioctl_get_cpu_gpu_timeinfo a, b;
	memset(&a, 0, sizeof a);
	a.in.request_flags = KBASE_TIMEINFO_MONOTONIC_FLAG | KBASE_TIMEINFO_TIMESTAMP_FLAG;
	b = a;
	if (kb_ioctl(ctx, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &a))
		return 0;
	usleep(20000);
	if (kb_ioctl(ctx, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &b))
		return 0;
	double dt = (double)(b.out.sec - a.out.sec) + (double)((int64_t)b.out.nsec - (int64_t)a.out.nsec) * 1e-9;
	double dts = (double)(b.out.timestamp - a.out.timestamp);
	if (dt <= 0 || dts <= 0)
		return 0;
	double hz = dts / dt;
	return hz > 1e6 && hz < 2e9 ? (uint64_t)(hz + 0.5) : 0;
}
