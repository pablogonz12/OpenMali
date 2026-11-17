/* SPDX-License-Identifier: MIT */
#ifndef KB_INTERNAL_H
#define KB_INTERNAL_H

#include "kbase.h"

struct kb_ctx {
	int fd;
	uint16_t uapi_major, uapi_minor;
	struct kb_caps caps;
	struct kb_gpu_props props;
	char ddk[96];
	void *tracking;           /* 4 KiB tracking page, kept until close */
	int kcpu_id;              /* -1 until created */
	bool group_faulted[256];
	unsigned long long events_seen;
	struct kb_event last_fatal;   /* last fatal notification (consumed by any reader), for post-mortem; type 0 = none */
	bool have_fatal;
	int live_bos, live_queues;
	int exec_init_errno, jit_init_errno; /* 0 = MEM_EXEC_INIT / MEM_JIT_INIT succeeded */
};

void kb_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kb_ioctl(struct kb_ctx *ctx, unsigned long req, void *arg);   /* retries EINTR; returns 0 or -errno */
void kb_kcpu_destroy(struct kb_ctx *ctx);

#endif
