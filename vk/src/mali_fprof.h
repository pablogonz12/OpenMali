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
   double frame_ms;     /* kick-to-kick interval in force when this was kicked (0: unknown) */
   unsigned pending;    /* frames not retired yet when this one was kicked (>0: the GPU had queued work) */
   double submit_us;    /* CPU time the backend kick took */
};

/* The steps of one present, as the software (MIT-SHM/PutImage) path runs them (mali_wsi.c): the acquire call, the
 * wait for the frame's submit fence, the device->CPU sync of the image, the copy of the whole image into the shm
 * segment, the X request + flush, and the whole vkQueuePresentKHR call around them. `present_ms` in the frame line is
 * only the *interval* between two presents (1/fps at steady state); these are the times inside the call. */
enum mali_fprof_wsi_step {
   MALI_FPROF_WSI_ACQUIRE = 0,
   MALI_FPROF_WSI_WAIT,
   MALI_FPROF_WSI_SYNC,
   MALI_FPROF_WSI_COPY,
   MALI_FPROF_WSI_PUT,
   MALI_FPROF_WSI_CALL,
   MALI_FPROF_WSI_STEPS
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
   /* per-step present times: the last present and the running sums, in ns */
   uint64_t last_wsi_ns[MALI_FPROF_WSI_STEPS];
   uint64_t sum_wsi_ns[MALI_FPROF_WSI_STEPS];
   uint64_t nwsi;
};

void mali_fprof_init(struct mali_fprof *p, struct mali_device *dev);
void mali_fprof_finish(struct mali_fprof *p);
/* A submission is about to be kicked: `expected` is the retire-counter value after it, `pending` the number of
 * earlier frames whose retire counter had not moved yet. */
void mali_fprof_kick(struct mali_fprof *p, struct mali_device *dev, uint64_t expected, unsigned pending);
/* The submission has been kicked: CPU time the backend kick (be->submit_cs) cost. Command-stream construction
 * happens before hw_submit_multi and is not included; nor is the wait for an older frame. */
void mali_fprof_kicked(struct mali_fprof *p, double submit_ms);
/* The retire counter now reads `counter`: close every submission it ends (one GPU-clock sample for the whole call). */
void mali_fprof_retired(struct mali_fprof *p, struct mali_device *dev, uint64_t counter);
/* A present-shaped submit (vk_common's QueuePresentKHR: no command buffers, waits + a signal). */
void mali_fprof_present(struct mali_fprof *p);
/* One step of the present path (`step` = enum mali_fprof_wsi_step) took `ns`; a no-op unless profiling is on. */
void mali_fprof_wsi(struct mali_fprof *p, unsigned step, uint64_t ns);
/* Start of a present: clears the per-step times of the last one. */
void mali_fprof_wsi_begin(struct mali_fprof *p);
/* GPU timestamp of "now", false if the backend has none. */
bool mali_fprof_ticks(struct mali_device *dev, uint64_t *ticks);

#endif
