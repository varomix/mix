![MIX](docs/name.jpg)

# MIX

Fast to run, fast to write, fast to read. Compiles to native code via LLVM, with C and WebAssembly backends too.

```bash
make                     # build the compiler
./build/mix run hello    # compile & run in one step
```

## Why MIX?

MIX is for writing programs that are easy to read, reason about, and run fast. It combines the ergonomics of a modern language with the performance of native compilation.

```rust
fibonacci(n: int) -> int
    if n <= 1
        done n
    fibonacci(n - 1) + fibonacci(n - 2)

main()
    for i in 0..10
        print(fibonacci(i))
```

### Values, not pointers

Types are values by default — they copy, pass, and return like structs. When you need aliasing or stable addresses, reach for `ref` or `Box` explicitly.

```rust
shape Vec2
    x, y: float

main()
    p = Vec2(x: 1.0, y: 2.0)

    // List backed by a zone allocator
    pts! = List[Vec2].new(zone_create("frame", 1024 * 1024))
    pts.push!(p)

    // Mutable borrow through the list
    slot = pts.at_mut!(0)
    slot.x = 9.0
```

### Errors you can't ignore

Functions with side effects use `~`. Functions that can fail return results — `else` provides fallbacks, `?` propagates errors up the call chain. The compiler won't let you forget.

```rust
divide(a: float, b: float) -> float ~
    if b == 0.0
        fail "division by zero"
    done a / b

main()
    result = divide(10.0, 3.0) else 0.0
    safe = divide(10.0, 0.0) else -1.0
```

### Concurrency without the boilerplate

```rust
main()
    t1 = go double(10)
    t2 = go double(20)
    r1 = wait t1
    r2 = wait t2

    counter = shared int(0)
    counter.update!(x => x + 1)
    print("Counter: {counter.read()}")
```

## Features at a glance

**Language** — type inference, shapes with methods, tagged unions, generics with `has` constraints, optionals (`T?`), pattern matching with exhaustiveness checking, string interpolation, closures, slices and comprehensions, modules, conditional compilation

**Collections** — `List` (dynamic arrays), `Map` (key-value), `Set` (unique elements), all with built-in methods like `push!`, `sort!`, `contains`, `join`, `keys`, `values`, and more

**Memory** — value semantics by default, explicit `ref`/`Box` borrows, zone-based allocation for grouping lifetimes

**Interop** — `extern` blocks for C/Objective-C, auto-generate MIX bindings from C headers with `--bind`, transparent `use c "header.h"` FFI

**Tooling** — LSP server with diagnostics, hover, go-to-definition, completions, and more; `mix fmt` token-based formatter; Neovim, VSCode, and Zed extensions; tree-sitter grammar

**Backends** — LLVM (native), C (`--backend c`), WASM (`--target wasm32`), browser WebAssembly (`--target wasm-browser`)

## Quick reference

```
mix build [file]      Compile to binary
mix run [file]        Compile and execute
mix fmt [path...]     Format source (-w writes in place)
mix --bind <header>   Generate bindings from C headers

Options: -o <file>, --emit-ir, --debug, -O<level>,
         -l<lib>, -v, --backend <llvm|c>, --target <native|wasm32|wasm-browser>
```

## Get started

See the `examples/` directory for programs covering shapes, tagged unions, concurrency, operator overloading, SDL3, raylib, and WebGPU graphics.

Full language reference: [`docs/DOCS.md`](docs/DOCS.md)
Language specification: [`docs/MIX-spec_v01.md`](docs/MIX-spec_v01.md)
Memory model design: [`docs/ZONES_VALUE_FIRST_DESIGN_SKETCH.md`](docs/ZONES_VALUE_FIRST_DESIGN_SKETCH.md)

## Build from source

```bash
make           # builds mix + mix-lsp
make test-all  # runtime + error test suites
```
