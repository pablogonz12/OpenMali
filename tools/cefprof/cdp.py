#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Minimal stdlib DevTools client: cdp.py <user-data-dir> gpuinfo | eval <js> [url-substring]."""
import base64, json, os, socket, struct, sys, time, urllib.request


class WS:
    def __init__(self, port, path):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=20)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(1)
        if b" 101 " not in buf.split(b"\r\n")[0]:
            raise SystemExit("websocket refused: %r" % buf[:200])
        self.n = 0

    def _read(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise SystemExit("websocket closed")
            b += c
        return b

    def call(self, method, params=None):
        self.n += 1
        data = json.dumps({"id": self.n, "method": method, "params": params or {}}).encode()
        ln = len(data)
        head = bytes([0x81]) + (bytes([0x80 | ln]) if ln < 126 else
                                bytes([0xFE]) + struct.pack(">H", ln) if ln < 65536 else bytes([0xFF]) + struct.pack(">Q", ln))
        mask = os.urandom(4)
        self.s.sendall(head + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))
        msg = b""
        while True:
            b0, b1 = self._read(2)
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._read(8))[0]
            msg += self._read(ln)
            if not b0 & 0x80:
                continue
            m, msg = json.loads(msg), b""
            if m.get("id") == self.n:
                return m


def endpoint(udd):
    for _ in range(100):
        try:
            port, path = open(os.path.join(udd, "DevToolsActivePort")).read().split()
            return int(port), path
        except (OSError, ValueError):
            time.sleep(0.2)
    raise SystemExit("no DevToolsActivePort")


def main():
    port, path = endpoint(sys.argv[1])
    if sys.argv[2] == "gpuinfo":
        r = WS(port, path).call("SystemInfo.getInfo")["result"]["gpu"]
        print(json.dumps({k: r.get(k) for k in ("devices", "auxAttributes", "featureStatus", "driverBugWorkarounds")}, indent=1))
        return
    match = sys.argv[4] if len(sys.argv) > 4 else ""
    tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=20))
    tab = next(t for t in tabs if t["type"] == "page" and match in t["url"])
    ws = WS(port, tab["webSocketDebuggerUrl"].split(str(port), 1)[1])
    r = ws.call("Runtime.evaluate", {"expression": sys.argv[3], "returnByValue": True, "awaitPromise": True})
    print(json.dumps(r["result"]["result"].get("value", r["result"])))


main()
