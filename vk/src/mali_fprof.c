/* SPDX-License-Identifier: MIT */
/* M15: per-frame profiling (CPU submit time, GPU busy estimate, present interval).
 *
 * MALI_FRAME_PROF=1          one line per frame on stderr
 * MALI_FRAME_PROF=<path>     the same lines appended to <path> (what tests and tools/frameprof read)
 * MALI_FRAME_PROF=0|unset    off: no ioctl, no clock read, no allocation (the driver path is untouched)
 *
 * What is measured, and with what:
 *   submit_us   CPU time of the backend kick (be->submit_cs); command-stream construction happens before and is not
 *               included, nor is the wait for older frames.
 *   gpu_ms      estimate of the frame's own GPU time from the GPU timestamp counter (kb_timeinfo, global, GPU clock),
 *               sampled when the CPU kicks the frame and when the CPU first sees the frame's retire counter move.
 *   present_ms  interval between two present-shaped submits (0 until a second one is seen).
 *   frame_ms    interval between two frame kicks (submit-to-submit): the wall clock the app runs at.
 *
 * The estimator (see also mali_fprof.h):
 *   pending == 0  the GPU had already retired everything when this frame was kicked, so the frame started at once:
 *                 gpu_ms = retire_ticks - kick_ticks                        (est=idle)
 *   pending > 0   older frames were still queued, so the GPU was saturated and consecutive retires tile the GPU
 *                 timeline: gpu_ms = retire_ticks - previous_retire_ticks    (est=sat)
 *   both clamped to [0, frame_ms]; bound=gpu iff pending > 0 at the kick, which is the lag-free signal (the retire
 *   counter is read at the kick, so it says what the GPU had *not* finished at that instant).
 *
 * Accuracy, honestly:
 *   - kbase has no job start/end timestamps and no busy counter: this is a sampled estimate, not hardware busy time.
 *   - the counter is a clock: it advances while the GPU is idle, so every span is an upper bound on the work inside it.
 *   - retirement is observed by the CPU, not by the GPU: the driver checks the retire counter at the next frame's
 *     job_begin (and whenever it has to wait). With the GPU idle at the kick that observation can be up to one frame
 *     late, so est=idle values are an upper bound (clamped to frame_ms). est=sat subtracts the same observation lag at
 *     both ends, so in the steady state it cancels; when one observation closes several queued retirements they share
 *     one timestamp and the observed span is divided over the batch, so individual durations are estimates (the
 *     aggregate is preserved). Clamping to frame_ms can under-report a frame that outlived the app's interval.
 *   - one submission reports one retire, so tiler/fragment segment boundaries inside a frame are invisible, and a
 *     frame split across several submissions appears as several frames.
 *   - submit_us is the CPU's own work; the wait for the oldest in-flight frame happens in hw_retire before the build
 *     and is not included.
 */
#include "mali_fprof.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mali_private.h"
#include "mali_queue.h"

static uint64_t
cpu_now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

bool
mali_fprof_ticks(struct mali_device *dev, uint64_t *ticks)
{
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;
   *ticks = 0;
   return be->gpu_timestamp && be->gpu_timestamp(dev->kdev, ticks) == 0;
}

static double
ticks_to_ms(const struct mali_fprof *p, uint64_t dt)
{
   const double hz = p->hz ? (double)p->hz : 1e9; /* no hz (fake backend): the counter is ns */
   return (double)dt * 1e3 / hz;
}

static void
fprof_disable(struct mali_fprof *p, const char *why)
{
   fprintf(p->out, "FRAMEPROF %s: frame profiling disabled\n", why);
   fflush(p->out);
   p->on = false;
}

void
mali_fprof_init(struct mali_fprof *p, struct mali_device *dev)
{
   memset(p, 0, sizeof *p);
   p->out = stderr;
   const char *e = getenv("MALI_FRAME_PROF");
   if (!e || !*e || !strcmp(e, "0"))
      return;
   p->on = true;
   if (strcmp(e, "1") && strcmp(e, "stderr")) {
      FILE *f = fopen(e, "a");
      if (f) {
         p->out = f;
         p->owned_out = true;
      }
   }
   p->hz = mali_device_pdev(dev)->gpu.ts_hz;
   uint64_t t;
   if (!mali_fprof_ticks(dev, &t))
      fprof_disable(p, "gpu_clock=none: no GPU timestamp counter");
}

void
mali_fprof_finish(struct mali_fprof *p)
{
   if (p->on) {
      const unsigned n = p->n_gpu_bound + p->n_cpu_bound;
      const uint64_t present_n = p->npresent > 1 ? p->npresent - 1 : 0;
      fprintf(p->out, "FRAMEPROF summary frames=%llu gpu_bound=%u cpu_bound=%u submit_us_avg=%.1f gpu_ms_avg=%.2f present_ms_avg=%.2f\n",
              (unsigned long long)p->seq, p->n_gpu_bound, p->n_cpu_bound,
              n ? p->sum_submit_us / n : 0.0, n ? p->sum_gpu_ms / n : 0.0,
              present_n ? p->sum_present_ms / present_n : 0.0);
      if (p->out)
         fflush(p->out);
      p->on = false;
   }
   if (p->owned_out) {
      fclose(p->out);
      p->owned_out = false;
      p->out = NULL;
   }
}

void
mali_fprof_kick(struct mali_fprof *p, struct mali_device *dev, uint64_t expected, unsigned pending)
{
   if (!p->on)
      return;
   uint64_t ticks = 0;
   if (!mali_fprof_ticks(dev, &ticks)) {
      fprof_disable(p, "gpu_clock=lost: GPU timestamp ioctl failed");
      return;
   }
   const uint64_t now = cpu_now_ns();
   if (p->have_kick)
      p->last_frame_ms = (double)(now - p->last_kick_cpu_ns) / 1e6;
   p->last_kick_cpu_ns = now;
   p->have_kick = true;
   if (p->nfl == MALI_FPROF_MAX) /* bookkeeping only (the queue bounds its own list): drop the oldest */
      memmove(p->fl, p->fl + 1, (MALI_FPROF_MAX - 1) * sizeof p->fl[0]), p->nfl--;
   /* Save this submission's own kick interval: last_frame_ms changes before it retires. */
   p->fl[p->nfl++] = (struct mali_fprof_fl){.expected = expected, .kick_ticks = ticks, .kick_cpu_ns = now,
                                            .frame_ms = p->last_frame_ms, .pending = pending};
}

void
mali_fprof_kicked(struct mali_fprof *p, double submit_ms)
{
   if (!p->on || !p->nfl)
      return;
   p->fl[p->nfl - 1].submit_us = submit_ms * 1e3;
}

void
mali_fprof_retired(struct mali_fprof *p, struct mali_device *dev, uint64_t counter)
{
   if (!p->on || !p->nfl || p->fl[0].expected > counter)
      return;
   uint64_t ticks = 0;
   if (!mali_fprof_ticks(dev, &ticks)) {
      fprof_disable(p, "gpu_clock=lost: GPU timestamp ioctl failed");
      return;
   }
   unsigned k = 0;
   while (k < p->nfl && p->fl[k].expected <= counter)
      k++;
   /* One observation closing k submissions gives them all the same timestamp. Distribute the observed span over the
    * saturated part of the batch instead of advancing the baseline per submission (which reports 0 for the rest). */
   unsigned nsat = 0;
   for (unsigned i = 0; i < k; i++)
      if (p->fl[i].pending > 0 && p->have_retire)
         nsat++;
   const double batch_ms = nsat > 1 ? ticks_to_ms(p, ticks - p->last_retire_ticks) / nsat : 0.0;
   for (unsigned i = 0; i < k; i++) {
      const double saved_frame_ms = p->fl[i].frame_ms;
      const double frame_ms = saved_frame_ms > 0 ? saved_frame_ms : ticks_to_ms(p, ticks - p->fl[i].kick_ticks);
      const bool sat = p->fl[i].pending > 0 && p->have_retire;
      double gpu_ms = sat ? (nsat > 1 ? batch_ms : ticks_to_ms(p, ticks - p->last_retire_ticks))
                          : ticks_to_ms(p, ticks - p->fl[i].kick_ticks);
      if (gpu_ms < 0)
         gpu_ms = 0;
      if (gpu_ms > frame_ms)
         gpu_ms = frame_ms; /* the frame cannot have been on the GPU longer than the app's frame interval */
      p->seq++;
      p->sum_submit_us += p->fl[i].submit_us;
      p->sum_gpu_ms += gpu_ms;
      if (sat)
         p->n_gpu_bound++;
      else
         p->n_cpu_bound++;
      fprintf(p->out, "FRAMEPROF f=%llu submit_us=%.1f gpu_ms=%.2f est=%s depth=%u present_ms=%.2f frame_ms=%.2f bound=%s\n",
              (unsigned long long)p->seq, p->fl[i].submit_us, gpu_ms, sat ? "sat" : "idle", p->fl[i].pending,
              p->have_present ? p->last_present_ms : 0.0, frame_ms, sat ? "gpu" : "cpu");
   }
   if (k) {
      p->last_retire_ticks = ticks; /* one baseline advance for the whole observed batch */
      p->have_retire = true;
      memmove(p->fl, p->fl + k, (p->nfl - k) * sizeof p->fl[0]);
      p->nfl -= k;
   }
}

void
mali_fprof_present(struct mali_fprof *p)
{
   if (!p->on)
      return;
   const uint64_t now = cpu_now_ns();
   if (p->have_present) {
      p->last_present_ms = (double)(now - p->last_present_cpu_ns) / 1e6;
      p->sum_present_ms += p->last_present_ms;
   }
   p->last_present_cpu_ns = now;
   p->have_present = true;
   p->npresent++;
}
