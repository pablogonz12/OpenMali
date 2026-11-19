# Explicit sync on the Wayland WSI path (lane explicit-sync-design)

## 1. What the driver has today (measured on fake v11/v13, t_ext_sync_caps)
- Device exts: KHR_external_semaphore(+_fd), KHR_external_fence(+_fd), KHR_timeline_semaphore (mali_physical_device.c:174-183); instance *_capabilities (mali_instance.c:25-26).
- vk_sync types (mali_sync.c:217, mali_queue.h): `mali_sync_type` = BINARY sync_file (import_sync_file/export_sync_file, kbase KCPU fence) + runtime `vk_sync_timeline` (EMULATED timeline, mali_sync_init_physical). vk_queue is DEFERRED.
- Result: binary SYNC_FD semaphore + fence are import+export (features 0x3). Timeline OPAQUE_FD export = 0.
- Present today = implicit: wsi exports a sync_file of the present semaphore and DMA_BUF_IOCTL_IMPORT_SYNC_FILE's it into the dma-buf (mali_wsi.c header; libkbase kb_dmabuf_import_sync_file).

## 2. What kbase / libkbase exposes
- KCPU queue (kbase.h:175-181): kb_kcpu_fence_signal -> new sync_file fd (signals after prior KCPU cmds); FENCE_WAIT, CQS_WAIT/SET(_OPERATION) commands in kbase_uapi.h:212-217; kb_fence_wait(sync fd).
- kb_dmabuf_export/import_sync_file (dma-buf implicit fences). Fences are plain dma-fence sync_files: no timeline, no syncobj, no DRM node (kbase is /dev/mali0).
- So kbase can produce/consume binary fences only. A timeline needs either CQS-word based points (GPU-visible u64, KCPU CQS_WAIT_OPERATION) or emulation over sync_files.

## 3. What Mesa wsi_common requires (pinned tree third_party/mesa/src/vulkan/wsi)
- wsi_device_supports_explicit_sync (wsi_common.c:3681) = !sw && has_timeline_semaphore && timeline export handle types has OPAQUE_FD. Today false (OPAQUE_FD timeline not exportable) => Wayland WSI never uses wp_linux_drm_syncobj (wsi_wl_use_explicit_sync, wsi_common_wayland.c:261).
- If it were true: wsi_create_image_explicit_sync_drm (wsi_common_drm.c:282) creates 2 timeline semaphores/image exportable as OPAQUE_FD, GetSemaphoreFdKHR, then `device->sync->fd_to_handle(...)` (vk_device::sync = a vk_drm_syncobj, requires a real DRM fd) and passes the fds to wp_linux_drm_syncobj_manager_v1.import_timeline. Acquire/release points are submitted as timeline waits/signals (wsi_common.c:2628), release waited via wsi_drm_wait_for_explicit_sync_release.
- => The OPAQUE_FD fd MUST be a real DRM syncobj fd (the compositor imports it with drmSyncobjFDToHandle). A kbase sync_file or a private fd is not accepted.
- SYNC_FD export/import only (already present) serves the implicit path; it does not enable the protocol.

## 4. Key obstacle
kbase has no DRM node; Android phones expose /dev/dri/* only to the display stack (likely none usable by the app domain; to verify on X8: ls -l /dev/dri, SELinux). Without any DRM fd we cannot create a syncobj, so protocol-level explicit sync is impossible. Options:
 A. Open any DRM node (card/renderD, even vgem/display) purely for syncobj ioctls (CREATE, TIMELINE_SIGNAL/WAIT, FD_TO_HANDLE/HANDLE_TO_FD, EVENTFD). Driver-side: new vk_sync type `mali_syncobj_timeline` wrapping vk_drm_syncobj_type on that fd, bridged to kbase fences via DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE/HANDLE_TO_FD with SYNC_FILE flags (import a kbase sync_file as a point; export a point as sync_file to feed kb KCPU FENCE_WAIT). Set vk.drm_fd-equivalent `device->sync` for wsi. Needs the compositor to be on the same DRM device? No: syncobj fds are device-agnostic enough for import into the compositor's own DRM fd only if kernel syncobj; works across DRM nodes (syncobjs are per-file but fd->handle import works on any DRM device).
 B. Userspace-only: do not advertise; keep implicit sync (current). Compositors fall back to implicit via dma-buf (works for compositors that honour dma-buf fences).
 C. DroidDeck's compositor path is gamescope/own compositor with no syncobj protocol: explicit sync has no consumer there.
Recommendation: do A only if DroidDeck's compositor (or Xwayland/KWin on the phone) binds wp_linux_drm_syncobj_manager_v1 and a DRM node is reachable; otherwise stay B.

## 5. Host-test options (fake backend)
- Capability gate: t_ext_sync_caps (done, slice 0).
- Fake DRM syncobj: on x86 host, a real /dev/dri/renderD* syncobj works (needs access; skip if absent) or vgem; a test can run the full vk_sync syncobj type against the fake backend since fake fences are sync-file-less: fake backend would have to export timeline-signalled syncobj. Pure-userspace alternative: drive the `mali_syncobj_timeline` type with a mocked ioctl table (injectable `mali_drm_ops`), no kernel needed.
- Protocol level: weston/wlroots headless nested compositor exposing linux-drm-syncobj-v1 + t_vk_present --mode fifo (needs host DRM node; manual, not in check.sh).

## 6. Slice plan
| # | Slice | Files | Cost | Risk | Host-testable |
|---|-------|-------|------|------|---------------|
| 0 | t_ext_sync_caps: pin SYNC_FD binary import/export and the "timeline OPAQUE_FD off" gate (DONE, this PR) | vk/tests/t_ext_sync_caps.c, build.sh, check.sh | XS | none | yes |
| 1 | Probe: ls /dev/dri + SELinux on X8; try DRM_IOCTL_SYNCOBJ_CREATE from the app domain. Go/no-go. | none (farm hold, tools) | XS | needs phone | no |
| 2 | mali_drm_syncobj.c: tiny wrapper over syncobj ioctls with injectable ops, sync_file<->syncobj point helpers | new non-hot file + meson/build.sh | S | low | yes (mock ops) |
| 3 | vk_sync type mali_syncobj_timeline (timeline, OPAQUE_FD export/import, CPU wait/signal, GPU wait via sync_file bridge) registered in mali_sync_init_physical; set device->sync | mali_sync.c, mali_queue.c (sync_types, submit waits ~516-562), mali_queue.h, mali_device.c | M | med: queue wait path is DEFERRED/CPU waits; vk_sync_timeline emulation conflict | partly (mock) |
| 4 | Flip EXPLICIT_SYNC_READY=1; capability only on kbase backend with a DRM node; MALI_EXPLICIT_SYNC=0 kill switch | mali_physical_device.c, mali_wsi.c, t_ext_sync_caps | S | med: switches Wayland WSI to the explicit path for all clients | partial |
| 5 | End-to-end: t_vk_present against a syncobj-capable compositor on X8 | tests, farm gate | M | high (compositor dependent) | no |
No slice touches hot files mali_cmd.c / mali_cs_emit.c / mali_pipeline.c.
