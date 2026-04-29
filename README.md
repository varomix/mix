# MIX Language

MIX is a compiled, statically typed language that emphasizes clarity and safety. It compiles to native code via QBE.

## Quick Start

```bash
# Build the compiler
make

# Compile and run a program
./build/mix examples/hello.mix
./hello
```

## Features

### Language
- **Type inference** — types deduced from context, explicit annotations optional
- **Shapes** — value types with methods, computed fields, and operator overloading
- **Borrows & boxes** — `ref T`, `ref! T`, and `Box[T]` for explicit aliasing and stable-address zone allocation
- **Tagged unions** — sum types with exhaustive `match` (warns on unhandled variants)
- **Unions** — C-style untagged unions for transparent C interop
- **Generics** — `@T` type parameters with `has` constraints (operators + shape methods enforced at call sites)
- **Generic shapes** — `shape Pair[T]`, `Stack[T]`, etc., monomorphized per concrete `T` (`Stack[int]`, `Stack[float]` are distinct)
- **Optionals** — `T?` types with `else` fallback, `?` propagation, and `match opt; some(v) =>; none =>` arms
- **Error handling** — `fail expr`, `else` fallback, `?` propagation, `match res; ok(v) =>; err(e) =>` arms
- **Match exhaustiveness** — warns on missing variants/arms for tagged unions, optionals, and results
- **String interpolation** — `"Hello {name}!"`
- **String operations** — `+`, comparison, `.sort()`, `.char_at()`, `.code()`, `ord()`, `chr()`
- **Lists** — `push!`, `pop!`, `sort!` (int/float/string), `reverse!`, `insert!`, `remove!`, `contains`, `index_of`, `join`, `at`, `at_mut!`, `for item! in list`
- **Maps** — `{"key": val}`, `.has()`, `.remove!()`, `.keys`, `.values`
- **Sets** — `set{"a", "b"}`, `.add!()`, `.remove!()`, `.has()`, `.union()`, `.intersect()`, `.diff()`
- **Slices & comprehensions** — `list[1..3]`, `[x*x for x in list]`
- **Closures / lambdas** — `x => x * 2`
- **Concurrency** — `go`/`wait` with `shared` for thread-safe values
- **C interop** — `extern "lib"` blocks, auto-binding from C headers (`--bind`), `use c "header.h"` for transparent FFI
- **Zones** — anonymous scoped zones, explicit `Zone` handles, and allocator-backed `List[T].new(zone)` / `Map[K, V].new(zone)` / `Set[T].new(zone)`
- **Modules** — `use path.to.module`, selective imports `use std.math: PI, hypot`, `pub` exports
- **Conditional compilation** — `@os == "macos"`, `@debug`, `@release`

### Standard library (`use std.X`)
- **`std.io`** — `read_file`, `write_file`, `read_lines`, `exists`, `current_dir`
- **`std.fmt`** — `hex`, `bin`, `pad_left`, `pad_right`, `truncate`
- **`std.path`** — `join`, `dirname`, `basename`, `extension`, `is_absolute`
- **`std.random`** — `seed`, `next_int`, `next_float`, `range`
- **`std.time`** — `now_ms`, `elapsed`, `Stopwatch`
- **`std.math`** — `PI`, `E`, `TAU`, `deg_to_rad`, `rad_to_deg`, `hypot`, `sign`, `fmod`
- **`std.string`** — `is_empty`, `capitalize`, `reverse`, `count`, `pad_left/right`
- **`std.collections`** — `Stack[T]`, `Queue[T]` (generic)

### Tooling
- **Two backends** — QBE (default native) and C (`--backend c`, useful on platforms without QBE)
- **`mix fmt`** — token-based formatter with comment preservation; supports `--check`, `--diff`, `-w`, recursive directory walk
- **LSP server** (`mix-lsp`) — diagnostics, hover, go-to-definition, find references, document/workspace symbols, rename, inlay hints, code actions, signature help, completion
- **Error messages** — colored output, line gutters, "did you mean?" suggestions, error limit
- **Debug info** — DWARF line numbers for `lldb` source-level debugging
- **Build system** — `build.mix` with built-in `Project` shape for declarative builds
- **Editor support** — Neovim plugin (`editors/nvim/`), VSCode extension (`editors/vscode-mix/`), tree-sitter grammar (`editors/tree-sitter-mix/`, v0)

## Compiler Options

```
mix [command] [options] [file.mix]

Commands:
  build [file.mix]   Compile to binary
  run [file.mix]     Compile and execute
  fmt [path...]      Format source files or directories
                     (-w writes in place, --check exits 1 on diffs,
                      --diff prints a unified diff)

  Running 'mix' with no command or file shows this help.
  'build' or 'run' without a file auto-discovers main().

Options:
  -o <file>        Output binary (default: derived from input)
  --emit-ir        Output QBE IR only
  --emit-tokens    Print token stream
  --emit-ast       Print AST
  --debug          Enable DWARF debug info
  --bind <path>    Generate .mix bindings from C headers
  --lib <name>     Library name for --bind
  -l<lib>          Link library (passed to cc)
  -v               Verbose
  --backend <name> Code backend (qbe or c)
```

## Build System

Create a `build.mix` in your project root and run `mix build`:

```mix
main() ~
    app = Project(
        name: "game",
        entry: "src/game.mix",
        output: "build/game",
        libs: ["SDL3", "m"],
        lib_paths: ["/opt/homebrew/lib"]
    )
    app.build()
```

```bash
mix build    # finds build.mix, compiles and runs it
```

## Error Handling

Functions with `~`, a non-void return type, and `fail` in the body automatically return results:

```mix
divide(a: float, b: float) -> float ~
    if b == 0.0
        fail "division by zero"
    done a / b

main()
    result = divide(10.0, 3.0) else 0.0    // 3.33333
    safe = divide(10.0, 0.0) else -1.0     // -1 (fallback)
```

Use `?` to propagate errors through call chains:

```mix
double_parsed(x: int) -> int ~
    val = parse_positive(x)?    // propagates error if failed
    done val * 2
```

## Memory Model

MIX is value-first:

- `shape` values copy/pass/return like structs
- `ref T` / `ref! T` are explicit non-owning borrows
- `Box[T]` is explicit stable-address storage allocated in a `Zone`
- `zone` blocks and explicit `Zone` handles group dynamic allocations by lifetime
- `List[T]`, `Map[K, V]`, and `Set[T]` can be allocated explicitly with `*.new(zone)`
- `items[i]` reads a value; `items.at(i)` / `items.at_mut!(i)` borrow without copying
- `for item in items` is read-only; `for item! in items` writes through to the element

Typical examples:

```mix
shape Vec2
    x, y: float

main() ~
    frame = zone_create("frame", 1024 * 1024)

    p = Vec2(x: 1.0, y: 2.0)          // plain value
    pts! = List[Vec2].new(frame)      // zone-backed dynamic storage
    pts.push!(p)

    slot = pts.at_mut!(0)             // mutable borrow
    slot.x = 9.0

    boxed = box(frame, p)             // stable-address zone allocation
```

## Effect Markers

MIX uses markers on function declarations to signal intent:

- `~` — function has side effects (I/O, mutation of external state)
- `!` — function mutates its receiver or arguments

```
greet(name: str) ~
    print("Hello {name}!")

counter.increment!()
    self.value! += 1
```

## Examples

See the `examples/` directory for complete programs including shapes with methods, tagged unions, operator overloading, string interpolation, concurrency, and vec2 math.

## Testing

```bash
make test-all    # runtime + error + error-message suites (run for current count)
```

## Documentation

See `docs/DOCS.md` for the full language reference, `MIX-spec_v01.md` for the language specification, `ZONES_VALUE_FIRST_DESIGN_SKETCH.md` for the memory-model rationale, and `LLVM_MIGRATION_PLAN.md` for the backend migration roadmap.

## Building the LSP

```bash
make        # builds both mix and mix-lsp
```
