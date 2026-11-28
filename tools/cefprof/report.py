#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""report.py <run-dir> [<run-dir>...]: per-frame tables from cefprof JSON; several dirs give a side-by-side table."""
import glob, json, os, re, sys


def load(d):
    files = [f for f in glob.glob(os.path.join(d, "prof.*")) if re.search(r"prof\.\d+$", f)]
    r = max((json.load(open(f)) for f in files), key=lambda j: j["frames"])
    r["k"] = {k: (w, a, first, last) for k, w, a, first, last in r["kv"]}
    r["dir"] = d
    return r


def summary(r):
    n = r["frames"]
    c = lambda *names: sum(r["cmds"].get(x, [0, 0])[0] for x in names) / n
    ms = lambda *names: sum(r["cmds"].get(x, [0, 0])[1] for x in names) / n / 1e6
    k = lambda key: r["k"].get(key, (0,))[0] / n
    ks = r["kstats_window"]
    kn = lambda i: ks[i] / n if ks else float("nan")
    cmdnames = [x for x in r["cmds"] if x.startswith("Cmd") or x in ("BeginCommandBuffer", "EndCommandBuffer")]
    rows = [
        ("host fps with this layer (not phone fps)", n / r["seconds"]),
        ("vkQueueSubmit*", c("QueueSubmit", "QueueSubmit2", "QueueSubmit2KHR")),
        ("command buffers submitted", k("sub.cmdbufs")),
        ("render passes", k("pass.total")),
        ("  with colour LOAD", k("pass.load_color")),
        ("  with depth/stencil LOAD", k("pass.load_zs")),
        ("  with any LOAD", k("pass.load_any")),
        ("  with 0 draws", k("pass.zero_draw")),
        ("  with exactly 1 draw", k("pass.one_draw")),
        ("draws", k("draw.total")),
        ("  of which attributed to a render pass", k("pass.draws_attributed")),
        ("compute dispatches", c("CmdDispatch", "CmdDispatchIndirect")),
        ("pipeline barriers (calls)", c("CmdPipelineBarrier", "CmdPipelineBarrier2", "CmdPipelineBarrier2KHR")),
        ("  image barriers", k("barrier.image")),
        ("buffer->image copies (regions)", k("copy.buf2img.count")),
        ("  KiB uploaded", k("copy.buf2img.bytes") / 1024),
        ("image->buffer (readback) regions", k("copy.img2buf.count")),
        ("image->image copy regions", k("copy.img2img.count")),
        ("  MiB image->image", k("copy.img2img.bytes") / 1048576),
        ("blit / resolve regions", k("copy.blit.count") + k("copy.resolve.count")),
        ("buffer->buffer regions", k("copy.buf2buf.count")),
        ("clear colour/zs image", c("CmdClearColorImage", "CmdClearDepthStencilImage")),
        ("vkCmdClearAttachments", c("CmdClearAttachments")),
        ("descriptor sets bound", k("bind.descriptor_sets")),
        ("descriptor sets allocated", k("desc.sets_allocated")),
        ("descriptor writes", k("desc.writes")),
        ("pipeline binds", c("CmdBindPipeline")),
        ("pipelines created (in window, total)", sum(w for key, (w, *_) in r["k"].items() if key.startswith("pipe.created_"))),
        ("push constant calls", c("CmdPushConstants")),
        ("vertex+index buffer binds", c("CmdBindVertexBuffers", "CmdBindVertexBuffers2", "CmdBindVertexBuffers2EXT", "CmdBindIndexBuffer", "CmdBindIndexBuffer2KHR")),
        ("image creates", c("CreateImage")), ("image destroys", c("DestroyImage")),
        ("image view creates", c("CreateImageView")), ("image view destroys", c("DestroyImageView")),
        ("buffer creates", c("CreateBuffer")), ("buffer destroys", c("DestroyBuffer")),
        ("framebuffer creates", c("CreateFramebuffer")), ("render pass creates", c("CreateRenderPass", "CreateRenderPass2", "CreateRenderPass2KHR")),
        ("vkAllocateMemory", c("AllocateMemory")), ("vkFreeMemory", c("FreeMemory")),
        ("vkMapMemory", c("MapMemory")), ("vkUnmapMemory", c("UnmapMemory")), ("flush+invalidate ranges", k("mem.flush_ranges") + k("mem.invalidate_ranges")),
        ("vkWaitForFences", c("WaitForFences")), ("vkGetFenceStatus", c("GetFenceStatus")), ("vkQueueWaitIdle", c("QueueWaitIdle")),
        ("vkDeviceWaitIdle", c("DeviceWaitIdle")), ("semaphore waits (timeline)", c("WaitSemaphores", "WaitSemaphoresKHR")),
        ("query begins + timestamps", c("CmdBeginQuery", "CmdWriteTimestamp", "CmdWriteTimestamp2")), ("vkGetQueryPoolResults", c("GetQueryPoolResults")),
        ("semaphore fd export/import", c("GetSemaphoreFdKHR", "ImportSemaphoreFdKHR")),
        ("kernel: MEM_SYNC ioctls CPU->GPU", kn(515)), ("kernel: MEM_SYNC MiB CPU->GPU", kn(512) / 1048576),
        ("kernel: MEM_SYNC ioctls GPU->CPU", kn(516)), ("kernel: MEM_SYNC MiB GPU->CPU", kn(513) / 1048576),
        ("kernel: CS_QUEUE_KICK", kn(37)), ("kernel: KCPU enqueue", kn(47)),
        ("kernel: MEM_ALLOC(_EX)", kn(59) + kn(5)), ("kernel: MEM_FREE ioctl", kn(7)),
        ("kernel: all kbase ioctls", sum(ks[:256]) / n if ks else float("nan")),
        ("CPU ms: vkCmd* + begin/end (record)", ms(*cmdnames)),
        ("CPU ms: vkQueueSubmit* (build CS + kernel)", ms("QueueSubmit", "QueueSubmit2", "QueueSubmit2KHR")),
        ("CPU ms: vkQueuePresentKHR (includes X11 shm copy)", ms("QueuePresentKHR")),
        ("CPU ms: pipeline creation", ms("CreateGraphicsPipelines", "CreateComputePipelines")),
    ]
    return rows


def main():
    runs = [load(d) for d in sys.argv[1:]]
    sums = [summary(r) for r in runs]
    print("per-frame averages; frame = vkQueuePresentKHR")
    names = [os.path.basename(r["dir"].rstrip("/")) for r in runs]
    print("%-52s" % "metric" + "".join("%22s" % x[:21] for x in names))
    print("%-52s" % "frames in window" + "".join("%22d" % r["frames"] for r in runs))
    for i, (name, _) in enumerate(sums[0]):
        print("%-52s" % name + "".join("%22.2f" % s[i][1] for s in sums))
    if len(runs) > 1:
        return
    r, n = runs[0], runs[0]["frames"]
    print("\nlive kernel allocations at end of window (whole process): %d" % (r["kstats_total"][517] - r["kstats_total"][518]) if r["kstats_total"] else "")
    print("instances %d, VkDevices created %d" % (r["instances"], r["devices_created"]))
    for f in sorted(glob.glob(r["dir"] + "/prof.*.device*.txt")):
        print("\n== " + os.path.basename(f))
        print(open(f).read().strip())

    def table(title, pred, limit=40, per_frame=True):
        items = sorted(((k, v) for k, v in r["k"].items() if pred(k)), key=lambda kv: (-kv[1][0], -kv[1][1], kv[0]))
        if not items:
            return
        print("\n== %s  (per-frame in window | window total | whole run) [%d distinct]" % (title, len(items)))
        for k, (w, a, first, last) in items[:limit]:
            print("%9.3f %9d %9d  %s" % (w / n, w, a, k))

    pre = lambda *p: (lambda k: k.startswith(p))
    table("submissions and present", pre("sub.", "present", "swapchain", "queue.", "device."))
    table("render pass kinds", pre("pass."), 40)
    table("draw kinds", pre("draw."))
    table("copies by op/format/size", lambda k: k.startswith(("copy.", "clear.", "update_buffer")), 80)
    table("copy regions, most frequent (exact tuples)", pre("region "), 60)
    table("R8 uploads (coverage masks / glyph atlas): all tuples by whole-run count", lambda k: k.startswith("region op=buf2img fmt=R8_UNORM"), 80)
    table("image->image sources", pre("img2img."), 20)
    table("barriers", pre("barrier."), 20)
    table("descriptors", pre("desc.", "bind.", "push."), 20)
    table("objects and memory", pre("image.create", "buffer.create", "mem.", "query."), 60)
    table("waits", pre("wait."))
    print("\n== distinct pipelines first bound / created, cumulative by frame")
    for kind, prefix in (("created", "pipe.create_frame="), ("first bound", "pipe.first_bound frame=")):
        evs = sorted((int(k.split("=")[1]), v[1]) for k, v in r["k"].items() if k.startswith(prefix))
        tot = 0
        marks = [1, 5, 30, 60, 120, 180, 300, 480, 10 ** 9]
        line = []
        for m in marks:
            tot = sum(c for f, c in evs if f <= m)
            line.append("<=%s:%d" % ("end" if m == 10 ** 9 else m, tot))
        print(" %-12s %s" % (kind, " ".join(line)))
    print("\n== R8 upload widths seen (whole run): width%4 histogram and distinct extents")
    ext = {}
    for k, v in r["k"].items():
        m = re.match(r"region op=buf2img fmt=R8_UNORM .* ext=(\d+)x(\d+) off=(-?\d+),(-?\d+)", k)
        if m:
            e = tuple(map(int, m.groups()))
            ext[e] = ext.get(e, 0) + v[1]
    if ext:
        tot = sum(ext.values())
        for label, idx in (("width%4", 0), ("offset.x%4", 2)):
            h = [sum(v for e, v in ext.items() if e[idx] % 4 == m) for m in range(4)]
            print(" %s: %s  (of %d regions)" % (label, h, tot))
        print(" distinct WxH: " + " ".join(sorted({"%dx%d" % e[:2] for e in ext}, key=lambda s: tuple(map(int, s.split("x"))))))
    print("\n== all device entry points called (per frame in window, mean CPU us per call)")
    for name, (w, ns, a, ans) in sorted(r["cmds"].items(), key=lambda kv: -kv[1][1]):
        print("%10.2f/frame %10.2f us/call %10.3f ms/frame %9d total  vk%s" % (w / n, ns / w / 1e3 if w else ans / a / 1e3, ns / n / 1e6, a, name))
    if r["kstats_window"]:
        print("\n== kbase ioctls by nr, per frame (15=MEM_SYNC 37=CS_QUEUE_KICK 47=KCPU_ENQUEUE 59=MEM_ALLOC_EX 7=MEM_FREE)")
        print(" " + " ".join("nr%d:%.3f" % (i, v / n) for i, v in enumerate(r["kstats_window"][:256]) if v))


main()
