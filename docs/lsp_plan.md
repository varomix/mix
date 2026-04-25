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
- `make test` — compiler tests still pass (error callback is backward-compatible)
- All features tested via manual JSON-RPC and in VS Code

---

# Phase 6+ — Roadmap

What ships today (Phases 1–5) covers diagnostics, hover, go-to-definition,
completion, and signature help. The capabilities below are the gaps that
make Neovim/VSCode default keymaps return nothing today, plus the
modern-feature features users now expect from a working LSP. Each phase
ships independently — pick and choose.

## Architecture context

- **Server** (`src/lsp/`, ~2.6k LOC C) does the real work. Both editor
  plugins are thin clients that just spawn it.
- The compiler pipeline (lex → parse → sema) already produces:
  - `SymbolEntry` per definition with `SrcLoc`, type, container (shape),
    param info (`src/lsp/lsp_symbols.h`)
  - Resolved types on every AST node (`AstNode.resolved_type`)
  - Diagnostics via callback (`lsp_diagnostic_callback`)
- Editor plugins (`editors/nvim/mix.lua`, `editors/vscode-mix/`) need
  updates only for: (a) on-attach UX, (b) capability-flag-driven UI
  hooks (e.g. inlay hint toggle), (c) settings.

So **most of the work is in `src/lsp/`**. Editor changes are small but
important for discoverability.

## Phase A — Stop the silent keymaps (1 day)

The cheapest win: advertise the capabilities we'll add and provide stub
handlers so default keymaps surface helpful "no result" responses
instead of nothing.

**Server** (`src/lsp/lsp_server.c`):
- Extend `handle_initialize` capabilities block to include
  `referencesProvider`, `documentSymbolProvider`,
  `workspaceSymbolProvider`, `renameProvider`,
  `documentHighlightProvider`, `inlayHintProvider`,
  `documentFormattingProvider`, `codeActionProvider`.
- Add dispatch arms for each (return empty array / `null` for now).

**Editor plugins**: no changes; they read capabilities from `initialize`
response.

**Why first**: zero risk, makes the server look complete to the editor,
lets us roll out actual implementations without re-touching the boot
path.

## Phase B — Document symbols (~1 day)

Outline view in VSCode and `gO` / `:Telescope lsp_document_symbols` in
Neovim.

**Server**:
- New file `src/lsp/lsp_outline.c` (~150 LOC).
- Iterate `doc->symbols.all[]` and emit hierarchical `DocumentSymbol[]`
  (functions at top, methods nested under their `container` shape).
- LSP `SymbolKind`: 12 = Function, 7 = Method, 23 = Struct (for shape),
  14 = Constant (for `@const`).
- Range comes from `def_loc`; selectionRange same.

**Editors**: nothing — once advertised, both clients use the outline
panel automatically.

## Phase C — Workspace symbols (~1.5 days)

`Cmd+T` in VSCode, `:Telescope lsp_workspace_symbols` in Neovim.
Project-wide fuzzy symbol search.

**Server**:
- On `initialize`, scan the workspace root (sent in `rootUri`) for
  `*.mix` files using a recursive walk capped at, say, 5000 files.
- For each, run lex/parse/sema once into a per-file `SymbolIndex`.
  Cache by absolute path + mtime; invalidate on `didChange`.
- `workspace/symbol` filters cached entries by name (case-insensitive
  substring or fuzzy).

**Editors**: nothing.

**Risk**: cold start for big projects. Mitigation: build asynchronously
after `initialized`, send progress via `$/progress`.

## Phase D — Find references + document highlight (~2 days)

`grr` / `:Telescope lsp_references`, plus auto-highlighting of all uses
of the symbol under cursor.

**Server changes are deeper here** because the existing `SymbolIndex`
only stores definitions, not uses.

- Extend `SymbolEntry` with a `Reference *uses` linked list (file URI +
  range).
- During `symbol_index_build`, walk the AST a second time collecting
  every `NODE_IDENT`, `NODE_CALL_EXPR.name`,
  `NODE_FIELD_EXPR.field_name`, `NODE_METHOD_CALL.method_name`,
  `NODE_SHAPE_LIT.shape_name`, and append to the matching definition's
  `uses` list.
- For workspace-wide references, walk every cached file's AST (Phase C
  cache).
- Implement `textDocument/references`: filter by `includeDeclaration`
  flag.
- Implement `textDocument/documentHighlight`: same query, but only the
  current document, returns `Read`/`Write` kind based on whether the
  use is on the LHS of an assignment.

**Editors**: nothing.

## Phase E — Live diagnostics (~0.5 day)

Currently diagnostics only publish on `didSave`. Modern LSPs publish on
`didChange` for live underlines.

**Server**:
- In `handle_did_change`, call `document_ensure_analyzed(doc)` and
  `lsp_publish_diagnostics(uri, &doc->diagnostics)` after the text
  update.
- Add a 250ms debounce to avoid analyzing on every keystroke.
  Implementation: store `last_change_time` per doc, only analyze if
  `now - last_change > debounce_ms`. Or: defer analysis to next message
  receive after a quiet period.

**Editors**: nothing.

## Phase F — Inlay hints (~1.5 days)

Show inferred types inline: `x = some_fn()  ▸ : int`. Modern feel, big
quality-of-life win.

**Server**:
- Implement `textDocument/inlayHint`: walk AST in the requested range;
  for each `NODE_VAR_DECL` without `type_ann`, emit a hint at the
  position right after the name with text `: <type_name>`.
- Skip when `type_ann` exists, when init is a literal that makes the
  type obvious (`x = 42`), when the var is `_`.
- Optional: hints for parameter names at call sites: `f(▸name: 42)`.

**Editors**:
- VSCode: enable by default (set `editor.inlayHints.enabled = "on"` in
  `configurationDefaults` in `package.json`).
- Neovim: add to `on_attach` in `mix.lua`:
  ```lua
  if client.server_capabilities.inlayHintProvider then
      vim.lsp.inlay_hint.enable(true, { bufnr = bufnr })
  end
  ```

## Phase G — Rename (~1.5 days, depends on Phase D)

`grn` / F2. Refactor-rename across all uses.

**Server**:
- `textDocument/prepareRename`: confirm the position is on a renameable
  identifier; return its range.
- `textDocument/rename`: build a `WorkspaceEdit` whose changes map
  `uri → TextEdit[]`. Iterate the symbol's `uses` list (Phase D) and
  create one `TextEdit` per occurrence.
- Reject renames that would shadow a builtin, conflict with a keyword,
  or clash with an existing same-scope name. Sema already knows the
  rules — share the validator.

**Editors**: nothing.

## Phase H — Code actions (~1–2 days, ongoing)

Quick fixes triggered by diagnostics. Three high-value ones to start:

**Server**:
- "Did you mean `X`?" → replace identifier. Sema already produces these
  as `note:`s; surface the suggested name in the diagnostic data field,
  then synthesize the edit.
- "Add `~` to function with side effects" — when sema warns about an
  unmarked side-effecting function.
- "Wrap in `unsafe { }`" for pointer derefs outside `unsafe` blocks.

Implement `textDocument/codeAction` returning `CodeAction[]`. Each
action carries an `edit: WorkspaceEdit`.

**Editors**: nothing — `gra` / Cmd+. just works.

## Phase I — Formatting [shipped]

`mix fmt` is a token-based formatter (`src/fmt.{c,h}`) that lexes the
input, walks tokens, and re-emits with normalized whitespace,
indentation derived from INDENT/DEDENT, and standalone or trailing
comments preserved via a separate source-scanning pass.

Idempotent: `format(format(x)) == format(x)`.

**Server**: `textDocument/formatting` handler in `lsp_server.c` calls
`mix_format` directly (no shelling out). Returns a single TextEdit
replacing the whole buffer.

**Editors**:
- VSCode: opt-in for `editor.formatOnSave` with `[mix]` scope (TODO —
  the capability is advertised; users can enable it themselves today).
- Neovim: `vim.lsp.buf.format()` works out of the box; a `BufWritePre`
  autocmd snippet for users who want format-on-save is a future README
  addition.

Limitations of v1:
- No reflow of long lines.
- No restructuring of code (won't fix mismatched indentation that
  changes semantics).
- Trailing comment placement is preserved; there's no canonical
  alignment for column-based layouts.

## Phase J — Editor UX polish (~1 day, parallelizable)

Independent of server changes.

**Neovim** (`editors/nvim/mix.lua`):
- Add a documented `on_attach` callback that:
  - Lists the keymaps users get for free in 0.10+ (`gd`, `K`, `grr`,
    `grn`, `gra`, `gO`).
  - Calls `vim.lsp.completion.enable(client, bufnr, true, { autotrigger = true })`
    for autocompletion-as-you-type (0.11+).
  - Calls `vim.lsp.inlay_hint.enable(true, { bufnr = bufnr })` if the
    server advertises.
  - Sets up `:MixLspRestart` user command.
- Add a `health.lua` for `:checkhealth mix` that reports binary
  version, server reachability, workspace root.
- README block in the file header showing the recommended `:LspInfo`
  workflow.

**VSCode** (`editors/vscode-mix/`):
- `package.json`: add command palette entries for `mix.restartServer`,
  `mix.showOutput`, `mix.toggleInlayHints`.
- `extension.ts`: register those commands.
- README: settings table, screenshot of features.
- Bump `engines.vscode` to `^1.85.0` for inlay hint API stability if
  shipping Phase F.

## Phase K — Tree-sitter grammar (separate epic, optional)

Mentioned for completeness. A real `tree-sitter-mix` parser would
replace the `syntax/mix.vim` regex-based highlighter with structural
highlighting, give Neovim text objects, fix folding, and let VSCode
lean less on its TextMate grammar. Probably 3–5 days for someone
familiar with tree-sitter. Not LSP work, but it's the other
UX-amplifying lever.

## Recommended order if shipping incrementally

1. **A + E** (1.5 days) — capability stubs + live diagnostics.
   Immediate "feels alive" upgrade.
2. **B + C** (2.5 days) — outline + workspace symbols. Navigation gets
   dramatically better.
3. **D** (2 days) — references + highlight. Now refactoring becomes
   possible to plan.
4. **G** (1.5 days) — rename, building on D.
5. **F** (1.5 days) — inlay hints. The headline modern feature.
6. **J** (1 day, can be done anytime) — editor polish.
7. **H** (ongoing) — code actions, add as we identify common pain.

**Total: ~10 working days** to a feature-complete LSP for both editors.
Phase A alone is 1 day and gets you 80% of the perceived "completeness"
because it stops the silent default keymaps.
