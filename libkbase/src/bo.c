/* SPDX-License-Identifier: MIT */
/* Layer B: GPU memory (kbase native allocations, dma-buf imports, dma-heap helper). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"

#define PAGE 4096u

static size_t round_page(size_t n) { return (n + PAGE - 1) & ~(size_t)(PAGE - 1); }

/* Opt-in (v13 bisect): raw kbase flags OR'd into every plain GPU_RD/WR allocation (not exec, not CSF_EVENT). 0 = unchanged behaviour. */
uint64_t kb_bo_extra_flags;

int kb_bo_alloc(struct kb_ctx *ctx, size_t size, uint32_t flags, struct kb_bo *out)
{
	memset(out, 0, sizeof *out);
	out->dmabuf_fd = -1;
	if (!size)
		return -EINVAL;
	size = round_page(size);

	uint64_t f = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;
	if (flags & KB_BO_GPU_RD) f |= BASE_MEM_PROT_GPU_RD;
	if (flags & KB_BO_GPU_WR) f |= BASE_MEM_PROT_GPU_WR;
	if (flags & KB_BO_GPU_EX) f |= BASE_MEM_PROT_GPU_EX;
	if (flags & KB_BO_CPU_CACHED) f |= BASE_MEM_CACHED_CPU;
	if (flags & KB_BO_CSF_EVENT) f |= BASE_MEM_CSF_EVENT;
	if (flags & KB_BO_GPU_UNCACHED) f |= BASE_MEM_UNCACHED_GPU;
	if (kb_bo_extra_flags && !(flags & (KB_BO_GPU_EX | KB_BO_CSF_EVENT)))
		f |= kb_bo_extra_flags;
	if (!(flags & KB_BO_GPU_EX))
		f |= BASE_MEM_SAME_VA; /* the kernel forces it for non-executable regions on 64-bit clients anyway */

	union kbase_ioctl_mem_alloc_ex req;
	memset(&req, 0, sizeof req);
	req.in.va_pages = size / PAGE;
	req.in.commit_pages = size / PAGE;
	req.in.flags = f;
	int r;
	if (ctx->caps.mem_alloc_ex) {
		r = kb_ioctl(ctx, KBASE_IOCTL_MEM_ALLOC_EX, &req);
	} else {
		union kbase_ioctl_mem_alloc lr;
		memset(&lr, 0, sizeof lr);
		lr.in.va_pages = req.in.va_pages;
		lr.in.commit_pages = req.in.commit_pages;
		lr.in.flags = req.in.flags;
		r = kb_ioctl(ctx, KBASE_IOCTL_MEM_ALLOC, &lr);
		req.out.flags = lr.out.flags;
		req.out.gpu_va = lr.out.gpu_va;
	}
	if (r) {
		kb_set_error("MEM_ALLOC_EX(%zu, flags 0x%llx): %s", size, (unsigned long long)f, strerror(-r));
		return r;
	}
	uint64_t of = req.out.flags, handle = req.out.gpu_va;
	/* SAME_VA: handle is a cookie, the mmap creates the region and its address is the GPU VA.
	 * EXEC_VA zone: handle is the real GPU VA and doubles as the mmap offset. */
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, (off_t)handle);
	if (p == MAP_FAILED) {
		int e = errno;
		struct kbase_ioctl_mem_free fr = { handle };
		kb_ioctl(ctx, KBASE_IOCTL_MEM_FREE, &fr);
		kb_set_error("mmap of BO cookie 0x%llx: %s", (unsigned long long)handle, strerror(e));
		return -e;
	}
	out->ctx = ctx;
	out->cpu = p;
	out->kmap = NULL;
	out->size = size;
	out->flags = flags;
	out->same_va = (of & BASE_MEM_SAME_VA) != 0;
	out->gpu_va = out->same_va ? (uint64_t)(uintptr_t)p : handle;
	ctx->live_bos++;
	return 0;
}

int kb_bo_import_dmabuf(struct kb_ctx *ctx, int dmabuf_fd, size_t size, uint32_t flags, struct kb_bo *out)
{
	memset(out, 0, sizeof *out);
	out->dmabuf_fd = -1;
	size = round_page(size);
	int fd = dup(dmabuf_fd);
	if (fd < 0)
		return -errno;

	uint64_t f = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_IMPORT_SHARED;
	if (flags & KB_BO_GPU_RD) f |= BASE_MEM_PROT_GPU_RD;
	if (flags & KB_BO_GPU_WR) f |= BASE_MEM_PROT_GPU_WR;
	union kbase_ioctl_mem_import req;
	memset(&req, 0, sizeof req);
	req.in.flags = f;
	req.in.phandle = (uint64_t)(uintptr_t)&fd; /* kernel reads the fd number through this pointer */
	req.in.type = BASE_MEM_IMPORT_TYPE_UMM;
	int r = kb_ioctl(ctx, KBASE_IOCTL_MEM_IMPORT, &req);
	if (r) {
		kb_set_error("MEM_IMPORT: %s", strerror(-r));
		close(fd);
		return r;
	}
	size_t bytes = (size_t)req.out.va_pages * PAGE;
	/* The kbase mapping creates the GPU VA. It must never be touched by the CPU (SIGBUS). */
	void *km = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, (off_t)req.out.gpu_va);
	if (km == MAP_FAILED) {
		int e = errno;
		struct kbase_ioctl_mem_free fr = { req.out.gpu_va };
		kb_ioctl(ctx, KBASE_IOCTL_MEM_FREE, &fr);
		close(fd);
		kb_set_error("mmap of import cookie: %s", strerror(e));
		return -e;
	}
	void *cp = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (cp == MAP_FAILED) {
		int e = errno;
		munmap(km, bytes);
		close(fd);
		kb_set_error("mmap of dma-buf: %s", strerror(e));
		return -e;
	}
	out->ctx = ctx;
	out->cpu = cp;
	out->kmap = km;
	out->size = bytes;
	out->flags = flags;
	out->dmabuf_fd = fd;
	out->same_va = true;
	out->gpu_va = (uint64_t)(uintptr_t)km;
	ctx->live_bos++;
	return 0;
}

void kb_bo_free(struct kb_bo *bo)
{
	if (!bo || !bo->ctx)
		return;
	struct kb_ctx *ctx = bo->ctx;
	if (bo->dmabuf_fd >= 0) {
		munmap(bo->cpu, bo->size);
		munmap(bo->kmap, bo->size); /* releases the GPU import */
		close(bo->dmabuf_fd);
	} else {
		munmap(bo->cpu, bo->size); /* SAME_VA: munmap frees the region */
		if (!bo->same_va) {
			struct kbase_ioctl_mem_free fr = { bo->gpu_va };
			kb_ioctl(ctx, KBASE_IOCTL_MEM_FREE, &fr);
		}
	}
	ctx->live_bos--;
	memset(bo, 0, sizeof *bo);
	bo->dmabuf_fd = -1;
}

bool kb_bo_gpu_va_live(struct kb_ctx *ctx, uint64_t gpu_va)
{
	union kbase_ioctl_mem_query q;
	memset(&q, 0, sizeof q);
	q.in.gpu_addr = gpu_va;
	q.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
	return kb_ioctl(ctx, KBASE_IOCTL_MEM_QUERY, &q) == 0;
}

void kb_bo_cache_flush_inv(const struct kb_bo *bo, size_t off, size_t len)
{
#if defined(__aarch64__)
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = 4u << ((ctr >> 16) & 0xf);
	uintptr_t a = ((uintptr_t)bo->cpu + off) & ~(uintptr_t)(line - 1);
	uintptr_t e = (uintptr_t)bo->cpu + off + len;
	__asm__ volatile("dsb sy" ::: "memory");
	for (; a < e; a += line)
		__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)bo; (void)off; (void)len;
	__sync_synchronize();
#endif
}

int kb_bo_dmabuf_sync(const struct kb_bo *bo, bool end, bool write)
{
	if (bo->dmabuf_fd < 0)
		return -EINVAL;
	struct kb_dma_buf_sync s = { (end ? KB_DMA_BUF_SYNC_END : KB_DMA_BUF_SYNC_START) |
				     (write ? KB_DMA_BUF_SYNC_RW : KB_DMA_BUF_SYNC_READ) };
	return ioctl(bo->dmabuf_fd, KB_DMA_BUF_IOCTL_SYNC, &s) < 0 ? -errno : 0;
}

int kb_dmaheap_alloc(enum kb_heap heap, size_t len)
{
	const char *path = heap == KB_HEAP_SYSTEM_UNCACHED ? "/dev/dma_heap/system-uncached" : "/dev/dma_heap/system";
	int h = open(path, O_RDONLY | O_CLOEXEC);
	if (h < 0) {
		kb_set_error("open(%s): %s", path, strerror(errno));
		return -errno;
	}
	struct kb_dma_heap_alloc a;
	memset(&a, 0, sizeof a);
	a.len = len;
	a.fd_flags = O_RDWR | O_CLOEXEC;
	int r = ioctl(h, KB_DMA_HEAP_IOCTL_ALLOC, &a);
	int e = errno;
	close(h);
	if (r < 0) {
		kb_set_error("DMA_HEAP_IOCTL_ALLOC(%s): %s", path, strerror(e));
		return -e;
	}
	return (int)a.fd;
}

int kb_dmabuf_export_sync_file(int dmabuf_fd, uint32_t flags, int *sync_fd)
{
	struct kb_dma_buf_export_sync_file a = { flags, -1 };
	if (ioctl(dmabuf_fd, KB_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &a) < 0)
		return -errno;
	*sync_fd = a.fd;
	return 0;
}

int kb_dmabuf_import_sync_file(int dmabuf_fd, uint32_t flags, int sync_fd)
{
	struct kb_dma_buf_import_sync_file a = { flags, sync_fd };
	return ioctl(dmabuf_fd, KB_DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &a) < 0 ? -errno : 0;
}
