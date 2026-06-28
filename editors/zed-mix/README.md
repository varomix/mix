# MIX Language - Zed Extension

Editor support for MIX in Zed:

- `.mix` file detection
- Tree-sitter syntax highlighting
- bracket matching and basic indentation
- outline entries for functions, shapes, unions, type aliases, and constants
- LSP integration through `mix-lsp`

## Install locally

1. Build MIX and the language server from the repository root:

   ```sh
   make
   ```

2. Open Zed and run `zed: install dev extension` from the command palette.

3. Select this directory:

   ```text
   /path/to/mix_lang/editors/zed-mix
   ```

4. Open a `.mix` file.

The extension first tries to launch `build/mix-lsp` from the opened MIX repository. If that does not exist, it falls back to `mix-lsp` from `PATH`.

For local dev installs, the Tree-sitter grammar is loaded from `tree-sitter-mix/` via a `file://` repository URL in `extension.toml`. If you move the repository, update that URL or replace it with a pushed GitHub revision.

## Troubleshooting

- If completions, diagnostics, or hover do not appear, run `make` at the MIX repo root and reload Zed.
- If Zed reports `failed to fetch revision` for the MIX grammar, remove `editors/zed-mix/grammars/` and reinstall the dev extension.
- If you use MIX from another project, put `mix-lsp` on `PATH` so the extension can find it.
- Zed compiles dev extensions with Rust from `rustup`; a Homebrew-only Rust install may not work for extension development.
