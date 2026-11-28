#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Small, synthetic regression tests independent of local Vulkan installations."""
import contextlib
import importlib.util
import io
import json
import os
import tempfile
from types import SimpleNamespace

path = os.path.join(os.path.dirname(__file__), "../../tools/feature-matrix/feature_matrix.py")
spec = importlib.util.spec_from_file_location("feature_matrix", path)
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)

with tempfile.TemporaryDirectory() as tmp:
    matrix.DATA = tmp
    ident = {"deviceName": "fake", "vendorID": 5045, "deviceID": 10,
             "driverName": "mali-own", "apiVersion": "1.3"}
    base = {"extensions": ["VK_EXT_present"], "features": {"present": True, "unsupported": False},
            "formats": {"VK_FORMAT_TEST": ["SAMPLED_IMAGE"]},
            "id": ident}
    probe = {"capabilities": {"device": {
        "extensions": {"VK_EXT_present": 1},
        "features": {"features": {"present": True, "unsupported": False}},
        "formats": {"VK_FORMAT_TEST": {"properties": {"optimalTilingFeatures":
                    ["VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"]}}},
        "properties": {"properties": dict(ident)}}}}
    baseline = os.path.join(tmp, "ours-v10.json")
    capture = os.path.join(tmp, "probe.json")
    args = SimpleNamespace(gpu="v10", vulkaninfo=capture, ignore=[])

    def check(expected, message):
        with open(baseline, "w") as f:
            json.dump(base, f)
        with open(capture, "w") as f:
            json.dump(probe, f)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = matrix.cmd_check(args)
        assert result == expected and message in out.getvalue(), out.getvalue()

    check(0, "still advertised")
    props = probe["capabilities"]["device"]["properties"]["properties"]
    props["driverName"] = "different-icd"
    check(1, "wrong probe driverName")
    props["driverName"] = ident["driverName"]
    props["deviceID"] = 11
    check(1, "wrong probe deviceID")
    props["deviceID"] = ident["deviceID"]
    del props["driverName"]
    check(1, "incomplete probe")
    props["driverName"] = ident["driverName"]
    props["driverVersion"] = 42
    check(0, "still advertised")  # a driver version change is not an identity change
    probe["capabilities"]["device"]["features"]["features"]["present"] = False
    check(1, "feature present")
    del probe["capabilities"]["device"]["features"]["features"]["present"]
    check(0, "not reported by this vulkaninfo")  # an older vulkaninfo that lacks the struct is not a regression
    probe["capabilities"]["device"]["features"]["features"]["present"] = True
    probe["capabilities"]["device"]["extensions"]["VK_EXT_new"] = 1
    del probe["capabilities"]["device"]["extensions"]["VK_EXT_present"]
    check(1, "extension VK_EXT_present")
    probe["capabilities"]["device"]["extensions"]["VK_EXT_present"] = 1
    probe["capabilities"]["device"]["formats"]["VK_FORMAT_TEST"]["properties"]["optimalTilingFeatures"] = []
    check(1, "format VK_FORMAT_TEST SAMPLED_IMAGE")
    probe["capabilities"]["device"]["formats"]["VK_FORMAT_TEST"]["properties"]["optimalTilingFeatures"] = [
        "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"]
    probe["capabilities"]["device"]["extensions"]["VK_EXT_new"] = 1
    check(0, "new item")
    del probe["capabilities"]["device"]["features"]["features"]["unsupported"]
    check(0, "still advertised")  # unknown/unsupported is not an advertised baseline
    probe["capabilities"]["device"]["formats"] = {}
    check(1, "incomplete probe")
    print("feature-matrix selftest: OK")
