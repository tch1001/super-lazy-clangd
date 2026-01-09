#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import sys
from typing import Any, Dict


def send(proc: subprocess.Popen, msg: Dict[str, Any]) -> None:
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    assert proc.stdin is not None
    proc.stdin.write(header + body)
    proc.stdin.flush()


def recv(proc: subprocess.Popen) -> Dict[str, Any]:
    assert proc.stdout is not None
    content_length = None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("EOF reading headers (server likely exited)")
        s = line.decode("ascii", errors="replace").strip()
        if not s:
            break
        if s.lower().startswith("content-length:"):
            content_length = int(s.split(":", 1)[1].strip())
    if content_length is None:
        raise RuntimeError("Missing Content-Length")
    body = proc.stdout.read(content_length)
    return json.loads(body.decode("utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("query", help="workspace/symbol query string (fixed-string grep)")
    ap.add_argument("--definition", action="store_true", help="also send textDocument/definition at a dummy position")
    ap.add_argument("--files", nargs="*", default=None, help="restrict server search to these files")
    ap.add_argument("--trace", action="store_true", help="enable server trace (sets SLCLANGD_TRACE=1)")
    args = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    exe = os.path.join(root, "build", "super-lazy-clangd")
    if not os.path.exists(exe):
        print(f"missing server executable: {exe}", file=sys.stderr)
        print("run: meson setup build && meson compile -C build", file=sys.stderr)
        return 2

    cmd = [exe]
    if args.files is not None:
        cmd += ["--files"] + args.files

    env = os.environ.copy()
    if args.trace:
        env["SLCLANGD_TRACE"] = "1"

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=root,
        env=env,
    )

    try:
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"rootUri": "file://" + root, "capabilities": {}},
            },
        )
        r1 = recv(proc)
        print("initialize response:", json.dumps(r1, indent=2))

        send(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "workspace/symbol", "params": {"query": args.query}})
        r2 = recv(proc)
        print("workspace/symbol response:", json.dumps(r2, indent=2))

        if args.definition:
            # Best-effort: open a file and request definition at the first
            # occurrence of the query string in that file (wire-format validation).
            test_path = None
            if args.files:
                test_path = args.files[0]
            if not test_path:
                test_path = os.path.join(root, "src", "lsp_server.cpp")
            if not os.path.isabs(test_path):
                test_path = os.path.join(root, test_path)
            test_text = open(test_path, "r", encoding="utf-8").read()
            idx = test_text.find(args.query)
            if idx >= 0:
                line = test_text.count("\n", 0, idx)
                col = idx - (test_text.rfind("\n", 0, idx) + 1)
            else:
                line, col = 0, 0

            test_uri = "file://" + test_path
            send(
                proc,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": test_uri,
                            "languageId": "cpp",
                            "version": 1,
                            "text": test_text,
                        }
                    },
                },
            )
            send(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 22,
                    "method": "textDocument/definition",
                    "params": {"textDocument": {"uri": test_uri}, "position": {"line": line, "character": col}},
                },
            )
            rdef = recv(proc)
            print("textDocument/definition response:", json.dumps(rdef, indent=2))

        send(proc, {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None})
        r3 = recv(proc)
        print("shutdown response:", json.dumps(r3, indent=2))

        send(proc, {"jsonrpc": "2.0", "method": "exit", "params": None})
        proc.wait(timeout=5)
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
        if proc.stderr is not None:
            err = proc.stderr.read().decode("utf-8", errors="replace")
            if err.strip():
                print("\n--- server stderr ---", file=sys.stderr)
                print(err, file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())


