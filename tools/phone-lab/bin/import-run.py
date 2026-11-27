#!/usr/bin/env python3
"""Internal locked import workflow; never automatically retries an import."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

M = Path(os.environ.get("M", Path.home() / ".config/opencode/mali-orchestrator"))
CACHE = Path(os.environ.get("CACHE", Path.home() / ".cache/mali-orchestrator"))
PICKER = "Choose a driver zip or an Android + Linux bundle"
active = {"phone": None, "snapshot": None, "hung": False, "restored": False}


def call(args, timeout=30):
    return subprocess.run([str(x) for x in args], capture_output=True, text=True,
                          check=True, timeout=timeout).stdout


def obj(text):
    return json.loads(text[text.index("{"):])


def nodes(dump):
    windows = dump.get("windows")
    if dump.get("ok") is not True or not isinstance(windows, list) or not windows:
        raise ValueError("incomplete UI dump")
    return [n for w in windows for n in w.get("nodes", []) if isinstance(n, dict)]


def classify(dump):
    ns = nodes(dump)
    texts = [str(n.get("text", "")) for n in ns]
    picker = any(PICKER in t for t in texts)
    components = any(n.get("tag") == "rail-components" for n in ns)
    failure = next((t for t in texts if "Import failed:" in t), None)
    toast = any(t.startswith("Imported ") or
                (t.startswith("Using ") and "as both the runtime and display driver" in t)
                for t in texts)
    return picker, components, toast, failure


def registered(registry, identifier):
    values = registry.get("values")
    if not isinstance(values, dict):
        raise ValueError("registry unavailable")
    value = values.get(identifier)
    if isinstance(value, str):
        value = json.loads(value)
    if value is None:
        return None
    if not isinstance(value, dict) or not value.get("linux") or not value.get("android"):
        raise ValueError("invalid bundle registry entry")
    return value


def watch(dump, registry, identifier, deadline, clock=None, sleep=None):
    """Bound all calls by the remaining deadline; never retry the import."""
    clock = clock or time.monotonic
    sleep = sleep or time.sleep
    while clock() < deadline:
        remaining = deadline - clock()
        try:
            ui = dump(remaining)
            picker, components, toast, failure = classify(ui)
            if failure:
                return "IMPORT_FAILED", None
            entry = registry(deadline - clock()) if not picker and components else None
            if not picker and components and (toast or entry):
                # Selection requires the exact registry Linux ID, never a guessed name.
                return "IMPORTED", entry
        except (OSError, ValueError, KeyError, subprocess.SubprocessError):
            pass
        left = deadline - clock()
        if left > 0:
            sleep(min(1, left))
    return "IMPORT_HUNG", None


def main(phone, path, select):
    path = Path(path).resolve()
    if not re.fullmatch(r"[A-Za-z0-9._-]+\.zip", path.name):
        raise ValueError("zip filename must contain only ASCII letters, digits, dot, dash and underscore")
    with zipfile.ZipFile(path) as z:
        manifest = json.loads(z.read("manifest.json"))
        ident = manifest["tag"]
        library = z.read(manifest["drivers"]["linux"]["path"].rstrip("/") + "/libvulkan_panfrost.so")
        if not re.fullmatch(r"[A-Za-z0-9-]+", ident):
            raise ValueError("unsafe bundle id")
        digest = hashlib.sha256(library).hexdigest()
    ddc = [M / "bin/ddc", phone]
    def run(*args, timeout=30):
        return call(ddc + list(args), timeout)
    def reg(timeout=30):
        if timeout <= 0:
            raise TimeoutError()
        return registered(obj(run("prefs", "get", "driver_bundles", timeout=min(20, timeout))), ident)
    if reg():
        raise ValueError("bundle already registered: " + ident)
    snap = call([M / "bin/phone-snap", phone, "import"], 90)
    match = re.search(r"(?m)^SNAP=(.+)$", snap)
    if not match:
        raise ValueError("snapshot failed")
    snapshot = match.group(1)
    active.update(phone=phone, snapshot=snapshot)
    target = "/sdcard/Download/" + path.name
    def ledger(what, original, undo):
        call([M / "bin/ledger", phone, "add", what, original, undo], 20)
    ledger("import bundle " + ident + " and its automatic driver selections",
           "not registered; prefs in " + snapshot,
           "remove bundle in GPU drivers; phone-restore " + phone + " " + snapshot + " --apply")
    ledger("push " + target, "absent (verify before deletion)",
           "ddc " + phone + " adb shell rm " + target)
    # Refuse to overwrite an existing phone file.
    run("adb", "shell", "sh", "-c", f"test ! -e {target!r}")
    run("adb", "push", str(path), target, timeout=120)
    run("ui", "click", "rail-components")
    run("ui", "click", "--match", "Import .zip")
    ui = obj(run("ui", "dump"))
    if not classify(ui)[0]:
        raise ValueError("picker not positively identified")
    # Only navigate by visible semantic labels; never guess a swipe coordinate.
    text = [str(n.get("text", "")) for n in nodes(ui)]
    if not any(path.name in t for t in text):
        if not any(t == "Download" for t in text):
            raise ValueError("Download directory not visible in picker")
        run("ui", "click", "--match", "Download")
    deadline = time.monotonic() + 90
    try:
        run("ui", "click", "--match", path.name, timeout=90)
        verdict, entry = watch(lambda t: obj(run("ui", "dump", timeout=min(10, t))),
                               reg, ident, deadline)
    except subprocess.TimeoutExpired:
        verdict, entry = "IMPORT_HUNG", None
    output = {"phone": phone, "verdict": verdict, "id": ident, "sha256": digest,
              "snapshot": snapshot, "verify_next_session": True}
    if verdict == "IMPORT_HUNG":
        active["hung"] = True
        try:
            run("adb", "shell", "input", "keyevent", "BACK", timeout=10)
        except subprocess.SubprocessError:
            pass
        shot = CACHE / "import" / (ident + "-hung.png")
        try:
            run("screenshot", str(shot), timeout=15)
            output["screenshot"] = str(shot)
        except subprocess.SubprocessError:
            output["screenshot_error"] = True
        print(json.dumps(output))
        return 1
    if verdict != "IMPORTED" or not entry:
        if verdict == "IMPORTED":
            output["verdict"] = "REGISTRY_UNVERIFIED"
        call([M / "bin/phone-restore", phone, snapshot, "--apply"], 90)
        output["restore_attempted"] = True
        output["restored"] = False
        print(json.dumps(output))
        return 1
    call([M / "bin/phone-restore", phone, snapshot, "--apply"], 90)
    expected = obj(Path(snapshot, "prefs-session.json").read_text()).get("values", {})
    actual = obj(run("prefs", "get", "session")).get("values", {})
    for key in ("linuxDriver", "androidDriver", "gpuDriverMode", "vkBridge"):
        if actual.get(key) != expected.get(key):
            raise ValueError("restore verification failed for " + key)
    active["restored"] = True
    if select:
        before = obj(run("prefs", "get", "session")).get("values", {}).get("linuxDriver", "")
        ledger("select linuxDriver " + entry["linux"], str(before),
               "phone-restore " + phone + " " + snapshot + " --apply")
        run("prefs", "set", "session", "linuxDriver", entry["linux"], "--type", "string")
    output["linux_id"] = entry["linux"]
    output["verify_next_session"] = {"linux_id": entry["linux"], "sha256": digest}
    print(json.dumps(output))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1], sys.argv[2], "--select" in sys.argv[3:]))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as exc:
        result = {"verdict": "IMPORT_FAILED", "reason": str(exc), "restored": active["restored"]}
        if active["snapshot"] and not active["hung"] and not active["restored"]:
            try:
                call([M / "bin/phone-restore", active["phone"], active["snapshot"], "--apply"], 90)
                result["restore_attempted"] = True
            except (OSError, subprocess.SubprocessError) as restore_error:
                result["restore_error"] = str(restore_error)
        print(json.dumps(result))
        sys.exit(1)
