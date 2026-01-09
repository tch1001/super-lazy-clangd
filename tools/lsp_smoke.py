#!/usr/bin/env python3

import json
import os
import subprocess
import sys
from typing import Any, Dict, Optional, Tuple


def send(proc: subprocess.Popen, msg: Dict[str, Any]) -> None:
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    proc.stdin.write(header + body)
    proc.stdin.flush()


def recv(proc: subprocess.Popen) -> Dict[str, Any]:
    # Read headers
    content_length = None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("EOF reading headers")
        line = line.decode("ascii", errors="replace").strip()
        if not line:
            break
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
    if content_length is None:
        raise RuntimeError("Missing Content-Length")

    body = proc.stdout.read(content_length)
    if len(body) != content_length:
        raise RuntimeError("Short read body")
    return json.loads(body.decode("utf-8"))


def main() -> int:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    exe = os.path.join(root, "build", "super-lazy-clangd")
    if not os.path.exists(exe):
        print(f"missing server executable: {exe}", file=sys.stderr)
        print("run: meson setup build && meson compile -C build", file=sys.stderr)
        return 2

    proc = subprocess.Popen(
        [exe, "--files", os.path.join(root, "src", "lsp_server.cpp")],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=root,
    )
    assert proc.stdin and proc.stdout

    send(
        proc,
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"rootUri": "file://" + root, "capabilities": {}},
        },
    )
    resp = recv(proc)
    assert resp.get("id") == 1, resp
    assert "result" in resp and "capabilities" in resp["result"], resp

    send(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

    # Query should hit src/lsp_server.cpp (we restricted search via --files).
    # Use a token that appears in code, not inside string literals.
    send(proc, {"jsonrpc": "2.0", "id": 2, "method": "workspace/symbol", "params": {"query": "Server::"}})
    resp2 = recv(proc)
    assert resp2.get("id") == 2, resp2
    assert isinstance(resp2.get("result"), list), resp2
    if len(resp2["result"]) == 0:
        raise RuntimeError("expected at least one workspace/symbol match")

    send(proc, {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None})
    resp3 = recv(proc)
    assert resp3.get("id") == 3, resp3

    send(proc, {"jsonrpc": "2.0", "method": "exit", "params": None})
    proc.wait(timeout=5)
    print("OK: initialize + workspace/symbol + shutdown/exit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


