/* SPDX-License-Identifier: MIT */
/* Debug/tuning environment knobs on the submit path: getenv() scans the whole environment (a strncmp per variable), and the
 * hot paths asked ~20 times per frame. MALI_GETENV_ONCE reads a name once per call site and per process. Do not use it for a
 * variable a test changes after the first submit. */
#ifndef MALI_ENV_H
#define MALI_ENV_H
#include <stdatomic.h>
#include <stdlib.h>
#define MALI_GETENV_ONCE(name)                                                       \
   ({                                                                                \
      static _Atomic(const char *) c_;                                               \
      static atomic_int i_;                                                          \
      if (!atomic_load_explicit(&i_, memory_order_acquire)) {                        \
         atomic_store_explicit(&c_, getenv(name), memory_order_relaxed);             \
         atomic_store_explicit(&i_, 1, memory_order_release);                        \
      }                                                                              \
      atomic_load_explicit(&c_, memory_order_relaxed);                               \
   })
#endif
