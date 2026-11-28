#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Every Mesa physical-device WSI entrypoint that reads the device-wide wsi_device pointer must be wrapped.

mali_wsi.c points pdev->vk.wsi_device at the software or the dma-buf instance for the length of one wrapped call,
under mali_wsi_lock. A Mesa entrypoint left in the dispatch table reads that pointer without the lock, so on a
second thread (a second VkDevice of the same process, e.g. Chromium's GPU process) it can answer from the other
instance. Usage: check_wsi_wrapped.py <mesa src/vulkan/wsi/wsi_common.c> <vk/src/mali_physical_device.c>
"""
import re
import sys

mesa, pdev = (open(p, encoding="utf-8").read() for p in sys.argv[1:3])
readers = []
for m in re.finditer(r"^(wsi_GetPhysicalDevice\w+)\(.*?^}", mesa, re.S | re.M):
    if re.search(r"\b(device|pdevice)->wsi_device\b", m.group(0)):
        readers.append(m.group(1)[len("wsi_"):])
wrapped = set(re.findall(r"dispatch_table\.(\w+)\s*=\s*mali_wsi_\w+;", pdev))
missing = [r for r in readers if r not in wrapped]
if not readers:
    raise SystemExit("no wsi_GetPhysicalDevice* readers found: is the Mesa path right?")
if missing:
    raise SystemExit("unwrapped wsi_device readers (race with mali_wsi_sw_enter): " + ", ".join(missing))
print("%d wsi_device readers, all wrapped: %s" % (len(readers), ", ".join(readers)))
