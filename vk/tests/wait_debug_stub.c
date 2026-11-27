/* SPDX-License-Identifier: MIT */
/* Standalone kbase tests do not link the Vulkan ICD's diagnostic logger. */
#include "mali_wait_debug.h"

int mali_wait_debug(void) { return 0; }
void mali_wait_event(const char *fmt, ...) { (void)fmt; }
/* The profiling sink: the standalone tests do not link mali_sync.c, so they get the disabled versions. */
int mali_prof_enabled(void) { return 0; }
void mali_prof_add(enum mali_prof_counter counter, uint64_t value) { (void)counter; (void)value; }
void mali_prof_tick(void) {}
void mali_prof_final(void) {}
