# MIX Language — VS Code Extension

Syntax highlighting for the [MIX programming language](https://github.com/varomix/mix-lang).

## Features

- Full syntax highlighting for `.mix` files
- Keywords, control flow, types, operators
- String interpolation with `{expr}`
- Compile-time directives (`@const`, `@os`, `@arch`, `@debug`, `@release`)
- Generic type parameters (`@T`, `@T has method`)
- Shape declarations and tagged unions
- `extern` C interop blocks
- Built-in function recognition
- Bracket matching and auto-closing
- Indentation-based folding

## Install (local)

1. Copy or symlink this folder into your VS Code extensions directory:

   ```sh
   # macOS / Linux
   ln -s "$(pwd)" ~/.vscode/extensions/mix-lang

   # or copy
   cp -r . ~/.vscode/extensions/mix-lang
   ```

2. Reload VS Code (`Cmd+Shift+P` → "Developer: Reload Window")

3. Open any `.mix` file — syntax highlighting is automatic.
