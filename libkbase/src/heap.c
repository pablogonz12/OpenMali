/* SPDX-License-Identifier: MIT */
/* Layer E: CSF tiler heap (KBASE_IOCTL_CS_TILER_HEAP_INIT / TERM). */
#include <errno.h>
#include <string.h>

#include "internal.h"

/* Requires MEM_JIT_INIT on the context (done in kb_open, src/ctx.c); otherwise the kernel returns ENOMEM for every size.
 * PanVK itself uses chunk 1 MiB, initial 10, max 400, target_in_flight 0xffff (see PANVK-SEQUENCE.md). */
int kb_tiler_heap_create(struct kb_ctx *ctx, uint32_t chunk_size, uint32_t initial_chunks, uint32_t max_chunks, struct kb_tiler_heap *h)
{
	union kbase_ioctl_cs_tiler_heap_init u;
	memset(h, 0, sizeof *h);
	memset(&u, 0, sizeof u);
	u.in.chunk_size = chunk_size;
	u.in.initial_chunks = initial_chunks;
	u.in.max_chunks = max_chunks;
	u.in.target_in_flight = 65535;
	int r = -ENOTTY;
	if (ctx->caps.tiler_heap_24b) {
		/* uAPI >= 1.14 in Arm's public headers; buf_desc_va = 0 (no reclaim buffer descriptor). Falls back to 16 B on ENOTTY. */
		union kbase_ioctl_cs_tiler_heap_init_24 u24;
		memset(&u24, 0, sizeof u24);
		u24.in.chunk_size = chunk_size; u24.in.initial_chunks = initial_chunks; u24.in.max_chunks = max_chunks;
		u24.in.target_in_flight = 65535;
		r = kb_ioctl(ctx, KBASE_IOCTL_CS_TILER_HEAP_INIT_24, &u24);
		if (r == 0) { u.out.gpu_heap_va = u24.out.gpu_heap_va; u.out.first_chunk_va = u24.out.first_chunk_va; }
	}
	if (r == -ENOTTY)
		r = kb_ioctl(ctx, KBASE_IOCTL_CS_TILER_HEAP_INIT, &u);
	if (r) {
		kb_set_error("CS_TILER_HEAP_INIT(chunk %u, initial %u, max %u): %s", chunk_size, initial_chunks, max_chunks, strerror(-r));
		return r;
	}
	h->ctx = ctx;
	h->ctx_va = u.out.gpu_heap_va;
	h->first_chunk_va = u.out.first_chunk_va;
	h->chunk_size = chunk_size;
	h->live = true;
	return 0;
}

void kb_tiler_heap_destroy(struct kb_tiler_heap *h)
{
	if (!h->live) return;
	struct kbase_ioctl_cs_tiler_heap_term t = { h->ctx_va };
	kb_ioctl(h->ctx, KBASE_IOCTL_CS_TILER_HEAP_TERM, &t);
	h->live = false;
}
