# Present path: what a frame costs and what it waits on (host audit, 2026-11-19)

Scope: one `vkQueuePresentKHR` and one `vkAcquireNextImageKHR` through our ICD on the dma-buf (Wayland/X11) path,
which is the path the Steam menu uses under zink. Host-only: everything below is read from the pinned Mesa tree
(`third_party/mesa`, commit 0a6fa60) and our `vk/src`, plus fake-kernel numbers. Claims the fake backend cannot
prove are marked **on-device**.

Line references are for Mesa 26.3 as pinned and for this commit's `vk/src`.

## 1. Swapchain image count and present modes: we do not choose, and 3 is the floor

`wsi_common` owns both. We never override the present mode (`mali_wsi.c` only sets `support_modifiers`).

| platform | `minImageCount` | source |
|---|---|---|
| Wayland, no `wp_fifo_v1` | 4 | `wsi_common_wayland.c` `WSI_WL_BUMPED_NUM_IMAGES` |
| Wayland, with `wp_fifo_v1` | 3 (`MAILBOX` 4) | `wsi_wl_surface_get_min_image_count()` |
| X11 / Xwayland | 3 (`MAILBOX`, or when the present path needs a fence wait, 4) | `wsi_common_x11.c` `x11_get_min_image_count[_for_present_mode]()` |

Creation only ever bumps **up** (`MAX2`, `wsi_common_wayland.c` ~3910-3932, `wsi_common_x11.c` ~3147-3157), and
`vkCreateSwapchainKHR` with `minImageCount` below the surface capability is invalid usage, so a compliant app cannot
get a 2-image chain. `maxImageCount` is 0 (no limit, `wsi_common_wayland.c` ~1928, `wsi_common_x11.c` ~818).

Consequence for the 120 Hz question: **a 2-image FIFO swapchain is unreachable, so "every late frame falls to 60"
cannot come from that.** The depth that matters is `image_count - 1` pipelined frames, and `wsi_common` throttles
present on the *per-image* fence (`wsi_common.c` ~2440-2470): a present waits for the previous use of **that image**,
not for the one it just queued. With a 3-image chain that is usually the present two frames back, but the offset
follows the app's acquire order (any free image may be reacquired) and is not a fixed number. No change is proposed here. What we default to is Mesa's default, i.e.
`FIFO` unless the app asks otherwise; `MESA_VK_WSI_PRESENT_MODE` (Mesa's own option) overrides, not a `MALI_*` switch.

**On-device** (to confirm which row applies to the Steam menu): log three numbers and `presentMode` once (zink passes
them straight through): the app's requested `VkSwapchainCreateInfoKHR::minImageCount`, the surface capability
`VkSurfaceCapabilitiesKHR::minImageCount`, and the count `vkGetSwapchainImagesKHR` actually returns after creation.
The request alone says nothing about the chain: an app may ask for 4 on a surface whose minimum is 3. Also check
whether gamescope advertises `wp_fifo_v1` (`wayland-info | grep fifo` inside the session). Expected: request 3,
capability 3, returned 3, FIFO.

## 2. `OUT_OF_DATE` and the real hazard: an unbounded wait, not a spin

We never return `VK_ERROR_OUT_OF_DATE_KHR` ourselves: there is no occurrence in `vk/src`. Acquire/present resizing is
entirely `wsi_common`'s (`wsi_common_wayland.c`, `wsi_common_x11.c` ~2560 turns a failed present-time fence wait into
`OUT_OF_DATE`). So an app that ignores `OUT_OF_DATE` gets no help from us, and our `vkCreateSwapchainKHR` does no
extent clamping of its own.

No spin exists in the present path after #255: a timeout-0 poll answers from the sync_file before touching
`wait_lock` (`mali_backend_kbase.c`, `kbase_fence_wait`), and a blocking wait is sliced at 20 ms.

**Hypothesis for the Steam UI hangs (stated as a hypothesis, not established):** `wsi_common_queue_present` waits on
the per-image throttle fence with `timeout = ~0ull` before *every* present (`wsi_common.c` ~2469; the X11 present
thread does the same at `wsi_common_x11.c` ~2557, the software path at `wsi_common.c` ~2890). There is no bound and no
`OUT_OF_DATE` path out of it.

That fence is the driver's own: it signals after the present's GPU work and the client wait semaphores, not when the
compositor releases the image. So the compositor cannot block it directly. The dependency that could carry compositor
retention into `vkQueuePresentKHR` is the other side of the same loop: the image stays unusable until the compositor
releases it, and the release arrives as the acquire-side wait for that image (the Wayland buffer release, or an
implicit dma-buf sync on the exported buffer on the X11/shm path). If a compositor stops releasing, the app's next
acquire/render for that image waits there, and if it holds an already-acquired image the present of that image may
never complete. That chain - compositor never releases image -> client blocks on the acquire-side wait for that image
-> the UI stops responding - is a hypothesis: it is consistent with the code, but it is not reproduced, and nothing
here distinguishes it from an app-side wait. It is stated as a hypothesis, not as established behaviour.

**On-device measurement that would confirm or kill it:** with the menu hung, run `gdb -p` (or `eu-stack`) on the
client, or take two `perf record -p <pid> -g -- sleep 5` samples, and check for a stack of
`vkQueuePresentKHR -> wsi_common_queue_present -> vk_common_WaitForFences -> vk_sync_wait -> kbase_fence_wait`.
Capture the same stack whenever the hang is seen; if present never appears, the hypothesis is dead. Second half of
the check: `dumpsys SurfaceFlinger --latency` (or `tools/steam-present`) during the hang shows whether the compositor
is still consuming the surface.

## 3. The dma-buf present costs no layout transition and no tile load/store

- Our driver does not implement layout transitions at all: `oldLayout`/`newLayout` appear nowhere in `vk/src`
  (barriers are the runtime's no-ops), and `PRESENT_SRC_KHR` is not referenced in `vk/src`.
- The dma-buf present path issues no barrier either: `wsi_common_queue_present` calls `swapchain->queue_present`
  directly; the `PRESENT_SRC_KHR` barriers in `wsi_common.c` (~3200-3400) belong to the blit paths only
  (`wsi_cmd_blit_image_to_buffer`, `wsi_cmd_blit_image_to_image`), i.e. the software/DXGI image types.

So **a present on our path is cheap**: no full-frame load/store, no resolve, no copy. The only GPU work per frame is
the render pass that produced the image, whose tile store to the linear (LINEAR-modifier) exportable buffer is part
of that pass. Linear render targets are slower per pixel than tiled/AFBC ones (see `GPU-GAP.md` finding 3), but that
is a render cost, not a present cost.

The exception is the **software path** (`MALI_WSI=sw`, or the fake backend): `wsi_cmd_blit_image_to_buffer` does a
full `vkCmdCopyImageToBuffer` of the frame per present — 1558x720x4 = 4.49 MB at 120 Hz = 538 MB/s plus a
GPU->CPU readback. If the Steam menu ever lands there (X11 without DRI3, i.e. DroidDeck's Xwayland), that alone caps
the frame rate. **On-device:** `MALI_WSI=sw` vs default on the same menu scene, compare
`tools/steam-present` pacing; and confirm the path in use by grepping the driver's own log for the swapchain image
type (or `MESA_VK_WSI_PRESENT_MODE=immediate` not changing the frame cost).

## 4. Every fence poll used to CSYNC every mapped BO (fixed for wsi waits; #248 covers the rest)

`kbase_fence_wait` swept every mapped BO with one `MEM_SYNC` ioctl on each successful wait, in both the timeout-0
poll branch and the blocking branch. Every present has one such wait (`wsi_common.c` ~2469), and zink also polls
fences per frame, so the sweep ran at least once per frame for a consumer that never reads driver memory there.

Host fake-kernel numbers (`t_memsync`, 300 present-shaped frames = frame CS + signal + throttle wait, 3 mapped BOs):

| | MEM_SYNC total | per present | us/present (fake) |
|---|---|---|---|
| throttle wait sweeping (before) | 900 | 3.00 | 0.69 |
| throttle wait skipped (fix) | 0 | 0.00 | 0.64 |

The fake kernel's `MEM_SYNC` only increments a counter, so the 0.05 us/present difference is **not** the device cost;
it is the floor of the bookkeeping. **On-device measurement** (this decides whether the CSYNC skip matters): on the
CMF, run the Steam menu with `MALI_PROF=1` and read the `PROF/memsync nbos=... non-lazy=...` line; it gives N, the
number of ioctls one sweep would issue. Then trace only `MEM_SYNC`, not all ioctls: `strace -f -e trace=ioctl -o
/tmp/io.log -p <pid>` for a fixed window (10 s), keep the calls whose request is `KBASE_IOCTL_MEM_SYNC`
(`_IOW(KBASE_IOCTL_TYPE, 15, ...)`; grep the request number if strace prints it raw), and divide by the number of
presents in the *same* window. Take that count from an independent present counter, not from `MALI_PROF`:
`PROF/frame` is printed only every 200th call and reports averages (`mali_cs_emit.c`, `if (++c % 200 == 0)`), and
`MALI_FRAME_PROF` prints one `FRAMEPROF` line per retired submission, so neither equals the present count. Use
SurfaceFlinger's frame counter for the app's layer (`dumpsys SurfaceFlinger --latency <layer>` over the same
window) or a counter added on the `vkQueuePresentKHR` path.
`perf stat -e syscalls:sys_enter_ioctl` and `strace -c -e ioctl` count every ioctl; they cannot yield MEM_SYNC per
present and must not be used for this ratio. The request filter counts every MEM_SYNC, not only the WSI wait's: submit-time CPU->device syncs and other waits'
device->CPU syncs stay in the trace. So attribute the difference rather than the absolute count: run the same
fixed workload twice - once on main and once with #262's guards (matched runs, same scene, same window) - and use
the delta in MEM_SYNCs per present as the cost the skip removes; any residual both runs share is not #262's.
If that delta x ioctl cost is under ~1 % of 8.3 ms, this is a micro-optimisation and should not gate the present
work; if it is over ~5 %, it is a frame-time item. Before the skip, the per-present count for the WSI wait alone is
bounded by N = non-lazy BOs from the `PROF/memsync` line; after it, 0.

### Coordination: #248, #255 and the CSYNC skip (#262)

The history this audit was written against, as it stands on main now: #255's timeout-0 poll fast path in
`kbase_fence_wait` (a poll must not queue behind another thread's 20 ms `wait_lock` slice) is on main, and #248
(per-BO write tracking for the fence-wait CSYNC) landed on top of it as `f2cabad` **without** deleting the poll
branch. The CSYNC skip described below is not part of either: it is PR #262, branch `lane/present-audit`: the rebased commit is
`f4ea8ab` (on main `7b7a3d1`, so with #248; its pre-rebase original is `7f2e860`, which predates #248), and it
applies its `if (!mali_fence_wait_no_csync)` guards to both
MEM_SYNC sites in `kbase_fence_wait` - the poll return (line 535) and the blocking-wait return (line 553).

So there is nothing left to rebase here: #248 is already merged, #262 is on top of it, and #228's ring-sized pieces
are also on main. Adding another fence-wait change on top of #262 must keep all three.

## Current state: the wsi CSYNC skip lives in #262, not in this PR

This PR only adds this document; it changes no code. The behaviour under audit is PR #262 (branch
`lane/present-audit`; the rebased commit is `f4ea8ab`, on top of main with #248, and the pre-rebase original is
`7f2e860`; not on main at the time of writing): in that branch `mali_wsi.c` wraps
`pdev->wsi.WaitForFences` (the same way it already wraps `DestroyQueryPool`) and sets the thread-local
`mali_fence_wait_no_csync` for the duration, and `kbase_fence_wait` skips the device->CPU sweep when it is set. If
that branch is not in your checkout, none of this code is here - treat it as a prerequisite of the numbers in
sections 3-4, not as something this change adds. What holds once #262 is applied: an app's own
`vkWaitForFences`/`vkGetFenceStatus` runs with the flag clear and still invalidates, and the sweep is stateless, so
skipping one cannot lose an invalidate a later wait needs. The flag is not used by the fake backend (it has no
MEM_SYNC), and the test drives the kbase backend over the fake kernel.

Mutation check (recorded here so it is repeatable on #262, not on this PR): delete both
`if (!mali_fence_wait_no_csync)` guards there and `tests/build-t_memsync.sh` fails 3 cases ("wsi throttle wait/poll:
no MEM_SYNC sweep", "300 presents: no MEM_SYNC at all" reports 900). With the guards, `vk/check.sh` is OK.
