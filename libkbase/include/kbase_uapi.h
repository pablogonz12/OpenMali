/* SPDX-License-Identifier: MIT */
/*
 * libkbase: our own minimal definition of the Arm mali_kbase (CSF) kernel UAPI.
 *
 * Only the ioctl numbers, struct layouts and flag bits libkbase uses. These are
 * kernel ABI facts; the declarations are written by us. Public documents they
 * were derived from / cross-checked against:
 *   [K]  Arm kbase uapi/gpu/arm/midgard/mali_kbase_ioctl.h
 *   [KC] Arm kbase uapi/gpu/arm/midgard/csf/mali_kbase_csf_ioctl.h
 *   [BC] Arm kbase uapi/gpu/arm/midgard/csf/mali_base_csf_kernel.h
 *   [BK] Arm kbase uapi/gpu/arm/midgard/mali_base_kernel.h
 *   [PV] zenithblue-oss/panvk-kbase-android kbase_kmod.c (how these are driven)
 *   [DH] linux/dma-heap.h, [DB] linux/dma-buf.h, [SF] linux/sync_file.h
 * Verified at runtime against uAPI 1.21 (CSF) on a MediaTek/Mali-G615 phone.
 * uAPI 1.30 (Poco, DDK r49p1) layouts below are taken from Arm's public headers and NOT yet verified on hardware:
 *   [KC30] mali_kbase_csf_ioctl.h changelog 1.22..1.30 (e.g. rockchip-linux/kernel develop-6.1
 *          include/uapi/gpu/arm/valhall/csf/, android.googlesource.com kernel/google-modules/gpu
 *          common/include/uapi/gpu/arm/midgard/csf/). Between 1.21 and 1.30 the only ioctl-struct change listed is
 *          1.22 (comp_pri_threshold/comp_pri_ratio in QUEUE_GROUP_CREATE) and 1.25 (CS_FAULT reporting: new
 *          cs_fault_report_enable field + notification type); 1.23/1.26/1.28 only change memory-flag rules,
 *          1.24/1.27 HWC, 1.29 child-process fd access, 1.30 GPU timestamp offset (no struct we use).
 *   CAVEAT: the public headers show the 112-byte group-create (NE fields, "1.19") and the 24-byte tiler-heap-init
 *   ("1.14") already below 1.21, yet the vendor r44p1 kernel at 1.21 accepted the 40-byte and 16-byte forms. The version
 *   numbers of the public trees therefore do not map 1:1 onto vendor kernels; kb_open keys on (major,minor) and the
 *   queue/heap code falls back to the older size on ENOTTY (size is part of the ioctl number).
 */
#ifndef KBASE_UAPI_H
#define KBASE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KBASE_IOCTL_TYPE 0x80 /* [K] */

/* ---- handshake / context ---- */
struct kbase_ioctl_version_check { __u16 major; __u16 minor; };           /* [KC] */
#define KBASE_IOCTL_VERSION_CHECK_CSF _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_VERSION_CHECK_JM  _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)

struct kbase_ioctl_set_flags { __u32 create_flags; };                     /* [K] */
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define BASE_CONTEXT_CSF_EVENT_THREAD (1u << 2)                           /* [BC] */

struct kbase_ioctl_get_gpuprops { __u64 buffer; __u32 size; __u32 flags; }; /* [K] */
#define KBASE_IOCTL_GET_GPUPROPS _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

struct kbase_ioctl_get_ddk_version { __u64 version_buffer; __u32 size; __u32 padding; }; /* [K] */
#define KBASE_IOCTL_GET_DDK_VERSION _IOW(KBASE_IOCTL_TYPE, 13, struct kbase_ioctl_get_ddk_version)

/* gpuprops blob: u32 LE header = (key << 2) | size_code, then the LE value. [K] */
#define KBASE_GPUPROP_PRODUCT_ID           1
#define KBASE_GPUPROP_MINOR_REVISION       3
#define KBASE_GPUPROP_MAJOR_REVISION       4
#define KBASE_GPUPROP_GPU_FREQ_KHZ_MAX     6
#define KBASE_GPUPROP_MAX_THREADS          18
#define KBASE_GPUPROP_MAX_REGISTERS        21
#define KBASE_GPUPROP_RAW_SHADER_PRESENT   25
#define KBASE_GPUPROP_RAW_TILER_PRESENT    26
#define KBASE_GPUPROP_RAW_L2_PRESENT       27
#define KBASE_GPUPROP_RAW_STACK_PRESENT    28
#define KBASE_GPUPROP_RAW_L2_FEATURES      29
#define KBASE_GPUPROP_RAW_MEM_FEATURES     31
#define KBASE_GPUPROP_RAW_MMU_FEATURES     32
#define KBASE_GPUPROP_RAW_AS_PRESENT       33
#define KBASE_GPUPROP_RAW_TILER_FEATURES   51
#define KBASE_GPUPROP_RAW_GPU_ID           55
#define KBASE_GPUPROP_COHERENCY_NUM_GROUPS 61

/* ---- memory ---- */
union kbase_ioctl_mem_alloc_ex {                                          /* [KC] nr 59 */
	struct { __u64 va_pages, commit_pages, extension, flags, fixed_address, extra[3]; } in;
	struct { __u64 flags, gpu_va; } out;
};
#define KBASE_IOCTL_MEM_ALLOC_EX _IOWR(KBASE_IOCTL_TYPE, 59, union kbase_ioctl_mem_alloc_ex)

/* Legacy MEM_ALLOC (nr 5): the only allocator before MEM_ALLOC_EX; uAPI 1.18 (r32) has no nr 59. Same field layout for the 4 words we use. */
union kbase_ioctl_mem_alloc {
	struct { __u64 va_pages, commit_pages, extension, flags; } in;
	struct { __u64 flags, gpu_va; } out;
};
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)

union kbase_ioctl_mem_query {                                             /* [K] nr 6 */
	struct { __u64 gpu_addr; __u64 query; } in;
	struct { __u64 value; } out;
};
#define KBASE_IOCTL_MEM_QUERY _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)
#define KBASE_MEM_QUERY_COMMIT_SIZE 1

struct kbase_ioctl_mem_free { __u64 gpu_addr; };                          /* [K] */
#define KBASE_IOCTL_MEM_FREE _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)

union kbase_ioctl_mem_import {                                            /* [K] nr 22 */
	struct { __u64 flags; __u64 phandle; __u32 type; __u32 padding; } in;
	struct { __u64 flags; __u64 gpu_va; __u64 va_pages; } out;
};
#define KBASE_IOCTL_MEM_IMPORT _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)
#define BASE_MEM_IMPORT_TYPE_UMM 2                                        /* [BK] */

/* [BK]/[BC] base_mem_alloc_flags */
#define BASE_MEM_PROT_CPU_RD     ((__u64)1 << 0)
#define BASE_MEM_PROT_CPU_WR     ((__u64)1 << 1)
#define BASE_MEM_PROT_GPU_RD     ((__u64)1 << 2)
#define BASE_MEM_PROT_GPU_WR     ((__u64)1 << 3)
#define BASE_MEM_PROT_GPU_EX     ((__u64)1 << 4)
#define BASE_MEM_COHERENT_SYSTEM ((__u64)1 << 10)
#define BASE_MEM_COHERENT_LOCAL  ((__u64)1 << 11)
#define BASE_MEM_CACHED_CPU      ((__u64)1 << 12)
#define BASE_MEM_SAME_VA         ((__u64)1 << 13)
#define BASE_MEM_NEED_MMAP       ((__u64)1 << 14)
#define BASE_MEM_IMPORT_SHARED   ((__u64)1 << 18)
#define BASE_MEM_CSF_EVENT       ((__u64)1 << 19) /* [BC] CQS / event memory, permanently kernel-mapped */
#define BASE_MEM_UNCACHED_GPU    ((__u64)1 << 21)

#define BASE_MEM_MAP_TRACKING_HANDLE        (3ull << 12)  /* [BK] mmap offset; needed before memory ops */
#define BASEP_MEM_CSF_USER_REG_PAGE_HANDLE  (47ull << 12) /* [BC] */

/* ---- CSF global interface ---- */
union kbase_ioctl_cs_get_glb_iface {                                      /* [KC] nr 51 */
	struct { __u32 max_group_num; __u32 max_total_stream_num; __u64 groups_ptr; __u64 streams_ptr; } in;
	struct { __u32 glb_version, features, group_num, prfcnt_size, total_stream_num, instr_features; } out;
};
#define KBASE_IOCTL_CS_GET_GLB_IFACE _IOWR(KBASE_IOCTL_TYPE, 51, union kbase_ioctl_cs_get_glb_iface)
struct basep_cs_stream_control { __u32 features, padding; };              /* [BC] */
struct basep_cs_group_control { __u32 features, stream_num, suspend_size, padding; }; /* [BC] */

/* ---- CSF queues / groups [KC] ---- */
struct kbase_ioctl_cs_queue_register { __u64 buffer_gpu_addr; __u32 buffer_size; __u8 priority; __u8 padding[3]; };
#define KBASE_IOCTL_CS_QUEUE_REGISTER _IOW(KBASE_IOCTL_TYPE, 36, struct kbase_ioctl_cs_queue_register)
struct kbase_ioctl_cs_queue_kick { __u64 buffer_gpu_addr; };
#define KBASE_IOCTL_CS_QUEUE_KICK _IOW(KBASE_IOCTL_TYPE, 37, struct kbase_ioctl_cs_queue_kick)
union kbase_ioctl_cs_queue_bind {
	struct { __u64 buffer_gpu_addr; __u8 group_handle; __u8 csi_index; __u8 padding[6]; } in;
	struct { __u64 mmap_handle; } out;
};
#define KBASE_IOCTL_CS_QUEUE_BIND _IOWR(KBASE_IOCTL_TYPE, 39, union kbase_ioctl_cs_queue_bind)
struct kbase_ioctl_cs_queue_terminate { __u64 buffer_gpu_addr; };
#define KBASE_IOCTL_CS_QUEUE_TERMINATE _IOW(KBASE_IOCTL_TYPE, 41, struct kbase_ioctl_cs_queue_terminate)

/* 40-byte layout valid for uAPI 1.7 .. 1.21 (the 1.25+ layout is 112 bytes and a different ioctl size). */
union kbase_ioctl_cs_queue_group_create {
	struct {
		__u64 tiler_mask, fragment_mask, compute_mask;
		__u8 cs_min, priority, tiler_max, fragment_max, compute_max, csi_handlers, padding[2];
		__u64 dvs_buf; /* 1.18: "reserved" before */
	} in;
	struct { __u8 group_handle; __u8 padding[3]; __u32 group_uid; } out;
};
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create)

/* 112-byte layout (same nr 58, different size => different ioctl number). [KC30] union kbase_ioctl_cs_queue_group_create
 * (field names "neural" = the public header's NE/neural-engine fields; unused on these GPUs, left 0).
 * 1.22 added comp_pri_threshold/comp_pri_ratio, 1.25 made cs_fault_report_enable meaningful (0 = no CS_FAULT notifications). */
union kbase_ioctl_cs_queue_group_create_112 {
	struct {
		__u64 tiler_mask, fragment_mask, compute_mask;
		__u8 cs_min, priority, tiler_max, fragment_max, compute_max, csi_handlers, neural_max, cs_fault_report_enable;
		__u64 dvs_buf, neural_mask;
		__u8 comp_pri_threshold, comp_pri_ratio, padding[62];
	} in;
	struct { __u8 group_handle; __u8 padding[3]; __u32 group_uid; } out;
};
/* uAPI 1.18 (r32p1, Poco X4 GT): the create is nr 42, 32 bytes (upstream CREATE_1_6), found by an ioctl size scan on the phone. */
union kbase_ioctl_cs_queue_group_create_1_6 {
	struct { __u64 tiler_mask, fragment_mask, compute_mask; __u8 cs_min, priority, tiler_max, fragment_max, compute_max, padding[3]; } in;
	struct { __u8 group_handle; __u8 padding[3]; __u32 group_uid; } out;
};
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 _IOWR(KBASE_IOCTL_TYPE, 42, union kbase_ioctl_cs_queue_group_create_1_6)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_112 _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create_112)
struct kbase_ioctl_cs_queue_group_term { __u8 group_handle; __u8 padding[7]; };
#define KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE _IOW(KBASE_IOCTL_TYPE, 43, struct kbase_ioctl_cs_queue_group_term)

/* [BC] enum base_queue_group_priority */
#define BASE_QUEUE_GROUP_PRIORITY_HIGH   0
#define BASE_QUEUE_GROUP_PRIORITY_MEDIUM 1
#define BASE_QUEUE_GROUP_PRIORITY_LOW    2

/* mmap of the bind cookie: 3 pages. Layout per [PV]: 0 doorbell, 1 input (CS_INSERT @0, EXTRACT_INIT @8), 2 output (CS_EXTRACT @0, CS_ACTIVE @8). */
#define BASEP_QUEUE_NR_MMAP_USER_PAGES 3

/* ---- CSF notifications (read() on the kbase fd, 64 bytes each) [BC] ---- */
struct base_gpu_queue_group_error_fatal_payload { __u64 sideband; __u32 status; __u32 padding; };
struct base_gpu_queue_error_fatal_payload { __u64 sideband; __u32 status; __u8 csi_index; __u8 padding[3]; };
struct base_gpu_queue_group_error {
	__u8 error_type; __u8 padding[7];
	union { struct base_gpu_queue_group_error_fatal_payload fatal_group; struct base_gpu_queue_error_fatal_payload fatal_queue; } payload;
};
struct base_csf_notification {
	__u8 type; __u8 padding[7];
	union {
		struct { __u8 handle; __u8 padding[7]; struct base_gpu_queue_group_error error; } csg_error;
		__u8 align[56];
	} payload;
};
#define BASE_CSF_NOTIFICATION_EVENT 0
#define BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR 1
#define BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP 2
#define BASE_GPU_QUEUE_GROUP_ERROR_FATAL 0
#define BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL 1
#define BASE_GPU_QUEUE_GROUP_ERROR_TIMEOUT 2
#define BASE_GPU_QUEUE_GROUP_ERROR_TILER_HEAP_OOM 3
#define BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT 4 /* [KC30]/[BC] 1.25 CS_FAULT reporting; only sent if cs_fault_report_enable != 0, which we never set */

/* ---- KCPU queues [KC]/[BC] ---- */
struct kbase_ioctl_kcpu_queue_new { __u8 id; __u8 padding[7]; };
#define KBASE_IOCTL_KCPU_QUEUE_CREATE _IOR(KBASE_IOCTL_TYPE, 45, struct kbase_ioctl_kcpu_queue_new)
struct kbase_ioctl_kcpu_queue_delete { __u8 id; __u8 padding[7]; };
#define KBASE_IOCTL_KCPU_QUEUE_DELETE _IOW(KBASE_IOCTL_TYPE, 46, struct kbase_ioctl_kcpu_queue_delete)
struct kbase_ioctl_kcpu_queue_enqueue { __u64 addr; __u32 nr_commands; __u8 id; __u8 padding[3]; };
#define KBASE_IOCTL_KCPU_QUEUE_ENQUEUE _IOW(KBASE_IOCTL_TYPE, 47, struct kbase_ioctl_kcpu_queue_enqueue)

#define BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL        0
#define BASE_KCPU_COMMAND_TYPE_FENCE_WAIT          1
#define BASE_KCPU_COMMAND_TYPE_CQS_WAIT            2
#define BASE_KCPU_COMMAND_TYPE_CQS_SET             3
#define BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION  4
#define BASE_KCPU_COMMAND_TYPE_CQS_SET_OPERATION   5
struct base_fence { int fd; int stream_fd; };                              /* kernel fills fd for FENCE_SIGNAL */
struct base_cqs_wait_operation_info { __u64 addr; __u64 val; __u8 operation; __u8 data_type; __u8 padding[6]; };
struct base_cqs_set_operation_info  { __u64 addr; __u64 val; __u8 operation; __u8 data_type; __u8 padding[6]; };
#define BASEP_CQS_DATA_TYPE_U32 0
#define BASEP_CQS_DATA_TYPE_U64 1
#define BASEP_CQS_WAIT_OPERATION_LE 0
#define BASEP_CQS_WAIT_OPERATION_GT 1
#define BASEP_CQS_SET_OPERATION_ADD 0
#define BASEP_CQS_SET_OPERATION_SET 1
struct base_kcpu_command {                                                /* 24 bytes */
	__u8 type; __u8 padding[7];
	union {
		__u64 fence;                                                      /* -> struct base_fence */
		struct { __u64 objs; __u32 nr_objs; __u32 inherit_err_flags; } cqs_wait_operation;
		struct { __u64 objs; __u32 nr_objs; __u32 padding; } cqs_set_operation;
		__u64 pad[2];
	} info;
};

/* ---- generic kernel UAPI ---- */
struct kb_dma_heap_alloc { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; }; /* [DH] */
#define KB_DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct kb_dma_heap_alloc)

struct kb_dma_buf_sync { __u64 flags; };                                  /* [DB] */
#define KB_DMA_BUF_SYNC_READ  (1u << 0)
#define KB_DMA_BUF_SYNC_WRITE (2u << 0)
#define KB_DMA_BUF_SYNC_RW    (3u << 0)
#define KB_DMA_BUF_SYNC_START (0u << 2)
#define KB_DMA_BUF_SYNC_END   (1u << 2)
#define KB_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct kb_dma_buf_sync)
struct kb_dma_buf_export_sync_file { __u32 flags; __s32 fd; };            /* [DB] kernel >= 6.0 */
struct kb_dma_buf_import_sync_file { __u32 flags; __s32 fd; };
#define KB_DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct kb_dma_buf_export_sync_file)
#define KB_DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW('b', 3, struct kb_dma_buf_import_sync_file)


/* ROOT CAUSE of the old CS_TILER_HEAP_INIT ENOMEM (verified on hardware, runs/20261115-cmf-libkbase/triangle-r7/r8-*.jsonl):
 * the kernel takes tiler-heap chunks from the context's JIT area, so MEM_JIT_INIT must have been called on the context first.
 * Without it every heap init (any chunk size / layout) returns ENOMEM; with it the identical call succeeds.
 * MEM_EXEC_INIT is NOT needed for the heap (r8-KB_SKIP_EXEC_INIT) but PanVK issues it too, so we do the same.
 * PanVK order after the handshake: VERSION_CHECK_CSF -> SET_FLAGS(0) -> mmap(tracking) -> GET_GPUPROPS x2 -> MEM_EXEC_INIT ->
 * MEM_JIT_INIT -> GET_GLB_IFACE x2. See driver/libkbase/PANVK-SEQUENCE.md.
 * Context memory-zone setup, as issued by PanVK on this phone (captured with driver/tools/ioctl_trace.c). */
struct kbase_ioctl_mem_exec_init { __u64 va_pages; };
#define KBASE_IOCTL_MEM_EXEC_INIT _IOW(KBASE_IOCTL_TYPE, 38, struct kbase_ioctl_mem_exec_init)
struct kbase_ioctl_mem_jit_init {
	__u64 va_pages; __u8 max_allocations; __u8 trim_level; __u8 group_id; __u8 padding[5]; __u64 phys_pages;
};
#define KBASE_IOCTL_MEM_JIT_INIT _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)

/* CSF tiler heap. PanVK trace (panvk-trace/vkdraw-trace1.jsonl): ioctl size 16, nr 48; in = 00001000 0a000000 90010000 ffff 00 00 =
 * chunk_size 0x100000 (1 MiB), initial_chunks 10, max_chunks 400, target_in_flight 0xffff, group_id 0; out gpu_heap_va 0x6000000000,
 * first_chunk_va 0x6000002000 (a 2nd heap created on a worker thread: 0x6000000040 / 0x6000a02000, i.e. 64-byte heap contexts).
 * The 24-byte (buf_desc_va) layout was never used by PanVK on this kernel ("kbase: tiler_heap_init layout=16 bytes").
 * (16-byte in layout, as used by the DDK r44 on uAPI 1.21; Arm mali_kbase_csf_ioctl.h). The heap context VA returned in
 * out.gpu_heap_va is what the command stream's HEAP_SET takes; out.first_chunk_va is the first chunk of the heap. */
union kbase_ioctl_cs_tiler_heap_init {
	struct {
		uint32_t chunk_size;
		uint32_t initial_chunks;
		uint32_t max_chunks;
		uint16_t target_in_flight;
		uint8_t group_id;
		uint8_t padding;
	} in;
	struct {
		uint64_t gpu_heap_va;
		uint64_t first_chunk_va;
	} out;
};
#define KBASE_IOCTL_CS_TILER_HEAP_INIT _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init)
/* 24-byte layout, [KC30] 1.14 "buffer descriptor VA in tiler heap init" (same nr 48, size 24 => different ioctl number). */
union kbase_ioctl_cs_tiler_heap_init_24 {
	struct { uint32_t chunk_size, initial_chunks, max_chunks; uint16_t target_in_flight; uint8_t group_id, padding; uint64_t buf_desc_va; } in;
	struct { uint64_t gpu_heap_va, first_chunk_va; } out;
};
#define KBASE_IOCTL_CS_TILER_HEAP_INIT_24 _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init_24)
struct kbase_ioctl_cs_tiler_heap_term { uint64_t gpu_heap_va; };
#define KBASE_IOCTL_CS_TILER_HEAP_TERM _IOW(KBASE_IOCTL_TYPE, 49, struct kbase_ioctl_cs_tiler_heap_term)


/* KBASE_IOCTL_GET_CPU_GPU_TIMEINFO (CSF): CLOCK_MONOTONIC_RAW + GPU timestamp counter in one call */
union kbase_ioctl_get_cpu_gpu_timeinfo {
	struct { uint32_t request_flags; uint32_t paddings[7]; } in;
	struct { uint64_t sec; uint32_t nsec; uint32_t padding; uint64_t timestamp; uint64_t cycle_counter; } out;
};
#define KBASE_IOCTL_GET_CPU_GPU_TIMEINFO _IOWR(KBASE_IOCTL_TYPE, 50, union kbase_ioctl_get_cpu_gpu_timeinfo)
#define KBASE_TIMEINFO_MONOTONIC_FLAG (1u << 0)
#define KBASE_TIMEINFO_TIMESTAMP_FLAG (1u << 1)

#endif
