/* SPDX-License-Identifier: MIT */
/* Standalone kbase tests do not link the Vulkan ICD's diagnostic logger. */
#include "mali_wait_debug.h"

int mali_wait_debug(void) { return 0; }
void mali_wait_event(const char *fmt, ...) { (void)fmt; }
