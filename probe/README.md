# kbase probe

Read-only probe of the Arm `mali_kbase` kernel interface (`/dev/mali0`) as seen from userspace.
Answers the questions behind an own Vulkan driver: handshake/UAPI version, GPU props, memory flags
and CPU-mapping cost, dma-buf import, fence mechanisms, CSF interface, syscall floor.
MIT. UAPI definitions: `kbase_uapi_min.h` (source header + version named per item).

Output: one JSON object per line (`{"test":...,"ok":...}`), diff-able between phones.
Every step is guarded (errno reported, continues); it never creates queue groups, submits or kicks.
All GPU regions are freed per step and `/dev/mali0` is closed at the end.

## Build (laptop)
    ./build.sh            # native x86-64 (-Wall -Wextra -Werror -pedantic) + aarch64
    ./build.sh aarch64
Result: `out/probe-aarch64` (dynamic, glibc, links `libc.so.6` only, needs <= GLIBC_2.34; the guest has 2.39)
via `droidtest/tools/cc-aarch64-glibc` (clang + Ubuntu arm64 sysroot). Dynamic was chosen to match the
guest libc and keep the binary at ~37 KB; static glibc would add ~700 KB for no gain.

## Run in the guest (do not run on the phone until agreed)
    adb push out/probe-aarch64 /storage/emulated/0/Download/probe-aarch64
    C="python3 -I ~/Projects/mali-gaming/tools/droiddeckctl"
    $C guest --timeout 120 -- sh -c 'cp /storage/emulated/0/Download/probe-aarch64 /tmp/probe && chmod +x /tmp/probe && /tmp/probe > /storage/emulated/0/Download/probe-out.jsonl 2>&1; cat /storage/emulated/0/Download/probe-out.jsonl' 
(`/sdcard` may be read-only in the guest; then print to stdout only, 256 KiB cap is enough.)
Needs a running session and Debugging tools ON. Native run on the laptop just reports `open` ENOENT.

## Steps
devnode survey, open+SELinux context, VERSION_CHECK (CSF nr52, then JM nr0) + SET_FLAGS, GET_GPUPROPS
(decoded + raw hex), MEM_ALLOC(_EX) over a flag table with mmap/timing/free, dma-heap + udmabuf
allocation and MEM_IMPORT (UMM) variants, KCPU FENCE_SIGNAL sync_file + sw_sync, CS_GET_GLB_IFACE,
DDK version/context id/timeinfo/priority, ioctl latency (mean/p99).

## Export path for kbase allocations
The UAPI (csf 1.12 headers as vendored by panvk-kbase-android, same set as r44-r49 for these calls) has no
ioctl exporting a native MEM_ALLOC region as a dma-buf; only MEM_IMPORT (UMM) goes dma-buf -> GPU.
Zero-copy presentation therefore has to allocate in dma-heap/udmabuf/gralloc and import into kbase.
Not verifiable from headers alone for r49: if a newer ioctl exists the probe will not see it.

## Not implemented
Queue groups/submission, tiler heaps, TLSTREAM, KINSTR_PRFCNT, KCPU MAP_IMPORT/FENCE_WAIT, USER_BUFFER import.

Note: dma-heaps whose name contains prot/svp/wfd/sapu/secure/tui are deliberately skipped.
