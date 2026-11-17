/* SPDX-License-Identifier: MIT */
/*
 * Minimal, hand-written subset of the Arm mali_kbase kernel UAPI used by probe.c.
 *
 * Only the ioctl numbers, structs and flag bits that the probe uses are
 * defined here; nothing is copied wholesale. The numbers/layouts describe the
 * kernel ABI of Arm's out-of-tree driver (GPL-2.0 WITH Linux-syscall-note UAPI
 * headers; interface facts are not copyrightable expression, definitions below
 * are our own).
 *
 * Sources (Arm DDK open kernel-driver UAPI, CSF "midgard" tree), cross-checked
 * against the copies vendored by zenithblue-oss/panvk-kbase-android
 * (patches/kbase-common/files/src/panfrost/lib/kmod/{kbase_uapi.h,
 *  kbase_csf_uapi.h, mali_base_csf_kernel.h, mali_base_kernel.h}, which carry
 * BASE_UK_VERSION 1.12 in their changelog, and kbase_kmod.c, whose runtime
 * paths use the same layouts on r4x devices):
 *   [K]   uapi/gpu/arm/midgard/mali_kbase_ioctl.h
 *   [KC]  uapi/gpu/arm/midgard/csf/mali_kbase_csf_ioctl.h
 *   [BC]  uapi/gpu/arm/midgard/csf/mali_base_csf_kernel.h
 *   [BK]  uapi/gpu/arm/midgard/mali_base_kernel.h
 *   [BM]  uapi/gpu/arm/midgard/mali_base_mem_priv.h / mali_base_common_kernel.h
 * Kernel-generic: [DH] linux/dma-heap.h, [UD] linux/udmabuf.h,
 *   [SS] linux/sync_file.h + drivers/dma-buf/sw_sync.c (staging uapi).
 * The kernel reports its own implemented version via VERSION_CHECK; the
 * layouts below are what existed up to csf uAPI 1.12+ for the used calls.
 */
#ifndef KBASE_UAPI_MIN_H
#define KBASE_UAPI_MIN_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KBASE_IOCTL_TYPE 0x80 /* [K] KBASE_IOCTL_TYPE */

/* [K]/[KC] struct kbase_ioctl_version_check. JM flavour is nr 0, CSF flavour nr 52. */
struct kbase_ioctl_version_check {
	__u16 major;
	__u16 minor;
};
#define KBASE_IOCTL_VERSION_CHECK_JM  _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_VERSION_CHECK_CSF _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)

/* [K] struct kbase_ioctl_set_flags (nr 1) */
struct kbase_ioctl_set_flags {
	__u32 create_flags;
};
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)

/* [K] struct kbase_ioctl_get_gpuprops (nr 3). size==0 -> returns blob size. */
struct kbase_ioctl_get_gpuprops {
	__u64 buffer;
	__u32 size;
	__u32 flags;
};
#define KBASE_IOCTL_GET_GPUPROPS _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

/* [K] gpuprops blob: u32 LE header = (key << 2) | size_code, then value (u8/u16/u32/u64 LE). */
#define KBASE_GPUPROP_VALUE_SIZE_U8  0x0
#define KBASE_GPUPROP_VALUE_SIZE_U16 0x1
#define KBASE_GPUPROP_VALUE_SIZE_U32 0x2
#define KBASE_GPUPROP_VALUE_SIZE_U64 0x3
/* Keys used by the probe ([K] KBASE_GPUPROP_*) */
#define KBASE_GPUPROP_PRODUCT_ID          1
#define KBASE_GPUPROP_VERSION_STATUS      2
#define KBASE_GPUPROP_MINOR_REVISION      3
#define KBASE_GPUPROP_MAJOR_REVISION      4
#define KBASE_GPUPROP_GPU_FREQ_KHZ_MAX    6
#define KBASE_GPUPROP_TEXTURE_FEATURES_0  9
#define KBASE_GPUPROP_L2_LOG2_LINE_SIZE   13
#define KBASE_GPUPROP_L2_LOG2_CACHE_SIZE  14
#define KBASE_GPUPROP_L2_NUM_L2_SLICES    15
#define KBASE_GPUPROP_MAX_THREADS         18
#define KBASE_GPUPROP_MAX_REGISTERS       21
#define KBASE_GPUPROP_RAW_SHADER_PRESENT  25
#define KBASE_GPUPROP_RAW_TILER_PRESENT   26
#define KBASE_GPUPROP_RAW_L2_PRESENT      27
#define KBASE_GPUPROP_RAW_STACK_PRESENT   28
#define KBASE_GPUPROP_RAW_L2_FEATURES     29
#define KBASE_GPUPROP_RAW_CORE_FEATURES   30
#define KBASE_GPUPROP_RAW_MEM_FEATURES    31
#define KBASE_GPUPROP_RAW_MMU_FEATURES    32
#define KBASE_GPUPROP_RAW_AS_PRESENT      33
#define KBASE_GPUPROP_RAW_JS_PRESENT      34
#define KBASE_GPUPROP_RAW_TILER_FEATURES  51
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0 52
#define KBASE_GPUPROP_RAW_GPU_ID          55
#define KBASE_GPUPROP_RAW_THREAD_FEATURES 59
#define KBASE_GPUPROP_RAW_COHERENCY_MODE  60
#define KBASE_GPUPROP_COHERENCY_NUM_GROUPS 61
#define KBASE_GPUPROP_COHERENCY_COHERENCY 63
#define KBASE_GPUPROP_NUM_EXEC_ENGINES    82
#define KBASE_GPUPROP_TLS_ALLOC           84
#define KBASE_GPUPROP_RAW_GPU_FEATURES    85

/* [K] union kbase_ioctl_mem_alloc (nr 5): legacy allocation. */
union kbase_ioctl_mem_alloc {
	struct {
		__u64 va_pages;
		__u64 commit_pages;
		__u64 extension;
		__u64 flags;
	} in;
	struct {
		__u64 flags;
		__u64 gpu_va;
	} out;
};
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)

/* [KC] union kbase_ioctl_mem_alloc_ex (nr 59, csf uAPI >= 1.9, per the fork's usage). */
union kbase_ioctl_mem_alloc_ex {
	struct {
		__u64 va_pages;
		__u64 commit_pages;
		__u64 extension;
		__u64 flags;
		__u64 fixed_address;
		__u64 extra[3];
	} in;
	struct {
		__u64 flags;
		__u64 gpu_va;
	} out;
};
#define KBASE_IOCTL_MEM_ALLOC_EX _IOWR(KBASE_IOCTL_TYPE, 59, union kbase_ioctl_mem_alloc_ex)

/* [K] union kbase_ioctl_mem_query (nr 6) */
union kbase_ioctl_mem_query {
	struct {
		__u64 gpu_addr;
		__u64 query;
	} in;
	struct {
		__u64 value;
	} out;
};
#define KBASE_IOCTL_MEM_QUERY _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)
#define KBASE_MEM_QUERY_COMMIT_SIZE 1
#define KBASE_MEM_QUERY_VA_SIZE     2
#define KBASE_MEM_QUERY_FLAGS       3

/* [K] struct kbase_ioctl_mem_free (nr 7) */
struct kbase_ioctl_mem_free {
	__u64 gpu_addr;
};
#define KBASE_IOCTL_MEM_FREE _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)

/* [K] union kbase_ioctl_mem_import (nr 22). For UMM, phandle = user pointer to an int dma-buf fd. */
union kbase_ioctl_mem_import {
	struct {
		__u64 flags;
		__u64 phandle;
		__u32 type;
		__u32 padding;
	} in;
	struct {
		__u64 flags;
		__u64 gpu_va;
		__u64 va_pages;
	} out;
};
#define KBASE_IOCTL_MEM_IMPORT _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)
#define BASE_MEM_IMPORT_TYPE_UMM 2 /* [BK] base_mem_import_type */

/* [K] struct kbase_ioctl_get_ddk_version (nr 13); returns string length */
struct kbase_ioctl_get_ddk_version {
	__u64 version_buffer;
	__u32 size;
	__u32 padding;
};
#define KBASE_IOCTL_GET_DDK_VERSION _IOW(KBASE_IOCTL_TYPE, 13, struct kbase_ioctl_get_ddk_version)

/* [K] struct kbase_ioctl_get_context_id (nr 17) - cheap, side-effect free */
struct kbase_ioctl_get_context_id {
	__u32 id;
};
#define KBASE_IOCTL_GET_CONTEXT_ID _IOR(KBASE_IOCTL_TYPE, 17, struct kbase_ioctl_get_context_id)

/* [K] union kbase_ioctl_get_cpu_gpu_timeinfo (nr 50) */
union kbase_ioctl_get_cpu_gpu_timeinfo {
	struct {
		__u32 request_flags;
		__u32 paddings[7];
	} in;
	struct {
		__u64 sec;
		__u32 nsec;
		__u32 padding;
		__u64 timestamp;
		__u64 cycle_counter;
	} out;
};
#define KBASE_IOCTL_GET_CPU_GPU_TIMEINFO _IOWR(KBASE_IOCTL_TYPE, 50, union kbase_ioctl_get_cpu_gpu_timeinfo)
#define BASE_TIMEINFO_MONOTONIC_FLAG (1u << 0) /* [BK] */
#define BASE_TIMEINFO_TIMESTAMP_FLAG (1u << 1)

/* [K] struct kbase_ioctl_context_priority_check (nr 54) */
struct kbase_ioctl_context_priority_check {
	__u8 priority;
};
#define KBASE_IOCTL_CONTEXT_PRIORITY_CHECK _IOWR(KBASE_IOCTL_TYPE, 54, struct kbase_ioctl_context_priority_check)

/* [BK] memory flags (base_mem_alloc_flags) */
#define BASE_MEM_PROT_CPU_RD       ((__u64)1 << 0)
#define BASE_MEM_PROT_CPU_WR       ((__u64)1 << 1)
#define BASE_MEM_PROT_GPU_RD       ((__u64)1 << 2)
#define BASE_MEM_PROT_GPU_WR       ((__u64)1 << 3)
#define BASE_MEM_PROT_GPU_EX       ((__u64)1 << 4)
#define BASE_MEM_GROW_ON_GPF       ((__u64)1 << 9)
#define BASE_MEM_COHERENT_SYSTEM   ((__u64)1 << 10)
#define BASE_MEM_COHERENT_LOCAL    ((__u64)1 << 11)
#define BASE_MEM_CACHED_CPU        ((__u64)1 << 12)
#define BASE_MEM_SAME_VA           ((__u64)1 << 13)
#define BASE_MEM_NEED_MMAP         ((__u64)1 << 14)
#define BASE_MEM_IMPORT_SHARED     ((__u64)1 << 18)
#define BASE_MEM_UNCACHED_GPU      ((__u64)1 << 21)
#define BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP ((__u64)1 << 26)

/* [KC] CSF: union kbase_ioctl_cs_get_glb_iface (nr 51) and its out-arrays. */
union kbase_ioctl_cs_get_glb_iface {
	struct {
		__u32 max_group_num;
		__u32 max_total_stream_num;
		__u64 groups_ptr;
		__u64 streams_ptr;
	} in;
	struct {
		__u32 glb_version;
		__u32 features;
		__u32 group_num;
		__u32 prfcnt_size;
		__u32 total_stream_num;
		__u32 instr_features;
	} out;
};
#define KBASE_IOCTL_CS_GET_GLB_IFACE _IOWR(KBASE_IOCTL_TYPE, 51, union kbase_ioctl_cs_get_glb_iface)
struct basep_cs_stream_control { /* [BC] */
	__u32 features;
	__u32 padding;
};
struct basep_cs_group_control { /* [BC] */
	__u32 features;
	__u32 stream_num;
	__u32 suspend_size;
	__u32 padding;
};

/* [KC] KCPU queues (nr 45..47). Commands are executed by the kernel, not the GPU. */
struct kbase_ioctl_kcpu_queue_new {
	__u8 id;
	__u8 padding[7];
};
#define KBASE_IOCTL_KCPU_QUEUE_CREATE _IOR(KBASE_IOCTL_TYPE, 45, struct kbase_ioctl_kcpu_queue_new)
struct kbase_ioctl_kcpu_queue_delete {
	__u8 id;
	__u8 padding[7];
};
#define KBASE_IOCTL_KCPU_QUEUE_DELETE _IOW(KBASE_IOCTL_TYPE, 46, struct kbase_ioctl_kcpu_queue_delete)
struct kbase_ioctl_kcpu_queue_enqueue {
	__u64 addr;       /* user pointer to array of struct base_kcpu_command */
	__u32 nr_commands;
	__u8 id;
	__u8 padding[3];
};
#define KBASE_IOCTL_KCPU_QUEUE_ENQUEUE _IOW(KBASE_IOCTL_TYPE, 47, struct kbase_ioctl_kcpu_queue_enqueue)

/* [BC] enum base_kcpu_command_type: FENCE_SIGNAL = 0, FENCE_WAIT = 1 */
#define BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL 0
#define BASE_KCPU_COMMAND_TYPE_FENCE_WAIT   1
struct base_fence { /* [BK] struct base_fence: basep.fd receives a sync_file fd for FENCE_SIGNAL */
	struct {
		int fd;
		int stream_fd;
	} basep;
};
struct base_kcpu_command { /* [BC] struct base_kcpu_command, 24 bytes; only .fence member used */
	__u8 type;
	__u8 padding[7];
	union {
		__u64 fence; /* user pointer to struct base_fence */
		__u64 pad[2];
	} info;
};

/* [DH] linux/dma-heap.h */
struct dma_heap_allocation_data_min {
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC_MIN _IOWR('H', 0x0, struct dma_heap_allocation_data_min)

/* [UD] linux/udmabuf.h */
struct udmabuf_create_min {
	__u32 memfd;
	__u32 flags;
	__u64 offset;
	__u64 size;
};
#define UDMABUF_CREATE_MIN _IOW('u', 0x42, struct udmabuf_create_min)
#define UDMABUF_FLAGS_CLOEXEC_MIN 0x01

/* [SS] sw_sync test timeline (/dev/sw_sync or debugfs sync/sw_sync) */
struct sw_sync_create_fence_data_min {
	__u32 value;
	char name[32];
	__s32 fence;
};
#define SW_SYNC_IOC_CREATE_FENCE_MIN _IOWR('W', 0, struct sw_sync_create_fence_data_min)
#define SW_SYNC_IOC_INC_MIN          _IOW('W', 1, __u32)

#endif
