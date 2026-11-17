/* SPDX-License-Identifier: MIT */
/* Layer C: CSF queue group (one group, one queue) and notification decoding. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"

#define DMB() kb_wmb()   /* dsb st: ring/descriptor writes are complete, not merely ordered, before the kick */

static unsigned popcnt64(uint64_t v) { return (unsigned)__builtin_popcountll(v); }

int kb_queue_create(struct kb_ctx *ctx, const struct kb_queue_opts *opts, struct kb_queue *q)
{
	memset(q, 0, sizeof *q);
	q->ctx = ctx;
	q->ring_size = opts && opts->ring_size ? opts->ring_size : 4096;
	uint8_t prio = opts && opts->group_priority ? opts->group_priority : BASE_QUEUE_GROUP_PRIORITY_LOW;
	if (q->ring_size < 4096 || (q->ring_size & (q->ring_size - 1)))
		return -EINVAL;
	const struct kb_gpu_props *p = &ctx->props;

	/* Ring buffer: GPU RD|WR, uncached (write-combined) CPU mapping. */
	int r = kb_bo_alloc(ctx, q->ring_size, KB_BO_GPU_RD | KB_BO_GPU_WR, &q->ring);
	if (r)
		return r;
	memset(q->ring.cpu, 0, q->ring_size);

	struct kbase_ioctl_cs_queue_register reg;
	memset(&reg, 0, sizeof reg);
	reg.buffer_gpu_addr = q->ring.gpu_va;
	reg.buffer_size = q->ring_size;
	reg.priority = 0;
	r = kb_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg);
	if (r) {
		kb_set_error("CS_QUEUE_REGISTER: %s", strerror(-r));
		goto fail;
	}
	q->registered = true;

	union kbase_ioctl_cs_queue_group_create gc;
	union kbase_ioctl_cs_queue_group_create_112 gc112;
	memset(&gc, 0, sizeof gc);
	memset(&gc112, 0, sizeof gc112);
	gc.in.tiler_mask = p->tiler_present;
	gc.in.fragment_mask = p->shader_present;
	gc.in.compute_mask = p->shader_present;
	gc.in.cs_min = 1;
	gc.in.priority = prio;
	gc.in.tiler_max = (uint8_t)popcnt64(p->tiler_present);
	gc.in.fragment_max = (uint8_t)popcnt64(p->shader_present);   /* popcount: Poco shader_present 0xee0057 = 11 cores, non-contiguous */
	gc.in.compute_max = (uint8_t)popcnt64(p->shader_present);
	r = -ENOTTY;
	if (ctx->caps.group_create_112b) {
		/* same fields as the 40 B layout; neural/comp_pri/cs_fault_report_enable stay 0 */
		gc112.in.tiler_mask = gc.in.tiler_mask; gc112.in.fragment_mask = gc.in.fragment_mask;
		gc112.in.compute_mask = gc.in.compute_mask; gc112.in.cs_min = gc.in.cs_min; gc112.in.priority = gc.in.priority;
		gc112.in.tiler_max = gc.in.tiler_max; gc112.in.fragment_max = gc.in.fragment_max; gc112.in.compute_max = gc.in.compute_max;
		r = kb_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_112, &gc112);
		if (r == 0) { gc.out.group_handle = gc112.out.group_handle; gc.out.group_uid = gc112.out.group_uid; }
	}
	if (r == -ENOTTY || !ctx->caps.group_create_112b)
		r = kb_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE, &gc);
	if (r) {
		kb_set_error("CS_QUEUE_GROUP_CREATE: %s", strerror(-r));
		goto fail;
	}
	q->group_handle = gc.out.group_handle;
	q->group_uid = gc.out.group_uid;
	q->group_live = true;
	ctx->group_faulted[q->group_handle] = false;

	union kbase_ioctl_cs_queue_bind bd;
	memset(&bd, 0, sizeof bd);
	bd.in.buffer_gpu_addr = q->ring.gpu_va;
	bd.in.group_handle = q->group_handle;
	bd.in.csi_index = 0;
	r = kb_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_BIND, &bd);
	if (r) {
		kb_set_error("CS_QUEUE_BIND: %s", strerror(-r));
		goto fail;
	}
	q->bound = true;

	void *io = mmap(NULL, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd,
			(off_t)bd.out.mmap_handle);
	if (io == MAP_FAILED) {
		r = -errno;
		kb_set_error("mmap of user I/O pages: %s", strerror(errno));
		goto fail;
	}
	q->user_io = io;
	q->doorbell = (volatile uint32_t *)((char *)io);
	q->cs_insert = (volatile uint64_t *)((char *)io + 4096);
	q->cs_extract = (volatile uint64_t *)((char *)io + 8192);
	q->cs_active = (volatile uint32_t *)((char *)io + 8192 + 8);
	q->insert = *q->cs_insert; /* a fresh queue starts at 0 */
	ctx->live_queues++;
	return 0;
fail:
	ctx->live_queues++; /* kb_queue_destroy decrements */
	kb_queue_destroy(q);
	return r;
}

uint64_t kb_queue_extract(const struct kb_queue *q) { DMB(); return *q->cs_extract; }
bool kb_queue_idle(const struct kb_queue *q) { return kb_queue_extract(q) == q->insert; }
bool kb_queue_faulted(const struct kb_queue *q) { return q->ctx->group_faulted[q->group_handle]; }

int kb_queue_submit(struct kb_queue *q, const uint64_t *ins, size_t n, enum kb_kick how)
{
	size_t total = (n * 8 + 63) & ~(size_t)63; /* pad to 64 bytes: the firmware prefetches in 64-byte units */
	if (!total || total >= q->ring_size)
		return -EINVAL;
	uint64_t ext = kb_queue_extract(q);
	/* Never fill the ring completely: with CS_INSERT - CS_EXTRACT == size the firmware raises CS_CONFIG_FAULT
	 * (status 0x40, sideband 0; CMF arch 11, 2026-11-16). One 64-byte unit always stays free. */
	if (q->insert + total - ext >= q->ring_size)
		return -EAGAIN;
	uint64_t *ring = q->ring.cpu;
	size_t mask = q->ring_size - 1;
	for (size_t i = 0; i < total / 8; i++) {
		uint64_t w = i < n ? ins[i] : kb_cs_nop();
		ring[((q->insert + i * 8) & mask) / 8] = w;
	}
	q->insert += total;
	DMB(); /* ring contents before CS_INSERT */
	*q->cs_insert = q->insert;
	DMB();
	if (how == KB_KICK_DOORBELL) {
		*q->doorbell = 1;
		q->doorbells++;
	} else {
		struct kbase_ioctl_cs_queue_kick k = { q->ring.gpu_va };
		int r = kb_ioctl(q->ctx, KBASE_IOCTL_CS_QUEUE_KICK, &k);
		if (r) {
			kb_set_error("CS_QUEUE_KICK: %s", strerror(-r));
			return r;
		}
		q->kicks++;
	}
	return 0;
}

void kb_queue_destroy(struct kb_queue *q)
{
	if (!q->ctx)
		return;
	struct kb_ctx *ctx = q->ctx;
	if (q->group_live) {
		struct kbase_ioctl_cs_queue_group_term t;
		memset(&t, 0, sizeof t);
		t.group_handle = q->group_handle;
		kb_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &t); /* may be EINVAL after a fatal fault */
		q->group_live = false;
	}
	if (q->user_io) {
		munmap(q->user_io, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096);
		q->user_io = NULL;
	}
	if (q->registered) {
		struct kbase_ioctl_cs_queue_terminate t = { q->ring.gpu_va };
		kb_ioctl(ctx, KBASE_IOCTL_CS_QUEUE_TERMINATE, &t);
		q->registered = false;
	}
	kb_bo_free(&q->ring);
	ctx->live_queues--;
	memset(q, 0, sizeof *q);
}

/* ---------------------------------------------------------------- notifications */
struct exc { unsigned lo, hi; const char *name; };
/* Exception codes (low byte of the status word). Names follow the public panthor/kbase tables. */
static const struct exc k_exc[] = {
	{ 0x0f, 0x0f, "CS_RESOURCE_TERMINATED" },
	{ 0x40, 0x40, "CS_CONFIG_FAULT" }, { 0x41, 0x41, "CS_UNRECOVERABLE" }, { 0x44, 0x44, "CS_ENDPOINT_FAULT" },
	{ 0x48, 0x48, "CS_BUS_FAULT" }, { 0x49, 0x49, "CS_INVALID_INSTRUCTION" }, { 0x4a, 0x4a, "CS_CALL_STACK_OVERFLOW" },
	{ 0x4b, 0x4b, "CS_INHERIT_FAULT" },
	{ 0x50, 0x50, "INSTR_INVALID_PC" }, { 0x51, 0x51, "INSTR_INVALID_ENC" }, { 0x55, 0x55, "INSTR_BARRIER_FAULT" },
	{ 0x58, 0x58, "DATA_INVALID_FAULT" }, { 0x59, 0x59, "TILE_RANGE_FAULT" }, { 0x5a, 0x5a, "ADDR_RANGE_FAULT" },
	{ 0x5b, 0x5b, "IMPRECISE_FAULT" },
	{ 0x60, 0x60, "OUT_OF_MEMORY" }, { 0x68, 0x68, "CSF_FW_INTERNAL_ERROR" }, { 0x69, 0x69, "CSF_RES_EVICTION_TIMEOUT" },
	{ 0x80, 0x80, "GPU_BUS_FAULT" }, { 0x88, 0x89, "GPU_SHAREABILITY_FAULT" }, { 0x8a, 0x8a, "GPU_CACHEABILITY_FAULT" },
	{ 0xc0, 0xc4, "TRANSLATION_FAULT" }, { 0xc8, 0xcb, "PERMISSION_FAULT" }, { 0xd0, 0xd3, "TRANSTAB_BUS_FAULT" },
	{ 0xd9, 0xdb, "ACCESS_FLAG" }, { 0xe0, 0xe0, "ADDRESS_SIZE_FAULT" }, { 0xe4, 0xe7, "ADDRESS_SIZE_FAULT" },
	{ 0xe8, 0xeb, "MEMORY_ATTRIBUTE_FAULT" },
};

const char *kb_exception_name(unsigned code)
{
	code &= 0xff;
	for (size_t i = 0; i < sizeof k_exc / sizeof *k_exc; i++)
		if (code >= k_exc[i].lo && code <= k_exc[i].hi)
			return k_exc[i].name;
	return "UNKNOWN_EXCEPTION";
}

unsigned long long kb_events_seen(const struct kb_ctx *ctx) { return ctx->events_seen; }

int kb_event_read(struct kb_ctx *ctx, struct kb_event *ev, int timeout_ms)
{
	memset(ev, 0, sizeof *ev);
	ev->group = ev->error_type = ev->csi_index = -1;
	struct pollfd pf = { ctx->fd, POLLIN, 0 };
	int r;
	do r = poll(&pf, 1, timeout_ms); while (r < 0 && errno == EINTR);
	if (r < 0)
		return -errno;
	if (r == 0 || !(pf.revents & POLLIN))
		return 0;
	struct base_csf_notification n;
	ssize_t got;
	do got = read(ctx->fd, &n, sizeof n); while (got < 0 && errno == EINTR);
	if (got < 0)
		return errno == EAGAIN ? 0 : -errno;
	if (got != (ssize_t)sizeof n) {
		kb_set_error("short notification read (%zd bytes)", got);
		return -EIO;
	}
	ctx->events_seen++;
	ev->type = n.type;
	switch (n.type) {
	case BASE_CSF_NOTIFICATION_EVENT:
		snprintf(ev->text, sizeof ev->text, "EVENT (cqs/queue activity)");
		break;
	case BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP:
		snprintf(ev->text, sizeof ev->text, "CPU_QUEUE_DUMP");
		break;
	case BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR: {
		const struct base_gpu_queue_group_error *e = &n.payload.csg_error.error;
		ev->group = n.payload.csg_error.handle;
		ev->error_type = e->error_type;
		switch (e->error_type) {
		case BASE_GPU_QUEUE_GROUP_ERROR_FATAL:
			ev->status = e->payload.fatal_group.status;
			ev->sideband = e->payload.fatal_group.sideband;
			ev->fatal = true;
			snprintf(ev->text, sizeof ev->text, "group %d FATAL: %s (status 0x%08x, sideband 0x%llx)", ev->group,
				 kb_exception_name(ev->status), ev->status, (unsigned long long)ev->sideband);
			break;
		case BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL:
			ev->status = e->payload.fatal_queue.status;
			ev->sideband = e->payload.fatal_queue.sideband;
			ev->csi_index = e->payload.fatal_queue.csi_index;
			ev->fatal = true;
			snprintf(ev->text, sizeof ev->text, "group %d queue %d FATAL: %s (status 0x%08x, sideband 0x%llx)",
				 ev->group, ev->csi_index, kb_exception_name(ev->status), ev->status,
				 (unsigned long long)ev->sideband);
			break;
		case BASE_GPU_QUEUE_GROUP_ERROR_TIMEOUT:
			ev->fatal = true;
			snprintf(ev->text, sizeof ev->text, "group %d progress TIMEOUT", ev->group);
			break;
		case BASE_GPU_QUEUE_GROUP_ERROR_TILER_HEAP_OOM:
			snprintf(ev->text, sizeof ev->text, "group %d tiler heap OOM", ev->group);
			break;
		default:
			snprintf(ev->text, sizeof ev->text, "group %d unknown error type %d", ev->group, e->error_type);
		}
		if (ev->fatal && (unsigned)ev->group < 256)
			ctx->group_faulted[ev->group] = true;
		break;
	}
	default:
		snprintf(ev->text, sizeof ev->text, "unknown notification type %d", n.type);
	}
	return 1;
}
