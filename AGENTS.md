# MIX Project Knowledge

## Build
```sh
make                     # rebuild the compiler (binary at build/mix)
./build/mix build <file> -o <out>   # compile a .mix file
./build/mix build <file> -o <out> -v   # verbose (show clang invocations)
```

## Architecture
```
src/main.c      — entrypoint, CLI, module resolution, linker orchestration
src/lexer.c     — tokenizer
src/parser.c    — AST parser
src/sema.c      — semantic analysis / type checking
src/lower.c     — lowering to LIR (intermediate representation)
src/llvm_emit.c — LLVM IR backend (PRIMARY backend, default)
src/c_emit.c    — C backend (has pre-existing shape-return bug)
src/lir.c       — LIR definitions / helpers
src/cbind.c     — C header binding generation (`use c "..."`)
src/fmt.c       — formatter
src/arena.c     — arena allocator
lib/runtime.c   — runtime support (linked into every binary)
```

## Critical Type Rules
- `float` = 64-bit `double` (maps to C `double`).
- `float32` = 32-bit `float` (maps to C `float`).
- `float32` values **cannot** be used directly in string interpolation (`"...{f32}..."`) with the LLVM backend — convert with `to_float(f32)` first.
- `print(float32)` works implicitly (runtime promotes to double).

## Pointer Safety
- `&param` for by-value function parameters creates dangling pointers (double indirection in generated C). **Always use mutable locals instead:**
  ```mix
  foo(v: vec3)
      v! = vec3(x: v.x, y: v.y, z: v.z)   // mutable copy
      some_extern(&v, ...)                  // safe
  ```

## Module System
- `use name` resolves to `<base_dir>/name.mix`, then `<base_dir>/lib/name/name.mix`, then `<exe_dir>/../lib/name/name.mix`.
- `std.*` modules (e.g. `std.math`) are special-cased to `<exe_dir>/../lib/std/<rest>.mix`.
- Module source files are compiled to object files; `use` in a main file triggers module compilation automatically.
- Module source files use `pub` to export shapes and functions.

## Extern / Linking
- `extern "libname"` auto-generates `-llibname` linker flag (unless libname is `"C"`).
- `use c "header.h" link "libname"` generates `-llibname` specifically for that C import.
- Vendor libraries under `lib/vendor/<name>/lib/` get `-L` flags auto-added.
- Vendor headers under `lib/vendor/<name>/include/` get `-I` flags auto-added (for `use c "..."`).
- Extern functions must use `*` pointers for out-parameters or struct-by-reference.
- Extern functions with no return value use `~` (e.g., `glmc_vec3_add(a: *vec3, b: *vec3, dest: *vec3) ~`).

## Vendored Dependencies
```
lib/vendor/cglm/include/cglm/   — cglm 0.9.6 headers
lib/vendor/cglm/lib/libcglm.a   — precompiled static library
```

## Test Suite
- `make test` — runs all tests under `tests/programs/`.
- Tests fail on macOS because `timeout` command is not available (Linux-only).
- Individual tests can be run manually: `./build/mix build tests/programs/004_functions.mix -o /tmp/t && /tmp/t`.

## CGLM Wrapper (`lib/cglm/cglm.mix`)
- Links against `libcglm.a` via `extern "cglm"`.
- Uses `glmc_*` functions (the "call" API — linkable symbols, not inline).
- Shapes `vec3` and `mat4` use `float32` fields to match C layout.
- Methods: `+`, `-`, `*` (scalar), `dot`, `cross`, `length`, `length2`, `normalize`.
- Free functions: `mat4_identity`, `perspective`, `lookat`, `ortho`, `rad`.

## LLVM vs C Backend
- **LLVM** (default): works correctly for all features including shape-returning functions.
- **C**: has a pre-existing bug with shape-returning functions (forward declaration type mismatch). Not recommended for new code.
- Backend selection is implicit (LLVM is the only fully-featured backend).

## Common Gotchas
- `.mix` files use indentation-based blocks (like Python).
- Shape fields are accessed with `.` (e.g., `v.x`).
- Mutable locals use `!` suffix (e.g., `v! = vec3(...)`).
- `done` returns a value from a function.
- No `return` statement — use `done` in expression position.
- String interpolation uses `"hello {name}"`.
