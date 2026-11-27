#!/usr/bin/env python3
"""Internal soak loop, run only inside smoke's guarded session."""
import importlib.util
import json
import math
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading
import zipfile

M = Path(os.environ.get("M", Path(os.environ.get("MALI_LAB_ROOT", Path.home() / ".local/share/mali-phone-lab"))))
spec = importlib.util.spec_from_file_location("schedule", M / "bin/soak-schedule.py")
schedule = importlib.util.module_from_spec(spec)
spec.loader.exec_module(schedule)
patterns = {
    "unsupported": r"mali_cmd:.*unsupported",
    "fatal": r"mali_vk:.*FATAL",
    "TRANSLATION_FAULT": r"TRANSLATION_FAULT",
    "DEVICE_LOST": r"DEVICE_LOST",
    "GUILTY_CONTEXT_RESET": r"GUILTY_CONTEXT_RESET",
    "SWIFTSHADER": r"SWIFTSHADER",
    "CrashCount": r"CrashCount",
    "resolve_1x": r"1x resolve as",
}
loss = ("fatal", "TRANSLATION_FAULT", "DEVICE_LOST", "GUILTY_CONTEXT_RESET", "SWIFTSHADER")


def command(args, timeout=25):
    return subprocess.run(args, text=True, capture_output=True, timeout=timeout, check=True).stdout


def json_output(text):
    return json.loads(text[text.index("{"):])


def pacing_summary(data):
    hist = data.get("presentToPresentMs") or {}
    bins = sorted((int(ms), int(n)) for ms, n in hist.items() if int(n) > 0)
    total = sum(n for _, n in bins)
    def percentile(p):
        rank, seen = math.ceil(total * p), 0
        for ms, n in bins:
            seen += n
            if seen >= rank:
                return ms
        return None
    return {"displayed_fps": data.get("fpsOnScreen"), "frames": data.get("frames"),
            "p95_interval_ms": percentile(.95), "p99_interval_ms": percentile(.99),
            "raw": data}


def run(phone, out, minutes, scroll=0):
    ddc = str(M / "bin/ddc")
    out = Path(out)
    result = {"screenshots": [], "pacing_screenshot": None, "gpu_pid_restarts": 0,
              "first_marker": None, "pacing": [], "counters": None,
              "scenario": f"steam_shelf_scroll_right_every_{scroll}s" if scroll else "steam_shelf_idle"}
    previous_pids = None
    baseline = None
    failure = None
    samples = queue.Queue()
    sample_errors = []
    write_lock = threading.Lock()
    with (out / "timeline.tsv").open("w", buffering=1) as timeline:
        timeline.write("scheduled_s\tactual_s\tevent\tdetail\n")

        def call(*args, timeout=25):
            return command([ddc, phone, *map(str, args)], timeout)

        def log(due, actual, event, detail):
            with write_lock:
                timeline.write(f"{due}\t{actual:.3f}\t{event}\t{json.dumps(detail, ensure_ascii=False)}\n")

        def shot(name):
            png, jpg = out / (name + ".png"), out / (name + ".jpg")
            call("screenshot", png, timeout=40)
            command([str(M / "bin/img"), "jpg", str(png), str(jpg)], 20)
            if not jpg.is_file() or not jpg.stat().st_size:
                raise RuntimeError("screenshot missing")
            result["screenshots"].append(str(jpg))
            return str(jpg)

        def sample(due):
            nonlocal previous_pids, baseline
            state = json_output(call("state"))
            if not state.get("session", {}).get("running"):
                raise RuntimeError("session not running")
            # Exclude the grep process itself using a bracketed regex.
            text = json_output(call("guest", "--", "sh", "-c",
                "ps -eo pid,args | grep -E '[s]teamwebhelper.*--type=gpu-process' || true"))["stdout"]["text"]
            pids = sorted({int(line.split(None, 1)[0]) for line in text.splitlines()
                           if line.split(None, 1) and line.split(None, 1)[0].isdigit()})
            if previous_pids is not None and pids and pids != previous_pids:
                result["gpu_pid_restarts"] += len(set(pids) - set(previous_pids))
            previous_pids = pids
            logs = out / ("live-logs-%04d" % due)
            logs.mkdir(exist_ok=True)
            call("logs", "latest", logs, timeout=60)
            for z in logs.rglob("*.zip"):
                with zipfile.ZipFile(z) as archive:
                    for info in archive.infolist():
                        target = (logs / info.filename).resolve()
                        if not target.is_relative_to(logs.resolve()):
                            raise RuntimeError("unsafe log archive")
                    archive.extractall(logs)
            counts = dict.fromkeys(patterns, 0)
            feature = None
            for path in sorted(logs.rglob("*")):
                if not path.is_file() or path.suffix.lower() == ".zip":
                    continue
                with path.open(errors="replace") as f:
                    for line in f:
                        for name, regex in patterns.items():
                            if re.search(regex, line, re.I):
                                counts[name] += 1
                        m = re.search(r"\[\s*gpu_compositing\s*\]\s*:\s*(\S+)", line, re.I)
                        if m:
                            feature = m.group(1)
            if baseline is None:
                baseline = counts.copy()
            delta = {k: max(0, v - baseline[k]) for k, v in counts.items()}
            if result["first_marker"] is None:
                for name in loss:
                    if delta[name]:
                        result["first_marker"] = {"marker": name, "time_s": due}
                        break
            result["counters"] = delta
            result["gpu_compositing"] = feature
            return {"pids": pids, "restarts": result["gpu_pid_restarts"],
                    "counters": delta, "gpu_compositing": feature}

        def event(due, actual, action, index):
            if sample_errors:
                raise RuntimeError("background sample failed: " + sample_errors[0])
            if action == "screenshot":
                detail = shot("soak-" + index)
            elif action == "sample":
                samples.put((due, actual))
                return
            else:
                state = json_output(call("state"))
                if not state.get("session", {}).get("running"):
                    raise RuntimeError("session not running before input")
                call("input", "pad", action)
                detail = index
            log(due, actual, action, detail)

        def reader():
            while True:
                item = samples.get()
                try:
                    if item is None:
                        return
                    due, actual = item
                    if not sample_errors:
                        log(due, actual, "sample", sample(int(due)))
                except Exception as exc:
                    sample_errors.append(str(exc))
                    log(due, actual, "sample_error", str(exc))
                finally:
                    samples.task_done()

        thread = threading.Thread(target=reader, name="soak-log-reader", daemon=True)
        thread.start()
        pacing_thread = None
        try:
            if minutes:
                schedule.run(minutes, event)
            else:
                samples.put((0, 0))
            call("input", "pad", "b")
            result["pacing_screenshot"] = shot("pre-pacing")
            for i in range(3):
                capture = {}
                ready = threading.Event()
                def capture_pacing():
                    ready.set()
                    try:
                        capture["raw"] = call("pacing", "--seconds", "30", timeout=90)
                    except Exception as exc:
                        capture["error"] = str(exc)
                pacing_thread = threading.Thread(target=capture_pacing, daemon=True)
                pacing_thread.start()
                ready.wait()
                base = minutes * 60 + i * 30
                def input_tick(due, actual):
                    if sample_errors:
                        raise RuntimeError("background sample failed: " + sample_errors[0])
                    call("input", "pad", "right")
                    log(base + due, base + actual, "pacing_right", i + 1)
                schedule.pacing(30, scroll, input_tick)
                pacing_thread.join(timeout=95)
                if pacing_thread.is_alive() or "error" in capture:
                    raise RuntimeError("pacing failed: " + capture.get("error", "timeout"))
                result["pacing"].append(pacing_summary(json_output(capture["raw"])))
                log(base + 30, base + 30, "pacing", i + 1)
                if i == 1:
                    samples.put((base + 30, base + 30))
            samples.put(None)
            thread.join()
            if sample_errors:
                raise RuntimeError("background sample failed: " + sample_errors[0])
        except (OSError, subprocess.SubprocessError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile) as exc:
            failure = str(exc)
            log(-1, -1, "error", failure)
        finally:
            if pacing_thread is not None and pacing_thread.is_alive():
                pacing_thread.join(timeout=95)
            if thread.is_alive():
                samples.put(None)
                thread.join()
    result["error"] = failure
    (out / "soak-data.json").write_text(json.dumps(result) + "\n")
    if failure:
        return 4
    return 3 if result["first_marker"] else 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])))
