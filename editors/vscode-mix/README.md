# MIX Language — VS Code Extension

Editor support for the [MIX programming language](https://github.com/varomix/mix-lang) — syntax highlighting plus a full LSP integration powered by `mix-lsp`.

## Features

### Syntax (no server required)
- Full syntax highlighting for `.mix` files
- Keywords, control flow, types, operators
- String interpolation `"... {expr} ..."`
- Compile-time directives (`@const`, `@os`, `@arch`, `@debug`, `@release`)
- Generic type parameters (`@T`, `@T has method`)
- Shape declarations and tagged unions
- `extern` C interop blocks
- Built-in function recognition
- Bracket matching and auto-closing
- Indentation-based folding

### Language server (when `mix-lsp` is available)
| Feature | What it does |
|---|---|
| Diagnostics | Live errors, warnings, "did you mean" notes |
| Hover (`K`) | Type info for identifiers and expressions |
| Go-to-definition (`F12`) | Jump to function / shape / variable definition |
| Find references (`Shift+F12`) | All uses across the workspace |
| Document highlight | Auto-highlights symbol under cursor |
| Document symbols | Outline view (functions, shapes, methods, constants) |
| Workspace symbols (`Cmd+T` / `Ctrl+T`) | Fuzzy symbol search across `.mix` files |
| Rename (`F2`) | Cross-file refactor-rename |
| Inlay hints | Inferred types shown inline |
| Code actions (`Cmd+.` / `Ctrl+.`) | Quick fixes (e.g. "Replace with 'X'") |
| Signature help | Parameter info as you type call args |
| Completion | Identifiers, fields, methods, builtins |

## Settings

| Setting | Default | Description |
|---|---|---|
| `mix.lsp.path` | `""` | Absolute path to the `mix-lsp` binary. Empty = use PATH (or auto-discover relative to extension). |
| `editor.inlayHints.enabled` (for `[mix]`) | `"on"` | Show inferred-type hints. Toggle via the command palette. |

## Commands

Invoke from the Command Palette (`Cmd+Shift+P` / `Ctrl+Shift+P`):

| Command | Effect |
|---|---|
| `MIX: Restart Language Server` | Stop and re-spawn `mix-lsp` (use after rebuilding). |
| `MIX: Show Server Output` | Open the `mix-lsp` output channel for debugging. |
| `MIX: Toggle Inlay Hints` | Flip `editor.inlayHints.enabled` for `[mix]`. |

## Install (local)

1. Build the language server from the project root:

   ```sh
   make
   ```

   The extension auto-discovers `build/mix-lsp` relative to its own location, so a symlinked extension will find a freshly built server.

2. Copy or symlink this folder into your VS Code extensions directory:

   ```sh
   # macOS / Linux
   ln -s "$(pwd)" ~/.vscode/extensions/mix-lang

   # or copy
   cp -r . ~/.vscode/extensions/mix-lang
   ```

3. Reload VS Code (`Cmd+Shift+P` → "Developer: Reload Window")

4. Open any `.mix` file — syntax highlighting is automatic; LSP attaches if `mix-lsp` is found.

## Troubleshooting

- **No diagnostics or completions** — run `MIX: Show Server Output` to see what the server is logging.
- **`mix-lsp` not found** — set `mix.lsp.path` in your settings to an absolute path, or symlink `build/mix-lsp` into `/usr/local/bin`.
- **Stale results after rebuilding the server** — run `MIX: Restart Language Server`.

## Requirements

- VS Code 1.85.0 or later (for stable inlay hint API)
- `mix-lsp` binary built from this project's `make`
