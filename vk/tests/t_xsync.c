/* SPDX-License-Identifier: MIT */
/* Two-process test of the cross-process fence on a kernel without the dma-buf sync-file ioctls (mali_xsync.c).
 * The "dma-buf" is a memfd handed to the child through the inherited fd table; the fence is an eventfd, signalled by
 * the parent when the grandfather clock strikes, i.e. exactly the shape the driver uses: publish(pending), observe
 * (poll -> done + futex wake), await(done).
 *
 *  1. the reader blocks until the writer's write completes, and is woken (not by the timeout)
 *  2. the reader times out when the writer never completes, and proceeds
 *  3. a writer killed mid-frame leaves pending > done: the reader times out instead of hanging
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mali_xsync.h"

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long long
ms_now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000ll + t.tv_nsec / 1000000;
}

/* the fake kbase poll: 0 when the eventfd is signalled, -ETIMEDOUT otherwise */
static int
fk_fence_wait(void *kdev, int fence_fd, int timeout_ms)
{
   (void)kdev;
   struct pollfd pf = {fence_fd, POLLIN, 0};
   int r = poll(&pf, 1, timeout_ms);
   if (r < 0)
      return -errno;
   return r > 0 ? 0 : -ETIMEDOUT;
}

static const struct mali_xsync_ops ops = {.fence_wait = fk_fence_wait};

/* Child: await the buffer, report how long it took and whether it timed out. */
static int
child_await(const char *dir, int dmabuf, int timeout_ms, long long *waited, unsigned *timeouts)
{
   struct mali_xsync *x = NULL;
   if (mali_xsync_dev_init(&x, dir) != 0)
      return 2;
   long long t0 = ms_now();
   mali_xsync_await(x, dmabuf, timeout_ms, timeouts);
   *waited = ms_now() - t0;
   mali_xsync_dev_finish(x);
   _exit(0);
}

int
main(void)
{
   char dir[] = "/tmp/mali-xsync-test-XXXXXX";
   if (!mkdtemp(dir)) { printf("FAIL  mkdtemp\n"); return 1; }
   int buf = memfd_create("dmabuf", MFD_CLOEXEC);
   if (buf < 0) { printf("FAIL  memfd\n"); return 1; }

   /* 1: writer publishes a pending write, the child waits, the writer's fence signals and observe() wakes it */
   {
      int ef = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      struct mali_xsync *w = NULL;
      EXPECT(mali_xsync_dev_init(&w, dir) == 0, "writer init");
      mali_xsync_publish(w, &ops, NULL, buf, ef);
      int pfd[2];
      if (pipe(pfd) != 0) { printf("FAIL  pipe\n"); return 1; }
      pid_t pid = fork();
      if (pid == 0) {
         struct mali_xsync *c = NULL;
         mali_xsync_dev_init(&c, dir);
         long long t0 = ms_now();
         unsigned to = 0;
         mali_xsync_await(c, buf, 500, &to);
         long long waited = ms_now() - t0;
         struct { long long w; unsigned t; } r = { waited, to };
         if (write(pfd[1], &r, sizeof r) < 0) {}
         _exit(0);
      }
      close(pfd[1]);
      usleep(60000);
      uint64_t one = 1;
      if (write(ef, &one, sizeof one) < 0) {}
      mali_xsync_observe(w, &ops, NULL);   /* the producer's next submit: poll, mark done, wake */
      struct { long long w; unsigned t; } rep = { 0, 0 };
      long long rd = read(pfd[0], &rep, sizeof rep);
      close(pfd[0]);
      int status = 0;
      waitpid(pid, &status, 0);
      EXPECT(rd == (long long)sizeof rep && rep.w >= 40 && rep.w < 400 && rep.t == 0,
             "the reader waited for the write and was woken by observe(), not by the timeout (%lld ms, %u timeouts)",
             rep.w, rep.t);
      struct mali_xsync_stats st;
      mali_xsync_stats(w, &st);
      EXPECT(st.published == 1 && st.observed == 1, "writer published and observed (%u/%u)", st.published, st.observed);
      EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "reader exited cleanly");
      close(ef);
      mali_xsync_dev_finish(w);
   }

   /* 2: the writer never completes: the reader times out, counts it, and proceeds */
   {
      int ef = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      struct mali_xsync *w = NULL;
      mali_xsync_dev_init(&w, dir);
      mali_xsync_publish(w, &ops, NULL, buf, ef);
      pid_t pid = fork();
      if (pid == 0)
         child_await(dir, buf, 100, &(long long){0}, &(unsigned){0});
      int status = 0;
      long long t0 = ms_now();
      waitpid(pid, &status, 0);
      long long took = ms_now() - t0;
      EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "reader exited after the timeout (%lld ms)", took);
      EXPECT(took >= 90 && took < 400, "the wait was bounded by the timeout (%lld ms)", took);
      close(ef);
      mali_xsync_dev_finish(w);   /* unlinks the page: the stale pending must not outlive us */
   }

   /* 3: a writer that dies mid-frame leaves pending > done; the reader must time out, never hang */
   {
      int ef = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      pid_t wpid = fork();
      if (wpid == 0) {
         struct mali_xsync *w = NULL;
         mali_xsync_dev_init(&w, dir);
         mali_xsync_publish(w, &ops, NULL, buf, ef);
         _exit(0);   /* dies with the write still in flight: nothing will ever set done */
      }
      int wstatus = 0;
      waitpid(wpid, &wstatus, 0);
      struct mali_xsync *w = NULL;
      mali_xsync_dev_init(&w, dir);
      mali_xsync_publish(w, &ops, NULL, buf, ef);   /* same page: pending keeps growing */
      close(ef);
      pid_t pid = fork();
      int tmo[2];
      if (pid == 0) {
         unsigned to = 0;
         long long waited = 0;
         child_await(dir, buf, 100, &waited, &to);
      }
      int status = 0;
      long long t0 = ms_now();
      waitpid(pid, &status, 0);
      long long took = ms_now() - t0;
      (void)tmo;
      EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "reader survives a writer that died mid-frame (%lld ms)", took);
      EXPECT(took >= 90 && took < 400, "and proceeds after the timeout (%lld ms)", took);
      mali_xsync_dev_finish(w);
   }

   close(buf);
   rmdir(dir);
   printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
   return fails != 0;
}
