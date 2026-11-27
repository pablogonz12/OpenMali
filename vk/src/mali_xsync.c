/* SPDX-License-Identifier: MIT */
/* Cross-process fences for dma-bufs on kernels without the dma-buf sync-file ioctls. See mali_xsync.h. */
#include "mali_xsync.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MALI_XSYNC_MAGIC 0x4d585331u   /* "MXS1" */
#define MALI_XSYNC_MAX_PUB 8           /* producer: outstanding publishes (one per handed-off buffer per frame) */
#define MALI_XSYNC_MAX_MAP 8           /* consumer: mapped pages */

/* One page per (device, inode). `pending` is the newest published write seqno, `done` the newest one known complete;
 * the producer is the only writer of both, consumers only read `pending` and futex-wait on `done`. */
struct mali_xsync_page {
   uint32_t magic;
   uint32_t pending;
   uint32_t done;
   uint32_t seq_base;   /* where this process's numbering continues from (see publish) */
   uint32_t pid;
   uint32_t pad[3];
};

struct xs_pub {
   int key;             /* st_ino of the dma-buf */
   char path[256];
   struct mali_xsync_page *page;
   int page_fd;
   int fence_fd;        /* dup of the submit's sync_file, polled by the next observe() */
   uint32_t seq;
   bool owned_file;     /* we created this page: unlink it at destroy */
};

struct xs_map {
   int key;
   char path[256];
   struct mali_xsync_page *page;
   int page_fd;
};

struct mali_xsync {
   char dir[200];
   struct xs_pub pub[MALI_XSYNC_MAX_PUB];
   unsigned npub;
   struct xs_map map[MALI_XSYNC_MAX_MAP];
   unsigned nmap;
   struct mali_xsync_stats st;
};

static int
futex_wait(uint32_t *addr, uint32_t val, int timeout_ms)
{
   struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
   return (int)syscall(SYS_futex, addr, FUTEX_WAIT, val, &ts, NULL, 0) < 0 ? -errno : 0;
}

static void
futex_wake_all(uint32_t *addr)
{
   syscall(SYS_futex, addr, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
}

/* st_dev/st_ino of the dma-buf fd: identical in every process that imported the same buffer. */
int
mali_xsync_path(const char *dir, int dmabuf_fd, char *buf, unsigned len)
{
   struct stat st;
   if (dmabuf_fd < 0 || fstat(dmabuf_fd, &st) != 0)
      return -errno;
   int n = snprintf(buf, len, "%s/mali-xsync-%llu-%llu", dir, (unsigned long long)st.st_dev,
                    (unsigned long long)st.st_ino);
   return n > 0 && (unsigned)n < len ? 0 : -ENAMETOOLONG;
}

static const char *
xsync_dir(void)
{
   const char *d = getenv("MALI_XSYNC_DIR");   /* tests */
   if (d && *d)
      return d;
   d = getenv("XDG_RUNTIME_DIR");
   if (d && *d)
      return d;
   if (access("/dev/shm", W_OK) == 0)
      return "/dev/shm";
   return "/tmp";
}

int
mali_xsync_dev_init(struct mali_xsync **out, const char *dir)
{
   *out = NULL;
   struct mali_xsync *x = calloc(1, sizeof *x);
   if (!x)
      return -ENOMEM;
   snprintf(x->dir, sizeof x->dir, "%s", dir && *dir ? dir : xsync_dir());
   if (access(x->dir, W_OK) != 0) {
      free(x);
      return -ENOENT;
   }
   for (unsigned i = 0; i < MALI_XSYNC_MAX_PUB; i++)
      x->pub[i].page_fd = x->pub[i].fence_fd = -1;
   for (unsigned i = 0; i < MALI_XSYNC_MAX_MAP; i++)
      x->map[i].page_fd = -1;
   *out = x;
   return 0;
}

void
mali_xsync_dev_finish(struct mali_xsync *x)
{
   if (!x)
      return;
   for (unsigned i = 0; i < MALI_XSYNC_MAX_PUB; i++) {
      struct xs_pub *p = &x->pub[i];
      if (p->fence_fd >= 0)
         close(p->fence_fd);
      if (p->page)
         munmap(p->page, 4096);
      if (p->page_fd >= 0)
         close(p->page_fd);
      if (p->owned_file && p->path[0])
         unlink(p->path);
   }
   for (unsigned i = 0; i < MALI_XSYNC_MAX_MAP; i++) {
      struct xs_map *m = &x->map[i];
      if (m->page)
         munmap(m->page, 4096);
      if (m->page_fd >= 0)
         close(m->page_fd);
   }
   free(x);
}

static struct mali_xsync_page *
xsync_open(struct mali_xsync *x, int dmabuf_fd, int *fd_out, char *path, unsigned path_len, bool *created)
{
   if (mali_xsync_path(x->dir, dmabuf_fd, path, path_len) != 0)
      return NULL;
   int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
   if (fd < 0)
      return NULL;
   struct stat st;
   bool is_new = fstat(fd, &st) == 0 && st.st_size == 0;
   if (is_new && ftruncate(fd, 4096) != 0) {
      close(fd);
      return NULL;
   }
   struct mali_xsync_page *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (p == MAP_FAILED) {
      close(fd);
      return NULL;
   }
   if (is_new) {
      memset(p, 0, 4096);
      p->magic = MALI_XSYNC_MAGIC;
      p->pid = (uint32_t)getpid();
   } else if (p->magic != MALI_XSYNC_MAGIC) {
      munmap(p, 4096);   /* somebody else's file, or a half-written one: start over */
      close(fd);
      unlink(path);
      return NULL;
   }
   *fd_out = fd;
   if (created)
      *created = is_new;
   return p;
}

void
mali_xsync_publish(struct mali_xsync *x, const struct mali_xsync_ops *ops, void *kdev, int dmabuf_fd, int fence_fd)
{
   (void)ops; (void)kdev;
   if (!x || dmabuf_fd < 0 || fence_fd < 0)
      return;
   char path[256];
   int key = 0;
   struct stat st;
   if (fstat(dmabuf_fd, &st) == 0)
      key = (int)st.st_ino;
   unsigned slot = MALI_XSYNC_MAX_PUB;
   for (unsigned i = 0; i < MALI_XSYNC_MAX_PUB; i++)
      if (x->pub[i].page_fd >= 0 && x->pub[i].key == key) {
         slot = i;
         break;
      }
   for (unsigned i = 0; i < MALI_XSYNC_MAX_PUB && slot == MALI_XSYNC_MAX_PUB; i++)
      if (x->pub[i].page_fd < 0)
         slot = i;
   if (slot == MALI_XSYNC_MAX_PUB) {
      /* full: recycle the first slot. Its fence is dropped unobserved, so its consumer falls back to the timeout. */
      slot = 0;
      if (x->pub[slot].fence_fd >= 0)
         close(x->pub[slot].fence_fd);
      x->pub[slot].fence_fd = -1;
      if (x->pub[slot].page)
         munmap(x->pub[slot].page, 4096);
      if (x->pub[slot].page_fd >= 0)
         close(x->pub[slot].page_fd);
      if (x->pub[slot].owned_file)
         unlink(x->pub[slot].path);
      memset(&x->pub[slot], 0, sizeof x->pub[slot]);
      x->pub[slot].page_fd = x->pub[slot].fence_fd = -1;
   }
   struct xs_pub *p = &x->pub[slot];
   if (p->page_fd < 0) {
      bool created = false;
      p->page = xsync_open(x, dmabuf_fd, &p->page_fd, path, sizeof path, &created);
      if (!p->page)
         return;
      snprintf(p->path, sizeof p->path, "%s", path);
      p->owned_file = created;
      p->key = key;
   }
   int d = fcntl(fence_fd, F_DUPFD_CLOEXEC, 3);
   if (d < 0)
      return;
   if (p->fence_fd >= 0)
      close(p->fence_fd);
   p->fence_fd = d;
   uint32_t seq = __atomic_add_fetch(&p->page->pending, 1, __ATOMIC_ACQ_REL);
   p->seq = seq;
   x->st.published++;
}

void
mali_xsync_observe(struct mali_xsync *x, const struct mali_xsync_ops *ops, void *kdev)
{
   if (!x || !ops || !ops->fence_wait)
      return;
   for (unsigned i = 0; i < MALI_XSYNC_MAX_PUB; i++) {
      struct xs_pub *p = &x->pub[i];
      if (!p->page || p->fence_fd < 0)
         continue;
      int r = ops->fence_wait(kdev, p->fence_fd, 0);   /* a poll: never blocks the producer */
      if (r != 0)
         continue;
      close(p->fence_fd);
      p->fence_fd = -1;
      if (__atomic_load_n(&p->page->done, __ATOMIC_RELAXED) != p->seq) {
         __atomic_store_n(&p->page->done, p->seq, __ATOMIC_RELEASE);
         futex_wake_all(&p->page->done);
         x->st.observed++;
      }
   }
}

bool
mali_xsync_await(struct mali_xsync *x, int dmabuf_fd, int timeout_ms, unsigned *timeouts)
{
   if (!x || dmabuf_fd < 0)
      return false;
   struct stat st;
   if (fstat(dmabuf_fd, &st) != 0)
      return false;
   const int key = (int)st.st_ino;
   struct xs_map *m = NULL;
   for (unsigned i = 0; i < x->nmap; i++)
      if (x->map[i].key == key) {
         m = &x->map[i];
         break;
      }
   if (!m) {
      if (x->nmap == MALI_XSYNC_MAX_MAP)
         return false;   /* out of slots: proceed unsynced rather than grow unbounded */
      m = &x->map[x->nmap];
      char path[256];
      m->page = xsync_open(x, dmabuf_fd, &m->page_fd, path, sizeof path, NULL);
      if (!m->page)
         return false;
      snprintf(m->path, sizeof m->path, "%s", path);
      m->key = key;
      x->nmap++;
   }
   uint32_t pending = __atomic_load_n(&m->page->pending, __ATOMIC_ACQUIRE);
   uint32_t done = __atomic_load_n(&m->page->done, __ATOMIC_ACQUIRE);
   if (pending == done)
      return false;   /* nothing published, or the producer's write is already complete */
   for (;;) {
      int r = futex_wait(&m->page->done, done, timeout_ms);
      done = __atomic_load_n(&m->page->done, __ATOMIC_ACQUIRE);
      pending = __atomic_load_n(&m->page->pending, __ATOMIC_ACQUIRE);
      if (pending == done) {
         x->st.awaited++;
         return true;
      }
      if (r == -ETIMEDOUT || r == -EAGAIN) {
         if (r == -EAGAIN)
            continue;   /* `done` moved: recheck, still inside the deadline only if it did not time out */
         x->st.timeouts++;
         if (timeouts)
            (*timeouts)++;
         return true;
      }
      if (r != 0)
         return true;   /* EINTR and friends: proceed, the producer's write may still be running */
   }
}

void
mali_xsync_stats(struct mali_xsync *x, struct mali_xsync_stats *out)
{
   if (x && out)
      *out = x->st;
   else if (out)
      memset(out, 0, sizeof *out);
}
