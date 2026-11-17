// SPDX-License-Identifier: MIT
/* Minimal "loader": forwards vkGetInstanceProcAddr to an ICD named in $MALI_ICD (direct, no loader layers). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stddef.h>
typedef void (*pfn)(void);
static pfn (*icd_gipa)(void *, const char *);
static void init(void)
{
	if (icd_gipa) return;
	void *h = dlopen(getenv("MALI_ICD"), RTLD_NOW | RTLD_LOCAL);
	if (h) icd_gipa = (pfn (*)(void *, const char *))dlsym(h, "vk_icdGetInstanceProcAddr");
}
__attribute__((visibility("default"))) pfn vkGetInstanceProcAddr(void *i, const char *n) { init(); return icd_gipa ? icd_gipa(i, n) : NULL; }
__attribute__((visibility("default"))) pfn vkGetDeviceProcAddr(void *d, const char *n) { init(); (void)d; (void)n; return NULL; }
