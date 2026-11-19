#!/usr/bin/env python3
# Small vkEnumeratePhysicalDevices prober (no vulkaninfo needed).
# Prints deviceName, driverID, driverVersion, API version and the ICD that answered.
import ctypes, ctypes.util, sys

VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001
VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES = 1000196000
DRIVER_IDS = {0: "UNKNOWN", 1: "AMD_PROPRIETARY", 2: "AMD_OPEN_SOURCE", 3: "MESA_RADV",
              4: "NVIDIA_PROPRIETARY", 5: "INTEL_PROPRIETARY_WINDOWS", 6: "INTEL_OPEN_SOURCE_MESA",
              7: "IMAGINATION_PROPRIETARY", 8: "QUALCOMM_PROPRIETARY", 9: "ARM_PROPRIETARY",
              10: "GOOGLE_SWIFTSHADER", 11: "GGP_PROPRIETARY", 12: "BROADCOM_PROPRIETARY",
              13: "MESA_LLVMPIPE", 14: "MOLTENVK", 15: "COREAVI_PROPRIETARY",
              16: "JUICE_PROPRIETARY", 17: "VK_DRIVER_ID_MESA_TURNIP", 18: "VK_DRIVER_ID_MESA_V3DV",
              19: "VK_DRIVER_ID_MESA_PANVK", 20: "VK_DRIVER_ID_SAMSUNG_PROPRIETARY",
              21: "VK_DRIVER_ID_MESA_VENUS", 22: "VK_DRIVER_ID_MESA_DOZEN",
              23: "VK_DRIVER_ID_MESA_NVK", 24: "VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA",
              25: "VK_DRIVER_ID_MESA_HONEYKRISP", 26: "VK_DRIVER_ID_VULKAN_SC_EMULATION_ON_VULKAN"}
DEVICE_TYPES = {0: "other", 1: "integrated-gpu", 2: "discrete-gpu", 3: "virtual-gpu", 4: "cpu"}


def ver(v):
    return "%d.%d.%d" % (v >> 22, (v >> 12) & 0x3FF, v & 0xFFF)


class VkPhysicalDeviceProperties(ctypes.Structure):
    _fields_ = [("apiVersion", ctypes.c_uint32), ("driverVersion", ctypes.c_uint32),
                ("vendorID", ctypes.c_uint32), ("deviceID", ctypes.c_uint32),
                ("deviceType", ctypes.c_int), ("deviceName", ctypes.c_char * 256),
                ("pipelineCacheUUID", ctypes.c_uint8 * 16), ("limits", ctypes.c_uint8 * 504),
                ("sparseProperties", ctypes.c_uint8 * 20)]


class VkPhysicalDeviceDriverProperties(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_int), ("pNext", ctypes.c_void_p),
                ("driverID", ctypes.c_int), ("driverName", ctypes.c_char * 256),
                ("driverInfo", ctypes.c_char * 256), ("conformanceVersion", ctypes.c_uint8 * 4)]


class VkPhysicalDeviceProperties2(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_int), ("pNext", ctypes.c_void_p),
                ("properties", ctypes.c_uint8 * 824)]


def main():
    lib = ctypes.CDLL("libvulkan.so.1")
    lib.vkCreateInstance.restype = ctypes.c_int
    lib.vkEnumeratePhysicalDevices.restype = ctypes.c_int
    lib.vkGetPhysicalDeviceProperties2.restype = None
    lib.vkGetPhysicalDeviceProperties.restype = None

    class VkApplicationInfo(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_int), ("pNext", ctypes.c_void_p),
                    ("pApplicationName", ctypes.c_char_p), ("applicationVersion", ctypes.c_uint32),
                    ("pEngineName", ctypes.c_char_p), ("engineVersion", ctypes.c_uint32),
                    ("apiVersion", ctypes.c_uint32)]

    class VkInstanceCreateInfo(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_int), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32),
                    ("pApplicationInfo", ctypes.POINTER(VkApplicationInfo)),
                    ("enabledLayerCount", ctypes.c_uint32), ("ppEnabledLayerNames", ctypes.c_void_p),
                    ("enabledExtensionCount", ctypes.c_uint32), ("ppEnabledExtensionNames", ctypes.c_void_p)]

    ai = VkApplicationInfo(0, None, b"mali-open-probe", 1, b"none", 1, 0x00403000)
    ci = VkInstanceCreateInfo(0, None, 0, ctypes.pointer(ai), 0, None, 0, None)
    inst = ctypes.c_void_p()
    rc = lib.vkCreateInstance(ctypes.byref(ci), None, ctypes.byref(inst))
    if rc != 0:
        print("vkCreateInstance failed: %d" % rc)
        return 1
    n = ctypes.c_uint32(0)
    rc = lib.vkEnumeratePhysicalDevices(inst, ctypes.byref(n), None)
    if rc != 0 or n.value == 0:
        print("vkEnumeratePhysicalDevices: rc=%d count=%d" % (rc, n.value))
        return 1
    arr = (ctypes.c_void_p * n.value)()
    rc = lib.vkEnumeratePhysicalDevices(inst, ctypes.byref(n), arr)
    if rc != 0:
        print("vkEnumeratePhysicalDevices(2): rc=%d" % rc)
        return 1
    print("physical devices: %d" % n.value)
    for i in range(n.value):
        p = VkPhysicalDeviceProperties()
        lib.vkGetPhysicalDeviceProperties(arr[i], ctypes.byref(p))
        dp = VkPhysicalDeviceDriverProperties()
        dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
        p2 = VkPhysicalDeviceProperties2()
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        p2.pNext = ctypes.addressof(dp)
        lib.vkGetPhysicalDeviceProperties2(arr[i], ctypes.byref(p2))
        name = p.deviceName.decode("utf-8", "replace")
        dname = dp.driverName.decode("utf-8", "replace").rstrip("\x00")
        dinfo = dp.driverInfo.decode("utf-8", "replace").rstrip("\x00")
        print("[%d] deviceName=%s" % (i, name))
        print("    driverID=%d (%s)  driverName=%s" % (dp.driverID, DRIVER_IDS.get(dp.driverID, "?"), dname))
        print("    driverInfo=%s" % dinfo)
        print("    driverVersion=%s (0x%08x)  apiVersion=%s  deviceType=%s  vendor=0x%04x device=0x%04x" % (
            ver(p.driverVersion), p.driverVersion, ver(p.apiVersion),
            DEVICE_TYPES.get(p.deviceType, "?"), p.vendorID, p.deviceID))
    return 0


sys.exit(main())
