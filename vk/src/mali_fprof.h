/* SPDX-License-Identifier: MIT */
/* Per-frame instrumentation (M15): CPU submit time, a GPU busy *estimate* and the present interval, one frame per
 * line. Off unless MALI_FRAME_PROF asks for it (see mali_fprof.c), so the driver path is unchanged.
 *
 * What kbase gives us is KBASE_IOCTL_GET_CPU_GPU_TIMEINFO (libkbase `kb_timeinfo`): a free-running GPU timestamp
 * counter, global to the GPU, sampled by the CPU. There is no per-job start/end stamp anywhere in kbase, so the GPU
 * number here is derived from when the CPU kicked a frame and when it *observed* the frame's retire counter: an
 * estimate with a known bias direction, not a hardware busy counter.
 */
#ifndef MALI_FPROF_H
#define MALI_FPROF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct mali_device;

#define MALI_FPROF_MAX 8 /* in-flight submissions tracked (MALI_MAX_INFLIGHT is 3; the CS ring can hold more) */

/* One submission that has been kicked but not yet observed retired. */
struct mali_fprof_fl {
   uint64_t expected;   /* retire-counter value that ends this submission */
   uint64_t kick_ticks; /* GPU timestamp at the kick */
   uint64_t kick_cpu_ns;
   unsigned pending;    /* frames not retired yet when this one was kicked (>0: the GPU had queued work) */
   double submit_us;    /* CPU time the submission itself took */
};

/* MALI_FRAME_PROF=1 (or "stderr") -> lines on stderr; any other value -> a file path the lines are appended to. */
struct mali_fprof {
   bool on;
   uint64_t hz;       /* GPU timestamp ticks per second; 0 = unknown, ticks are taken as ns (the fake backend) */
   uint64_t seq;      /* frames measured so far */
   uint64_t npresent; /* present-shaped submits seen */
   bool have_kick, have_retire, have_present;
   uint64_t last_kick_cpu_ns, last_retire_ticks, last_present_cpu_ns;
   double last_frame_ms;    /* interval between the last two frame kicks */
   double last_present_ms;  /* interval between the last two presents */
   /* averages over the run (printed once at device teardown) */
   double sum_submit_us, sum_gpu_ms, sum_present_ms;
   unsigned n_gpu_bound, n_cpu_bound;
   FILE *out;      /* stderr, or the file the env var named */
   bool owned_out; /* out is a FILE we opened (closed at teardown) */
   struct mali_fprof_fl fl[MALI_FPROF_MAX];
   unsigned nfl;
};

void mali_fprof_init(struct mali_fprof *p, struct mali_device *dev);
void mali_fprof_finish(struct mali_fprof *p);
/* A submission is about to be kicked: `expected` is the retire-counter value after it, `pending` the number of
 * earlier frames whose retire counter had not moved yet. */
void mali_fprof_kick(struct mali_fprof *p, struct mali_device *dev, uint64_t expected, unsigned pending);
/* The submission has been kicked: CPU time it cost (build + kick + signal). */
void mali_fprof_kicked(struct mali_fprof *p, double submit_ms);
/* The retire counter now reads `counter`: close every submission it ends (one GPU-clock sample for the whole call). */
void mali_fprof_retired(struct mali_fprof *p, struct mali_device *dev, uint64_t counter);
/* A present-shaped submit (vk_common's QueuePresentKHR: no command buffers, waits + a signal). */
void mali_fprof_present(struct mali_fprof *p);
/* GPU timestamp of "now", false if the backend has none. */
bool mali_fprof_ticks(struct mali_device *dev, uint64_t *ticks);

#endif
