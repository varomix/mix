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

- **Type inference** — types deduced from context, explicit annotations optional
- **Shapes** — struct-like types with methods, computed fields, and operator overloading
- **Tagged unions** — sum types with exhaustive `match`
- **Generics** — `@T` type parameters with `has` constraints
- **Optionals** — `T?` types with `else` fallback
- **Error handling** — result-based errors with `fail`, `else` fallback, and `?` propagation
- **String interpolation** — `"Hello {name}!"`
- **String operations** — `+` concatenation, `.ends_with()`, `.char_at()`, `to_string()`
- **Lists** — `push`, `pop`, `sort`, `reverse`, `insert`, `remove`, `contains`, `index_of`, `join`
- **Maps** — `{"key": val}`, `.has()`, `.remove!()`, `.keys`, `.values`
- **Slices & comprehensions** — `list[1..3]`, `[x*x for x in list]`
- **Closures / lambdas** — `x => x * 2`
- **Concurrency** — `go`/`wait` with `shared` for thread-safe values
- **C interop** — `extern "lib"` blocks, auto-binding from C headers (`--bind`)
- **Zones** — deterministic region-based memory
- **Modules** — `use path.to.module`
- **File I/O** — `file_read_all`, `file_write_all`, `file_open`/`file_write`/`file_close`
- **OS builtins** — `shell`, `file_exists`, `getcwd`, `mkdir`, `env`, `args`, `exit`
- **Build system** — `build.mix` with built-in `Project` shape for declarative builds
- **Math** — `sqrt`, `sin`, `cos`, `pow`, `abs`, `floor`, `ceil`, `round`, `min`, `max`
- **Conditional compilation** — `@os == "macos"`, `@debug`
- **Debug info** — DWARF line numbers for `lldb` source-level debugging
- **LSP** — language server with diagnostics, hover, and go-to-definition

## Compiler Options

```
mix [command] [options] [file.mix]

Commands:
  build [file.mix]   Compile to binary (default)
  run [file.mix]     Compile and execute

  If no file given with 'build', looks for build.mix first.

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
make test    # 44 tests
```

## Documentation

See `docs/DOCS.md` for the full language reference and `MIX-spec_v01.md` for the language specification.

## Building the LSP

```bash
make        # builds both mix and mix-lsp
```
