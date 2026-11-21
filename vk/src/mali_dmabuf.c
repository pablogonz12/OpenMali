/* SPDX-License-Identifier: MIT */
/* True when fd is an open dma-buf; used by mali_memory.c for VkImportMemoryFdInfoKHR and
 * vkGetMemoryFdPropertiesKHR, which both refuse anything else with VK_ERROR_INVALID_EXTERNAL_HANDLE.
 *
 * Two arms, because one alone is wrong on a platform we ship to:
 *
 * - statfs magic (DMA_BUF_MAGIC): the arm 070ad057 added after review, because the ioctl arm below alone accepted
 *   /dev/kvm - KVM's ioctl handler answers EINVAL to *any* unknown ioctl, where an fd that is not a dma-buf answers
 *   ENOTTY (PROVEN on the host: /dev/kvm gives EINVAL, a memfd gives ENOTTY). On a desktop kernel a real dma-buf is
 *   on the dmabuf pseudo filesystem, so this arm is exact.
 * - ioctl fallback: inside the DroidDeck guest on the phone (X8, Mali-G925) a real dma-buf fails the statfs arm -
 *   the guest's path translation answers statfs itself - so the import path returned -EBADF and every gamescope
 *   client texture was dropped (zink: "couldn't allocate memory"; no session reached READY). DMA_BUF_IOCTL_SYNC
 *   with flags = 0 makes the dma-buf core validate the flags before doing anything: a dma-buf answers EINVAL and
 *   nothing is synced, so the probe has no side effect. Deliberately not DMA_BUF_SET_NAME (renames the buffer) and
 *   not DMA_BUF_IOCTL_EXPORT_SYNC_FILE (allocates a file and a fence).
 *
 * To keep the /dev/kvm hole closed, the fallback also refuses character devices: a dma-buf is never one (udmabuf on
 * the host reports mode 0600, a regular file). memfd/shm and regular files are S_IFREG and answer ENOTTY, a pipe
 * answers ENOTTY: refused by both arms.
 *
 * MALI_TEST_NO_STATFS_MAGIC=1 disables the magic arm so the host test (vk/tests/t_dmabuf.c) can prove the fallback
 * still accepts a real dma-buf. Test hook only, never set in a session. */

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/magic.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "mali_dmabuf.h"

static bool
statfs_arm(int fd)
{
   struct statfs sf;
   return fstatfs(fd, &sf) == 0 && (unsigned long)sf.f_type == DMA_BUF_MAGIC;
}

static bool
ioctl_arm(int fd)
{
   struct dma_buf_sync s = {.flags = 0}; /* no direction: the core rejects the flags, nothing is synced */
   return ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) < 0 && errno == EINVAL;
}

bool
mali_dmabuf_probe(int fd)
{
   if (fd < 0 || fcntl(fd, F_GETFD) < 0)
      return false;
   if (!getenv("MALI_TEST_NO_STATFS_MAGIC") && statfs_arm(fd))
      return true;
   if (!ioctl_arm(fd))
      return false;
   struct stat st;
   if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode))
      return false; /* /dev/kvm and friends: unknown ioctls answer EINVAL, but a dma-buf is not a char device */
   return true;
}
