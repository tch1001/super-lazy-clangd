# super-lazy-clangd

Tiny C++ Language Server Protocol (LSP) skeleton built with **Meson**, modeled after `clangd` only in shape (stdio JSON-RPC), but with intentionally tiny features. Under the hood it uses **GNU `grep`** for searching.

## Features

- `initialize` / `shutdown` / `exit`
- `textDocument/didOpen`, `textDocument/didChange` (full sync), `textDocument/didClose`
- `workspace/symbol`: fixed-string grep across the workspace root
- `textDocument/hover`: grabs the word under cursor and greps for the first match
- `textDocument/definition`: ctrl+click in editors (grep word-under-cursor, returns matching locations)
- `textDocument/references`: grep-based references

## Build

```bash
meson setup build
meson compile -C build
```

Run (stdio):

```bash
./build/super-lazy-clangd
```

## Smoke test

```bash
python3 tools/lsp_smoke.py
```

## LSP references

- LSP specification: `https://microsoft.github.io/language-server-protocol/specifications/specification-current/`
- JSON-RPC framing: `Content-Length: <bytes>\\r\\n\\r\\n<json>`


