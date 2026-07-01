# tree-sitter-mix

Tree-sitter grammar for the [MIX programming language](https://github.com/varomix/mix-lang).

## Status: v0.1 (preview)

This grammar covers MIX's lexical structure and most of its declaration / expression syntax. It is **token-faithful** — keywords, operators, identifiers, literals, type names all parse correctly — and produces useful parse trees for syntax highlighting.

It is **not** structure-faithful: MIX uses indentation for blocks, and faithful block grouping requires a tree-sitter external scanner (`scanner.c`) that emits `INDENT` / `DEDENT` tokens. That's planned for v1; v0 leaves blocks loose.

## Build

```sh
npm install --global tree-sitter-cli
cd editors/tree-sitter-mix
npm install
npm run build
```

## Use with Neovim (`nvim-treesitter`)

Once a grammar registry entry is published you'll get it through the regular `:TSInstall mix` flow. For local development:

1. Build the parser as above.
2. Add to your `init.lua`:
   ```lua
   require('nvim-treesitter.parsers').get_parser_configs().mix = {
     install_info = {
       url = '/abs/path/to/mix_lang/editors/tree-sitter-mix',
       files = { 'src/parser.c' },
       branch = 'main',
     },
     filetype = 'mix',
   }
   ```
3. `:TSInstallFromGrammar mix`

## What works in v0

- Comments, strings (incl. interpolation literals), numbers, booleans
- All keywords (`if`, `else`, `match`, `shape`, `pub`, `defer`, `zone`, ...)
- Type keywords (`int`, `float`, `str`, `int32`, ...) highlighted as types
- Function and shape declarations (name + params + return type)
- Use declarations (paths and aliases)
- Generic instantiation syntax (`Stack[int]`)
- Method calls, field access, indexing
- Operator highlighting (`->`, `=>`, `?`, `~`, etc.)

## Known gaps in v0

- Block grouping isn't tracked — `if cond / body / else / body` isn't a single node, just a sequence of leaves. Folding and structural text objects need v1's external scanner.
- Match arms and tagged-union destructuring aren't a dedicated rule yet.
- String interpolation is treated as a single string token; the inner `{expr}` parts aren't separately parsed.
- The Pratt-style binary operator precedence is approximate — fine for highlighting, not for accurate AST shape.
