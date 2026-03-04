# MIX Language Server Protocol (LSP) — Implementation Complete

## Status: All 5 Phases Done

## Architecture

```
Editor (VS Code / Neovim)
    |
    | stdin/stdout (JSON-RPC)
    |
mix-lsp binary
    |
    +-- lsp_transport    (JSON-RPC framing over stdin/stdout)
    +-- lsp_json         (minimal JSON parser/emitter, ~300 lines)
    +-- lsp_server       (dispatch table, lifecycle, all handlers)
    +-- lsp_document     (per-document state, lex→parse→sema pipeline)
    +-- lsp_diagnostics  (error/warning collector → publishDiagnostics)
    +-- lsp_position     (token/AST lookup by cursor position)
    +-- lsp_symbols      (symbol → definition location hash index)
    +-- lsp_hover        (type_to_string for all 25 MIX types)
    +-- lsp_complete     (scope, keyword, dot, and string interpolation completion)
    |
    +-- [shared frontend] lexer, parser, sema, types, symtab, arena, errors
```

## Files Created

| File | Purpose |
|------|---------|
| `src/lsp/lsp_main.c` | Entry point, JSON-RPC message loop |
| `src/lsp/lsp_json.c/h` | JSON parser + JsonWriter emitter |
| `src/lsp/lsp_transport.c/h` | stdin/stdout JSON-RPC framing (Content-Length headers) |
| `src/lsp/lsp_diagnostics.c/h` | DiagnosticList collector, publishDiagnostics notification |
| `src/lsp/lsp_document.c/h` | DocumentStore (hash map by URI), per-doc arena, full re-analysis |
| `src/lsp/lsp_server.c/h` | LspServer struct, dispatch, initialize/shutdown + all feature handlers |
| `src/lsp/lsp_position.c/h` | `tokens_find_at()`, `ast_find_node_at()` (recursive walker for 30+ node kinds) |
| `src/lsp/lsp_hover.c/h` | `mix_type_to_string()` formatting all 25 type kinds |
| `src/lsp/lsp_symbols.c/h` | SymbolIndex (hash map), `symbol_index_build()` walks AST for functions, shapes, vars, consts |
| `editors/vscode-mix/src/extension.ts` | VS Code LSP client (auto-discovers mix-lsp binary) |
| `editors/vscode-mix/tsconfig.json` | TypeScript config for extension |
| `editors/nvim/mix.lua` | Neovim LSP config (vim.lsp.start) |

## Files Modified

| File | Change |
|------|--------|
| `src/errors.h/c` | Added `DiagSeverity`, `DiagnosticCallback`, `errors_set_callback()`, `errors_reset()` |
| `Makefile` | Split into frontend/compiler/LSP objects, added `mix-lsp` target |
| `editors/vscode-mix/package.json` | Added LSP client deps, `"main"`, `configurationDefaults` for string completion |

## Features Implemented

### Phase 1: Diagnostics
- Syntax and semantic errors reported as `textDocument/publishDiagnostics` notifications
- Red squiggly underlines appear as you type
- Errors clear when code is fixed
- Best-effort analysis: sema runs even with parse errors for partial results

### Phase 2: Hover
- `textDocument/hover` returns type info in markdown code blocks
- Works for all types: int, str, bool, float, list, map, shape, func, optional, shared, task
- Token-at-position lookup + AST node finder for resolved_type

### Phase 3: Go-to-Definition
- `textDocument/definition` jumps to symbol definition
- Symbol index built from AST: functions, shapes, constants, type aliases, local variables, for-loop variables, function parameters
- For-loop variable types correctly extracted from iterable element type

### Phase 4: Autocomplete
- `textDocument/completion` with multiple completion sources:
  - **General**: all symbols in file + 26 language keywords + 14 type keywords
  - **After `.`**: shape fields + methods, built-in methods for str (8), list (2), map (5), shared (2)
  - **Inside `{expr}`**: variables and dot-completion inside string interpolation
- Trigger character: `.`
- `configurationDefaults` enables auto-suggestions inside strings for MIX files

### Phase 5: Editor Extensions
- **VS Code**: extension.ts with `vscode-languageclient`, auto-discovers `mix-lsp` relative to extension path via symlink resolution
- **Neovim**: mix.lua with `vim.filetype.add` + `vim.lsp.start`

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Text sync | Full (not incremental) | MIX files are small; full re-lex/parse/sema per keystroke is fast |
| Memory | Per-document arena | Destroyed on re-analysis; avoids realloc pointer invalidation |
| JSON | Hand-rolled (~300 lines) | Zero dependencies; only needs LSP JSON subset |
| Errors | Callback hook in `errors.c` | One check in `report()`; fully backward-compatible with compiler |
| Symbol index | Walk AST (not modify symtab) | No changes to compiler's symbol table; LSP builds its own index |
| Transport | Raw stdin/stdout | Standard LSP; no socket/pipe complexity |

## Verification

- `make clean && make` — builds both `mix` and `mix-lsp` with zero errors
- `make test` — 37/37 compiler tests pass (error callback is backward-compatible)
- All features tested via manual JSON-RPC and in VS Code
