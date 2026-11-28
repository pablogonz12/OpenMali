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
    base = {"extensions": ["VK_EXT_present"], "features": {"present": True, "unsupported": False},
            "formats": {"VK_FORMAT_TEST": ["SAMPLED_IMAGE"]},
            "id": {"deviceName": "fake", "apiVersion": "1.3"}}
    probe = {"capabilities": {"device": {
        "extensions": {"VK_EXT_present": 1},
        "features": {"features": {"present": True, "unsupported": False}},
        "formats": {"VK_FORMAT_TEST": {"properties": {"optimalTilingFeatures":
                    ["VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"]}}},
        "properties": {"properties": {"deviceName": "fake", "apiVersion": "1.3"}}}}}
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
    probe["capabilities"]["device"]["features"]["features"]["present"] = False
    check(1, "feature present")
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
