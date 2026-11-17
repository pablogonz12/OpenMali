/* SPDX-License-Identifier: MIT */
/* Out-of-tree shim: the NDK does not ship the platform header <cutils/native_handle.h>. Layout is the
 * stable AOSP one (system/core/libcutils/include/cutils/native_handle.h); only the type is needed. */
#ifndef MALI_SHIM_NATIVE_HANDLE_H
#define MALI_SHIM_NATIVE_HANDLE_H
typedef struct native_handle {
   int version; /* sizeof(native_handle_t) */
   int numFds;
   int numInts;
   int data[0];
} native_handle_t;
#endif
