/* SPDX-License-Identifier: MIT */
/* VK_EXT_transform_feedback is WIP: the extension, features, properties and the NIR lowering are in place, but the
 * streamout job does not exist yet (libkbase has no vertex-only/streamout job: kbase_cs.h:111-154 is
 * RUN_IDVS/FINISH_TILING only), so a pipeline that declares XFB failes to compile with a message naming the reason
 * (mali_compile.c). MALI_EXPERIMENTAL_TF=1 advertises the plumbing for tests; it stays off by default, and nothing
 * may advertise the extension to applications before the job can write the buffers. */
#ifndef MALI_TF_H
#define MALI_TF_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_experimental_tf(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_TF");
   return e && e[0] == '1';
}

#endif
