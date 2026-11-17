# Presentation path: libkbase dma-buf -> phone screen (MaliDeck 0.7.1-bridge0.5.5.3-p9-gs1)

Paths under upstream/DroidDeck/app/src/main/cpp/waylandcomp/src/ unless noted. Raw runs: runs/20261115-cmf-present/.

## Chain
guest client -> gamescope (nested, xdg_backend, Wayland) -> DroidDeck Android compositor -> SurfaceView/ASurfaceControl -> SurfaceFlinger.

## Facts (source)
- Client buffers: zwp_linux_dmabuf_v1 v4; gamescope offered only LINEAR for XB24/AB24, so t_present uses AB24 linear, stride padded to 64 B (driver/present/t_present.c).
- Sync: no wp_linux_drm_syncobj anywhere. Session sets GAMESCOPE_DISABLE_EXPLICIT_SYNC=1 (tools/gamescope patches/0926-explicit-sync-switch.patch; droiddeck-session ~l.520-577). Sync is implicit via dma-buf fences (gamescope patches 0123/0124 read the dma-buf fence). t_present attaches the GPU fence with DMA_BUF_IOCTL_IMPORT_SYNC_FILE (WRITE).
- Gamescope passes client dma-bufs through as host wl_buffers when composite=0 (BL_VKBRIDGE_PASSTHROUGH=1).
- Android compositor copy path: compositor.c:906-913 logs "dma-buf: copied into the screen swapchain"; import vk_present.c:1163 (vkp_image_import_dmabuf), vkCmdBlitImage vk_present.c:2390, FIFO swapchain created vk_present.c:862-882, vkQueuePresentKHR :2228.
- Zero-copy path exists: banner_ahb_v1 (protocols/banner-ahb-v1.xml), ahb_swapchain.c:190 (present exports sync_file), sc_layer.c ~:746 ASurfaceTransaction_setBuffer with release fence. Gate: g_zero_copy (compositor.c:307), default off; enabled only by nativeSetZeroCopy(true) (SessionActivity.kt:2338, HDR), Quest, or vkp_quest_layer_presentation (compositor.c:3897). Needs a pref change: not done.
- Installed app speaks banner_ahb_v1 v2 only. MaliBridge wsi_wayland.c:156-170 speaks v3 (offer/withdraw) but the compositor source has no v3, so under nested gamescope game dma-bufs stay plain dma-bufs.
- AHBs come from the bridge server (server_dmabuf_ahb.c:116 AHardwareBuffer_allocate). A guest dma-heap/libkbase buffer is not a gralloc AHB: it needs the copy unless the app wraps it (unverified).

## Measured (phone CMF, 1600x720 @120 Hz, Steam session)
- t_present synced 20 s: commit 119.4 fps, presented 119.3 fps; intervals 8 ms x2372, 16 ms x14, none >=25 ms; commit->release mean 11.8 ms p99 17 ms; 0 errors, 0 GPU faults (run-sync.out).
- --no-sync 15 s: same pacing, mean 8.42 ms, p99 16.7 ms, 0 errors (run-nosync.out).
- App log during runs: "copy N, zero-copy 0, layer copy 0" on every interval (app-nosync.log:384,840,906) = copy path.

## Not shown
- The t_present window was never on screen: Steam UI (appId 769) holds gamescope focus; our native Wayland toplevel has appId 0 and is not in focusableApps (droiddeckctl focus). Screenshots show Steam, not our colours. gamescope composite_why lines (composite=0 layers=1) are for the Steam UI, not our surface. `droiddeckctl run` mode failed to start (GUEST_EXIT 1 / no session) on this build.
- So pass-through of our buffers and visible tearing/garbage with --no-sync are UNVERIFIED. Client-side selfcheck (readback) was clean.

## Update 2026-11-16: visible now
- Wrapping the client as `reaper SteamLaunch AppId=769 -- t_present_tri` (libkbase/tools/fake-reaper.c) makes gamescope accept the toplevel;
  the Steam UI is replaced on screen by our frames (libkbase/tests/t_present_tri.c, runs/20261116-r2-present/). Copy path confirmed:
  gamescope composite (GPU) + 1 compositor blit, 0 CPU copies, ~117 fps. Sync-less/--no-sync tearing is still unverified.

## What would remove the copy
1. App: implement banner_ahb_v1 v3 offer/withdraw in the compositor and default g_zero_copy on (or a switch).
2. Buffers must be AHB-backed (bridge server allocation, or compositor-side gralloc alloc exported as dma-buf and imported by libkbase).
3. Make our window focusable: run mode working, or a Steam appId tag.
