/* SPDX-License-Identifier: MIT */
/* Layer D: KCPU queue, CQS wait/set and sync_file fences. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

static int kcpu_ensure(struct kb_ctx *ctx)
{
	if (ctx->kcpu_id >= 0)
		return 0;
	struct kbase_ioctl_kcpu_queue_new n;
	memset(&n, 0, sizeof n);
	int r = kb_ioctl(ctx, KBASE_IOCTL_KCPU_QUEUE_CREATE, &n);
	if (r) {
		kb_set_error("KCPU_QUEUE_CREATE: %s", strerror(-r));
		return r;
	}
	ctx->kcpu_id = n.id;
	return 0;
}

void kb_kcpu_destroy(struct kb_ctx *ctx)
{
	if (ctx->kcpu_id < 0)
		return;
	struct kbase_ioctl_kcpu_queue_delete d;
	memset(&d, 0, sizeof d);
	d.id = (uint8_t)ctx->kcpu_id;
	kb_ioctl(ctx, KBASE_IOCTL_KCPU_QUEUE_DELETE, &d);
	ctx->kcpu_id = -1;
}

/* kbase requires exactly one command per enqueue. */
static int enqueue1(struct kb_ctx *ctx, struct base_kcpu_command *c)
{
	struct kbase_ioctl_kcpu_queue_enqueue e;
	memset(&e, 0, sizeof e);
	e.addr = (uint64_t)(uintptr_t)c;
	e.nr_commands = 1;
	e.id = (uint8_t)ctx->kcpu_id;
	int r = kb_ioctl(ctx, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &e);
	if (r)
		kb_set_error("KCPU_QUEUE_ENQUEUE(type %u): %s", c->type, strerror(-r));
	return r;
}

int kb_kcpu_cqs_wait_gt64(struct kb_ctx *ctx, uint64_t cqs_gpu_va, uint64_t val)
{
	int r = kcpu_ensure(ctx);
	if (r)
		return r;
	struct base_cqs_wait_operation_info w;
	memset(&w, 0, sizeof w);
	w.addr = cqs_gpu_va;
	w.val = val;
	w.operation = BASEP_CQS_WAIT_OPERATION_GT;
	w.data_type = BASEP_CQS_DATA_TYPE_U64;
	struct base_kcpu_command c;
	memset(&c, 0, sizeof c);
	c.type = BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION;
	c.info.cqs_wait_operation.objs = (uint64_t)(uintptr_t)&w;
	c.info.cqs_wait_operation.nr_objs = 1;
	return enqueue1(ctx, &c);
}

int kb_kcpu_cqs_set64(struct kb_ctx *ctx, uint64_t cqs_gpu_va, uint64_t val, bool add)
{
	int r = kcpu_ensure(ctx);
	if (r)
		return r;
	struct base_cqs_set_operation_info s;
	memset(&s, 0, sizeof s);
	s.addr = cqs_gpu_va;
	s.val = val;
	s.operation = add ? BASEP_CQS_SET_OPERATION_ADD : BASEP_CQS_SET_OPERATION_SET;
	s.data_type = BASEP_CQS_DATA_TYPE_U64;
	struct base_kcpu_command c;
	memset(&c, 0, sizeof c);
	c.type = BASE_KCPU_COMMAND_TYPE_CQS_SET_OPERATION;
	c.info.cqs_set_operation.objs = (uint64_t)(uintptr_t)&s;
	c.info.cqs_set_operation.nr_objs = 1;
	return enqueue1(ctx, &c);
}

int kb_kcpu_fence_signal(struct kb_ctx *ctx, int *sync_file_fd)
{
	int r = kcpu_ensure(ctx);
	if (r)
		return r;
	struct base_fence f = { -1, -1 };
	struct base_kcpu_command c;
	memset(&c, 0, sizeof c);
	c.type = BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL;
	c.info.fence = (uint64_t)(uintptr_t)&f;
	r = enqueue1(ctx, &c);
	if (r)
		return r;
	if (f.fd < 0) {
		kb_set_error("FENCE_SIGNAL returned no fd");
		return -EIO;
	}
	*sync_file_fd = f.fd;
	return 0;
}

static int64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int kb_fence_wait(struct kb_ctx *ctx, int sync_fd, int timeout_ms)
{
	int64_t end = now_ms() + timeout_ms;
	for (;;) {
		int64_t left = end - now_ms();
		if (left < 0)
			left = 0;
		struct pollfd pf[2] = { { sync_fd, POLLIN, 0 }, { ctx->fd, POLLIN, 0 } };
		int r = poll(pf, 2, (int)left);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (pf[0].revents & (POLLIN | POLLERR))
			return (pf[0].revents & POLLERR) ? -EIO : 0;
		if (pf[1].revents & POLLIN) {
			struct kb_event ev;
			int e = kb_event_read(ctx, &ev, 0);
			if (e > 0 && ev.fatal)
				return -EIO;
			continue;
		}
		if (r == 0)
			return -ETIMEDOUT;
	}
}
