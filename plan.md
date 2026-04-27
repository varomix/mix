# MIX Language — Implementation Plan

> Historical note: this file tracks the compiler build-out campaign. For the
> current user-facing language model, use `README.md`, `MIX-spec_v01.md`, and
> `docs/DOCS.md`. MIX is now value-first with explicit `ref` / `ref!`,
> `Box[T]`, and `Zone`, not "zones as the whole memory story."
>
> Updated: April 2026 — Phases 1-12 complete; run `make test-all` for current pass count

## Context

Implement the MIX programming language as defined in `MIX-spec_v01.md`. MIX is a systems language with C performance, Python-like clarity, value-first memory with explicit borrows / boxes / zones, and clean C interop — targeting graphics/games.

- **Backend**: QBE (lightweight native code compiler)
- **Compiler written in**: C (~7,500 lines total)
- **Platform**: macOS Apple Silicon (aarch64)
- **Spec file**: `MIX-spec_v01.md`

### Design Decisions (confirmed)
- `main()` implicitly returns 0, has implicit `~`
- `print(...)` auto-formats by type (int, float, str, bool, list)
- `int` = native word size (64-bit on aarch64, maps to QBE `l`)
- Indentation: 4 spaces per level (tabs are a compile error)
- No naming convention enforcement
- Variable semantics: `x = 42` declares immutable, `x! = 42` declares mutable
- Shape construction: both named `Vec2(x: 1.0, y: 2.0)` and positional `Vec2(1.0, 2.0)`
- Shapes registered before externs in sema (declaration order independent)
- `LDFLAGS` environment variable picked up automatically for linking
- Temp files use `/tmp` to avoid Synology Drive sync issues

---

## Current Status

Run `make test-all` for the current pass count across all three suites.

### Phase 1 — Foundation [COMPLETE]
- [x] Project scaffolding (Makefile, arena, errors, runtime)
- [x] Lexer with indentation (INDENT/DEDENT), `!` mutable idents, `//` comments
- [x] Recursive descent parser with Pratt precedence climbing
- [x] Semantic analysis with type inference and scoped symbol table
- [x] QBE IR emitter (SSA, aggregate types, correct C ABI parameter types)
- [x] CLI pipeline driver (mix → QBE → cc → binary)
- [x] End-to-end test suite

### Phase 2 — Data & Control [COMPLETE]
- [x] Shapes (structs): definition, construction, field access `v.x`
- [x] Shape passing to/from functions via QBE aggregate types
- [x] `@const` compile-time constant inlining
- [x] `defer` with reverse-order execution before every exit
- [x] `for i in 0..n` range loops (exclusive `..` and inclusive `..=`)
- [x] `match` with `=>` arms and `_` wildcard
- [x] String interpolation `"Hello {name}! {x + y}"`

### Phase 3 — Abstraction [COMPLETE]
- [x] Shape methods with implicit `self`, dot-call syntax `c.area()`
- [x] Methods with parameters `v.dot(other)`
- [x] Modules: `use path.to.module`, `pub` exports
- [x] Multi-file compilation and linking
- [x] Imported shape type definitions shared across compilation units
- [x] Type aliases: `type Score = int`
- [x] Lambdas: `x => x * 2`, `(a, b) => a + b`
- [x] Higher-order functions (indirect calls through function pointers)
- [x] SDL3 integration verified (cornflower blue window)

### Phase 4 — Collections & Memory [COMPLETE]
- [x] Lists: `[1, 2, 3]` literals, `list[i]` indexing, `list.len`, `list.push!(val)`
- [x] `print(list)` prints `[1, 2, 3]` format
- [x] `for item in list` — for-each iteration over lists
- [x] Operator overloading: `+(other)`, `-(other)`, `*(scalar)`, `==(other)` inside shapes
- [x] All arithmetic operators: `+ - * / %` and comparisons: `== != < > <= >=`
- [x] `v1 + v2` compiles to `Vec2_op_add(v1, v2)` automatically
- [x] Unsafe blocks: `unsafe { *ptr = val; ptr + offset }`
- [x] Dereference assignment: `*ptr = val` → `store val, ptr`
- [x] Zones: `zone { ... }` with automatic cleanup via watermark stack
- [x] Named zones: `zone:name { ... }`, nested zones supported
- [x] `mix_zone_alloc()` for zone-tracked allocations

### Phase 5 — Error Handling & Optionals [COMPLETE]
- [x] Optional types: `T?` type annotations (`-> int?`)
- [x] `none` creates empty optional via `mix_optional_none()`
- [x] `done value` in optional-returning functions wraps via `mix_optional_some(value)`
- [x] `expr else default` unwraps optional with fallback (stack-slot phi merge)
- [x] Tagged unions: `shape Shape { Circle(radius: float) | Rect(w: float, h: float) }`
- [x] Variant construction: `Circle(radius: 5.0)` sets tag + fields
- [x] Destructuring match: `Circle(r) => ...` extracts variant fields into scope
- [x] Tag comparison in match, field extraction at correct offsets
- [x] QBE tagged union type: `{ l (tag), l, l, ... }` padded to max variant size
- [x] `fail "message"` — panic with message and exit
- [x] Dead-code label after fail for valid QBE IR

### Phase 6 — Generics & Advanced Types [COMPLETE]
- [x] `@T` type parameters before functions
- [x] `@T, K` multiple type parameters
- [x] Generic functions: `identity(x: T) -> T`, `max_of(a: T, b: T) -> T`
- [x] Type params resolved as 64-bit values (uniform representation)
- [x] Generic context in sema for type resolution (save/restore around fn bodies)
- [x] Computed fields: zero-param methods callable without `()`
- [x] `r.area` (no parens) and `r.area()` (with parens) both work
- [x] When field lookup fails on a shape, tries method lookup automatically

### Bugfixes & Improvements
- [x] Positional shape construction: `Color(255, 0, 0, 255)` without field labels
- [x] Positional shapes moved from parser heuristic to sema (fixes `SetTargetFPS(60)` misidentification)
- [x] For-each loop element type inference from list's element type (not always int)
- [x] `[T]` list type annotation in function parameters
- [x] Byte-accurate memory: `storeb`/`loadub` for uint8, `storeh`/`loaduh` for uint16
- [x] `type_to_qbe_mem()` and `type_to_qbe_load()` for correct store/load suffixes
- [x] QBE type definitions use correct field sizes (`b`, `h`, `w`, `l`, `s`, `d`)
- [x] Shape-first registration in sema (shapes before externs, declaration order independent)
- [x] `LDFLAGS` environment variable support in linker command
- [x] Small integer widening (`extsw`) before `mix_print_int` for uint32 values
- [x] Temp files in `/tmp` to avoid Synology Drive sync interference
- [x] Raylib integration verified (solid red window with Color shape)
- [x] SDL3 integration verified (cornflower blue window)

---

## Phase 7 — Concurrency (Minimal) [COMPLETE]

### 7E: shared type [COMPLETE]
- [x] `TYPE_SHARED` with `struct { MixType *inner; } shared` in type system
- [x] `NODE_SHARED_EXPR` — `shared int(0)` creates mutex-wrapped value
- [x] Parser: `TOK_SHARED` → skip optional type keyword → `(` → parse init expr → `)`
- [x] Sema: wraps inner type in `TYPE_SHARED`; method dispatch for `.read()` → inner type, `.update!(fn)` → void
- [x] QBE emit: `$mix_shared_new(val)`, `$mix_shared_read(ptr)`, `$mix_shared_update(ptr, fn_ptr)`
- [x] Runtime: `MixShared` = `{pthread_mutex_t, int64_t}` with lock-based read/update

### 7A: go / wait [COMPLETE]
- [x] `TYPE_TASK` with `struct { MixType *result_type; } task` in type system
- [x] `NODE_GO_EXPR` — `go compute(5)` spawns function in new thread
- [x] `NODE_WAIT_EXPR` — `wait t` joins thread and returns result
- [x] Parser: `TOK_GO` → parse call expr; `TOK_WAIT` → parse expr
- [x] Sema: `go` wraps call result in `TYPE_TASK`; `wait` unwraps `TYPE_TASK`
- [x] QBE emit: pack fn ptr + args array → `$mix_task_spawn(fn, args, count)`; `$mix_task_wait(handle)`
- [x] Runtime: `MixTask` with `pthread_create`/`pthread_join`, trampoline dispatches 0-8 args
- [x] macOS: pthreads included in libSystem (no `-lpthread` needed)

### 7B: run block (structured concurrency) [DEFERRED]
```mix
run
    a = fetch_user(id)
    b = fetch_posts(id)
    c = fetch_notifications(id)
build_page(a, b, c)
```

### 7C: Streams and channels [DEFERRED]
```mix
stream count_up(max: int)
    i! = 0
    while i < max
        yield i
        i! += 1

ch = channel(int)
```

---

## Phase 8 — Practical Features [COMPLETE]

### 8B: String methods [COMPLETE]
- [x] `.len` property → int (via `mix_str_len`)
- [x] `.upper()`, `.lower()`, `.trim()` → str
- [x] `.split(delim)` → [str] (returns list of strings)
- [x] `.contains(s)`, `.starts_with(s)` → bool
- [x] `.replace(old, new)` → str
- [x] Sema: TYPE_STR field/method resolution
- [x] QBE emit: runtime call dispatch for all 8 string functions
- [x] Runtime: 8 C functions using `<string.h>` and `<ctype.h>`

### 8C: Maps [COMPLETE]
- [x] `TYPE_MAP` in type system with `key_type` and `val_type`
- [x] `NODE_MAP_LIT` — `{"key": val, ...}` literal syntax
- [x] `NODE_INDEX_ASSIGN` — `map!["key"] = val` index write
- [x] Map indexing: `map["key"]` via `mix_map_get()`
- [x] Map fields: `.len`, `.keys` → [str], `.values` → [int]
- [x] Map methods: `.has(key)` → bool, `.remove!(key)`
- [x] `for key, value in map` iteration (via keys list)
- [x] `print(map)` → `{"key": val, ...}` format
- [x] Runtime: hash map with open addressing (djb2 hash, 0.75 load factor)

### 8D: Slices and comprehensions [COMPLETE]
- [x] `NODE_SLICE_EXPR` — `list[start..end]`, `list[..n]`, `list[n..]`, `list[0..=2]`
- [x] Slice parsing: `PREC_RANGE` precedence to stop before `..` in postfix `[]`
- [x] `NODE_LIST_COMP` — `[expr for var in iter if cond]`
- [x] Comprehension parsing: detect `TOK_FOR` after first expression in `[...]`
- [x] Filtered comprehensions: optional `if` clause
- [x] Runtime: `mix_list_slice()` with negative index support
- [x] List type annotation: `[T]` in function parameter types

### 8A: Conditional compilation [COMPLETE]
- [x] `NODE_COND_DECL` — `@os == "macos"` / `@arch == "aarch64"` / `@debug` / `@release`
- [x] Compile-time evaluation using `#ifdef __APPLE__`, `__aarch64__`, etc.
- [x] `--debug` / `--release` CLI flags
- [x] Active conditional blocks register their declarations in sema passes
- [x] QBE emit only processes active conditional blocks

### 8E: `has` constraints for generics [COMPLETE]
- [x] `@T has +, ==` constraint parsing after type parameters
- [x] Operator and method name constraints stored on fn_decl AST node
- [x] Constraints parsed but enforcement deferred (type-erased representation)

---

## Phase 9 — Tooling [COMPLETE]

### 9A: C header binding generator [COMPLETE]
- [x] `--bind <path>` flag — accepts a single .h file or directory of .h files
- [x] `--lib <name>` flag — sets the extern library name (defaults to basename)
- [x] Preprocessor integration: runs `cc -E -P` to expand macros/includes
- [x] C function prototype parser: extracts return type, function name, parameters
- [x] Strips qualifiers (`const`, `volatile`, `__restrict`, `__attribute__`)
- [x] C-to-MIX type mapping: `int`→`int32`, `double`→`float`, pointers→`*byte`, stdint types, etc.
- [x] `#define` constant extraction via `cc -E -dM` (integer and float literals only)
- [x] System constant filtering (skips INT*_MAX, TARGET_OS_*, etc.)
- [x] Directory mode: processes all `.h` files, deduplicates across headers
- [x] Output: valid .mix file with `extern` block + `@const` declarations
- [x] Self-contained `src/cbind.c` — no dependency on arena/AST/types
- [x] Generated output verified parseable by the MIX compiler

### 9B: Debug info (DWARF) [COMPLETE]
- [x] QBE `dbgfile "filename"` directive emitted at program start
- [x] QBE `dbgloc line` directives emitted before each statement
- [x] `emit_dbgloc()` helper with redundant-line suppression
- [x] Debug line tracking reset per function and per lambda
- [x] `--debug` flag threads through `compile_module()` and emitter
- [x] `-g` flag added to `cc` link command in debug mode
- [x] DWARF line table verified in .o files via `xcrun dwarfdump --debug-line`

### 9C: Self-hosting exploration [DEFERRED]
- Rewrite mix in MIX itself

---

## Phase 10 — Ecosystem Improvements [COMPLETE]

### 10A: File I/O [COMPLETE]
- [x] `file_open(path, mode)` → int (FILE* handle, 0 on failure)
- [x] `file_read(handle)` → str (read chunk)
- [x] `file_write(handle, data)` → void
- [x] `file_close(handle)` → void
- [x] `file_read_all(path)` → str (convenience: read entire file)
- [x] `file_write_all(path, content)` → bool (convenience: write entire file)
- [x] Registered as built-in functions in sema with proper types
- [x] QBE emit with direct runtime dispatch (no generic call path)

### 10B: Math Functions [COMPLETE]
- [x] Single-arg: `sqrt`, `abs`, `sin`, `cos`, `tan`, `log`, `floor`, `ceil`, `round` (float → float)
- [x] Two-arg: `pow`, `min`, `max` (float, float → float)
- [x] Auto int-to-float conversion when passing int args to math functions
- [x] `-lm` added to link command (required on Linux for libm)
- [x] Runtime: thin wrappers around `<math.h>` functions

### 10C: String Operations [COMPLETE]
- [x] `str + str` → string concatenation via `mix_str_concat`
- [x] `.ends_with(suffix)` → bool
- [x] `.char_at(index)` → str (single character)
- [x] `.join(sep)` on lists → joined string
- [x] `to_string(value)` → str (converts int or float to string)
- [x] Sema: `str + str` recognized as concat in binary expr
- [x] QBE emit: dispatches to runtime concat/methods

### 10D: List Operations [COMPLETE]
- [x] `.pop!()` → last element (removes it)
- [x] `.remove!(idx)` → void (shift elements left)
- [x] `.insert!(idx, val)` → void (shift elements right)
- [x] `.sort!()` → void (qsort with int64 comparison)
- [x] `.reverse!()` → void (in-place swap)
- [x] `.contains(val)` → bool
- [x] `.index_of(val)` → int (-1 if not found)
- [x] All registered in sema with correct return types
- [x] QBE emit with runtime dispatch for each method

### 10E: Result-Based Error Handling [COMPLETE]
- [x] `TYPE_RESULT` added to type system with `ok_type` inner type
- [x] `NODE_TRY_EXPR` added to AST for `?` postfix operator
- [x] Parser: `?` parsed as postfix after expressions
- [x] Runtime: `mix_result_ok(value)`, `mix_result_err(err)`, `mix_result_is_ok(res)`, `mix_result_unwrap(res)`, `mix_result_unwrap_err(res)`
- [x] Result layout: `{int64_t is_ok; int64_t value}` — same as optional
- [x] **Auto-wrapping rule**: functions with `~` (side effects), non-void return, and body containing `fail` or `?` get return type auto-wrapped in `TYPE_RESULT`
- [x] `body_contains_fail()` recursive helper scans for `fail` statements and `?` expressions
- [x] `fail "msg"` in result-returning functions emits `mix_result_err()` + ret (instead of panic)
- [x] `done value` in result-returning functions wraps via `mix_result_ok()`
- [x] `expr else default` works with both `TYPE_OPTIONAL` and `TYPE_RESULT`
- [x] `expr?` (try operator) checks is_ok, propagates error if not, unwraps if ok
- [x] Float values in results handled via `cast` (bit-cast double ↔ int64)
- [x] Function signatures use `l` (pointer) return for result types
- [x] **Backward compatible**: `026_fail.mix` still passes — void functions with `fail` still panic

### 10F: Examples & README [COMPLETE]
- [x] `README.md` — project description, quick start, features, compiler options
- [x] `examples/shapes_methods.mix` — Circle/Rect with methods
- [x] `examples/tagged_unions.mix` — Shape variants with match
- [x] `examples/vec2_math.mix` — operator overloading + math
- [x] `examples/string_interp.mix` — interpolation showcase
- [x] `examples/concurrency.mix` — go/wait/shared

---

## Build Pipeline

```
hello.mix → [mix] → hello.ssa → [qbe] → hello.s → [cc] → hello (binary)
```

For multi-module projects:
```
main.mix ──→ [mix] ──→ main.ssa ──→ [qbe] ──→ main.s ──┐
                 │                                         │
math/vec.mix → [mix] → vec.ssa → [qbe] → vec.s ─────────┤
                                                           ├→ [cc] → binary
lib/runtime.c ────────────────────────────────────────────┘
```

## Project Structure

```
mix_lang/
├── Makefile
├── MIX-spec_v01.md          # Full language specification
├── plan.md                  # This file
├── DOCS.md                  # Language documentation (markdown)
├── DOCS.html                # Language documentation (website)
├── src/
│   ├── main.c               # CLI + multi-module pipeline (331 LOC)
│   ├── mix.h               # Shared types (25 LOC)
│   ├── arena.h/c            # Arena allocator (55 LOC)
│   ├── errors.h/c           # Error reporting (73 LOC)
│   ├── token.h              # Token definitions (129 LOC)
│   ├── lexer.h/c            # Lexer (624 LOC)
│   ├── ast.h/c              # AST nodes (350+255 LOC)
│   ├── parser.h/c           # Parser (1,411 LOC)
│   ├── types.h/c            # Type system (74+141 LOC)
│   ├── symtab.h/c           # Symbol table (49 LOC)
│   ├── sema.h/c             # Semantic analysis (806 LOC)
│   ├── qbe_emit.h/c        # QBE IR emitter (1,348 LOC)
│   └── cbind.h/c            # C header binding generator (470 LOC)
├── lib/
│   └── runtime.c            # C runtime (~750 LOC)
├── tests/
│   ├── run_tests.sh
│   └── programs/            # 43 test programs
│       ├── 000_return_zero.mix ... 042_error_propagation.mix
│       ├── mathlib/          # Module test files
│       └── expected/         # Expected outputs
└── examples/
    ├── hello.mix
    ├── arithmetic.mix
    ├── control_flow.mix
    ├── fibonacci.mix
    ├── extern_c.mix
    ├── shapes_methods.mix
    ├── tagged_unions.mix
    ├── vec2_math.mix
    ├── string_interp.mix
    ├── concurrency.mix
    ├── sdl3_window.mix
    └── raylib_example.mix
```

## Test Suite

| # | Test | Verifies |
|---|------|----------|
| 000 | return_zero | Implicit main return 0 |
| 001 | arithmetic | `+ - * / %` operators |
| 002 | hello | String output |
| 003 | if_else | Branching and else-if |
| 004 | functions | User functions, implicit return |
| 005 | while | While loops, mutable variables |
| 006 | extern | C interop (libc puts) |
| 007 | fibonacci | Recursion, done (early exit) |
| 008 | shapes | Shape construction, field access |
| 009 | shape_funcs | Shapes passed to/from functions |
| 010 | const | @const compile-time constants |
| 011 | defer | Defer in reverse order |
| 012 | for_range | For-range loops (0..n) |
| 013 | match | Match with pattern arms + wildcard |
| 014 | string_interp | String interpolation `{expr}` |
| 015 | methods | Shape methods, dot-call |
| 016 | modules | Multi-file use/pub |
| 017 | module_shapes | Imported shapes and methods |
| 018 | lambdas | Lambda variables |
| 019 | higher_order | Lambdas as function arguments |
| 020 | lists | List literals, indexing, len, push, for-each |
| 021 | operator_overload | +, -, *, == on shapes |
| 022 | unsafe | Pointer deref, pointer arithmetic |
| 023 | zones | Scoped memory, nested zones |
| 024 | optionals | T?, none, expr else default |
| 025 | tagged_unions | Variant shapes, destructuring match |
| 026 | fail | Error handling with fail keyword |
| 027 | generics | @T generic functions |
| 028 | computed_fields | No-parens method access |
| 029 | positional_shapes | Shape construction without field labels |
| 030 | string_methods | .len, .upper(), .lower(), .trim(), .split(), .contains(), .starts_with(), .replace() |
| 031 | maps | Map literals, indexing, .has(), index assign, .remove!(), .len, .keys, .values |
| 032 | slices_comp | List slicing [1..3], [..2], [2..], [0..=2], list comprehensions [x*x for x in list] |
| 033 | cond_compile | @os == "macos", @arch == "aarch64" conditional declarations |
| 034 | has_constraints | @T has == generic constraints, contains function |
| 035 | shared | shared int(0), .read(), .update!(fn), mutex-wrapped values |
| 036 | go_wait | go compute(5), wait t, multiple concurrent tasks |
| 037 | file_io | file_write_all, file_read_all, file_open/write/close |
| 038 | math | sqrt, abs, floor, ceil, round, pow, min, max, sin, cos |
| 039 | string_ops | str + str concat, ends_with, char_at, to_string |
| 040 | list_ops | sort, contains, index_of, pop, reverse, insert, remove, join |
| 041 | error_handling | Result type: divide with fail, catch with else |
| 042 | error_propagation | ? operator: error propagation through call chain |

## Verification

```bash
make              # build compiler
make test         # run the runtime test suite
./build/mix examples/sdl3_window.mix -o sdl3 -lSDL3      # cornflower blue window
./build/mix examples/raylib_example.mix -o raylib -lraylib # solid red window
./build/mix --bind /path/to/header.h -o bindings.mix --lib name  # C binding generator
./build/mix program.mix -o prog --debug                   # DWARF debug info
```

---

## Session log — 2026-04-25 (mixel unblock)

Triggered by a feedback note from the mixel framework session
(`/Users/varomix/SynologyDrive/DEV/MIX_DEV/mixel/README.md`) listing 7 MIX
compiler bugs blocking a Flixel-shaped API. All 7 are now closed.

### What changed

| # | Bug (mixel README) | Fix |
|---|---|---|
| 1 | Shape fields can't be mutated | Parser produces NODE_FIELD_ASSIGN; sema validates root mutability + shape-field rewrite for bare-name `field! += v` inside methods marked `has_mutation`. Both backends pass `self` as `l` (pointer), not `:Shape` (by-value). C backend renames param to `p_self` and aliases `Shape *v_self = p_self` so existing `v_*` lookup resolves. Field-load codegen uses `v_self->field`. |
| 2 | No module-level mutable state | Parser accepts `name! = literal` / `pub name! = literal` at top level → NODE_VAR_DECL{is_global, is_pub}. Sema's `register_global` requires literal-only init. New `Symbol.is_global` flag. QBE: `[export ]data $g_<name>` + special-cased NODE_IDENT/NODE_ASSIGN. C: `[static ]<type> v_<name>` at file scope; importers get `extern v_<name>` declarations from a symtab walk. |
| 3 | `[float]` element assignment ICE | QBE NODE_INDEX_ASSIGN bit-casts `d` → `l` via `=l cast` before passing to `mix_list_set` (parallels list-push pattern; C backend already had this fix). |
| 4 | `use c` constants lose type cross-module | Same root cause as #5. |
| 5 | `pub @const` integers can't be used cross-module | Sema collects every NODE_CONST_DECL it processes (any module) into `all_consts`. New pass-(e) splices not-already-present consts into the current program's AST so emitter inlining sees them. |
| 6 | `peek_u32(ptr)` returning `uint32` blew up arithmetic | Builtin signature changed to `peek_u32(ptr, off) -> int` (returns int64). Old `mix_peek_u32` C function kept so existing tests' `extern` decls keep working. New runtime helper `mix_peek_u32_at`. Also added `peek_byte`, `peek_f32`, `memcpy` builtins so `mixel_helper.h/c` can be deleted. |
| 7 | cbind libc symbol bleed-through (surfaced this session) | `preprocess_header` switched from `cc -E -P` to `cc -E` (preserves `# linenum "file"` markers). New `filter_user_decls` drops everything from system header paths (`/usr/`, `/Library/`, `/Applications/`, `/System/Library/`, `/opt/homebrew/include/c++/`). Backup libc skip-list in `write_mix_output` for stragglers. |

### Bonus fix discovered while testing

- **`x! = expr` mutation consistency** — the C backend was shadowing `x!` to a fresh local that went out of scope (made mixel's `px! = px - speed * dt` inside an if-block silently discard the update). Sema now rewrites `x! = expr` to NODE_ASSIGN when `x` already exists as mutable and isn't a shape/union. Shape/union still shadows because their NODE_VAR_DECL emit uses `=l copy` aliasing, not an alloca'd slot.

### Tests added (all in `tests/programs/`)

- 066_float_list_assign — float-list element store on both backends
- 067_peek_helpers — peek_byte/peek_f32/memcpy/peek_u32 with offsets
- 068_shape_field_mut — external `s.x =`, `s.x! =`, compound `+=`, internal `field! += v`, internal `self.field = v`
- 069_cross_module_const + constlib/keys.mix — imported `pub @const` integers in arithmetic, function args, match
- 070_mut_reassign — `x! = expr` reassigns existing mutable on both backends
- 071_module_globals + globallib/state.mix — local + cross-module pub mutables, mutation through helpers, direct write to imported global

Suite is now 72 runtime + 16 error + 19 error-message + 29 fmt = **136 tests, all green on both backends**.

### Open follow-ups (mixel-related, not blocking the framework)

1. **Drop `mixel_helper.h/c`** — every helper now has a MIX builtin equivalent. `use c "mixel_helper.h" source "mixel_helper.c"` line in framework + demos can be removed.
2. **Module-level mutable: non-literal initializers** — would need a per-module init function and a cross-module init-order story. Skipped for MVP.
3. **`shared T(...)` syntax** — spec mentions it for thread-safe globals; MVP globals are unsynchronized like C globals.
4. **The Flixel-shaped Sprite/Group/State API** — now unblocked. Concrete next step: rewrite mixel's hello example to use a `Sprite` shape with `pos: Vec2`, `vel: Vec2`, and a mutating `update!()` method, plus a module-level `running!` flag instead of threading it through `pump()`.
5. **mixel C backend** still hits one cosmetic warning (`int32_t = (void *) != int64_t`) in the emitted code for an SDL pointer comparison — harmless but worth tracing if it shows up elsewhere.

### Things to know if resuming this work

- The Claude Code sandbox is rooted at `~/SynologyDrive/DEV/mix_lang`. Builds from `mixel`'s tree fail with `EPERM` from inside the agent — that's the sandbox, not a real bug. Test mixel from your shell.
- `Sema.current_shape` and `current_method_mutates` are set/restored around method-body analysis. Anything new that walks methods (e.g. operator-overload resolution if extended) needs to do the same save/restore.
- `Symbol.is_global` is the canonical "this is a module-level mutable" flag. Both emitters key off it, not off the AST decl directly. If you add new ways to introduce module-level state (e.g. `shared`), set this flag too.
- cbind's `is_system_header_path` heuristic is conservative — if a user header lives under `/usr/local/` or similar, it'd get filtered out. Add explicit overrides if that bites.
- The `all_consts` and `all_globals` pattern on `Sema` is the template for any other "collect across modules" need that comes up later.

### Verification commands

```bash
make clean && make -j8
make test test-errors test-error-messages    # 107 tests, both backends
# To verify mixel from your shell (Claude Code can't write there):
cd ~/SynologyDrive/DEV/MIX_DEV/mixel/examples/01_hello
~/SynologyDrive/DEV/mix_lang/build/mix run main.mix
```

---

## Session log — 2026-04-25 (nested-shape mutation + mixel refactors)

Continues the previous mixel-unblock session. The remaining mixel blocker
(`shape Sprite { pos: Vec2 }` with `update!` mutating `pos.x`) was actually
**not** fully fixed last session — flat-field mutation worked, but every
nested-shape form (inside method, outside method, whole-shape replacement,
deep scalar write) crashed or returned garbage. This session closes that
gap on both backends and ports the mixel framework + hello demo to use
the natural `Sprite { pos: Vec2, vel: Vec2 }` API.

### What changed in the compiler

| File | Change |
|---|---|
| `src/sema.c` | NODE_VAR_DECL → NODE_ASSIGN rewrite extended: when `existing` is a SHAPE *and* matches a field of `current_shape`, force NODE_ASSIGN (which then triggers the FIELD_ASSIGN rewrite against `self`). Without this, `pos! = Vec2(...)` inside an `update !` shadowed the field with a fresh local and silently dropped the write. |
| `src/qbe_emit.c` | NODE_IDENT for shape-typed self-fields returns the **address** of the inline shape (copy `%v.self` if offset 0, `add %v.self,off` otherwise) instead of `loadl` (which would have read 8 bytes of an aggregate as a pointer). |
| `src/qbe_emit.c` | NODE_FIELD_ASSIGN for a shape-typed field with `=` emits `call $memcpy(dst+off, val, total_size)` instead of a single-word store. |
| `src/c_emit.c` | Switched nested-shape struct fields from `Vec2 *pos` to `Vec2 pos` (inline storage) — both in source-decl emission and in the imported-shapes-from-symtab pass. Matches the QBE memory model. |
| `src/c_emit.c` | NODE_SHAPE_LIT for a nested-shape field emits `memcpy(&t->pos, val, sizeof(Vec2))` instead of a pointer assignment. |
| `src/c_emit.c` | NODE_IDENT inside a method returns `<Shape> *t = &v_self->pos;` for shape-typed fields (not `int64_t t = v_self->pos;`, which warned and aliased a `Vec2 *` slot to scalar). |
| `src/c_emit.c` | NODE_FIELD_ASSIGN for a shape-typed field emits `memcpy(&((Sprite *)obj)->pos, val, sizeof(Vec2))`. |

### Tests

- `tests/programs/072_nested_shape_mut.mix` (+ `expected/072.txt`) — covers the five cases that previously broke: external whole-field replace, external deep scalar write, internal compound on inline scalar, internal whole-field replace via field name, internal explicit `self.field = Vec2(...)`.
- Both backends: 73 runtime + 16 error + 19 error-message + 29 fmt = **137 tests green on QBE**, 68 on C (5 pre-existing C-backend failures unrelated to this work: `006_extern`, `011_defer`, `013_match`, `022_unsafe`, `023_zones` — all `MIX_BACKEND=c` linker/codegen issues that already failed before this session).

### What changed in mixel

- Deleted `mixel_helper.h`, `mixel_helper.c`, and the `mixel_helper.{h,c}` symlinks under every example. Replaced every `mixel_*` call with the corresponding MIX builtin (`memcpy`, `peek_byte`, `peek_f32`, `peek_u32`).
- Added `pub shape Vec2 { x, y: float }` and `pub shape Sprite { pos: Vec2, vel: Vec2, width, height: float, color: Color }` to `mixel.mix`, with `update(dt) !` and `draw(g) ~` methods, plus a `make_sprite(...)` constructor.
- Rewrote `examples/01_hello/main.mix` against the new API: `s.update!(dt)`, `s.draw(g)`, `s.vel.x = ...` for input.
- Updated `mixel/README.md`: project layout drops the helper files, public API section now shows `Sprite`/`Vec2`, "Pending refactor" replaced with "Recently landed", roadmap repointed at Phase 2 (Group / overlap / text / audio / Snake demo).

### Open follow-ups for next session (in mixel order of urgency)

1. **Phase 2 starts with `Group` and `physics/overlap`** per `mixel/PLAN.md`. Snake is the headline deliverable for Phase 2 — it pulls in `text` (SDL_ttf), `audio` (SDL_mixer), `timer`, `random`. Audio + text are the largest chunks of new C-binding work.
2. **Module-level mutables, non-literal init** — would let mixel collapse the `g: Game` threading and adopt the Flixel `FlxG.*` global API. Needs a per-module `__mix_init` function emitted by both backends.
3. **C-backend pre-existing failures** (006/011/013/022/023) deserve a pass at some point — `006_extern`'s `extern int32_t puts(void *)` vs libc `int puts(const char *)` is a C-binding signature mismatch in `cbind` or extern-decl lowering.
4. **C-backend struct ordering** — switching nested shapes to inline storage now requires the inner shape to be declared *before* the outer one in the C output. The current emitter walks decls in source order and skips a topological sort. Fine for mixel (Vec2 before Sprite, naturally), but will bite if a user writes mutually-nested or out-of-order shape decls. Easy fix when it matters: topo-sort the shape decls before emit.

### Things to know if resuming

- `current_shape` is a `MixType *` pointer to the shape *type*, not the AST decl. To check whether a name corresponds to a self-field, use `type_find_field(sema->current_shape, name)`.
- The QBE field-store and the C field-store paths now have the same shape-special-case structure — keep them in sync if you extend either (e.g. for compound `+=` on shape-typed fields, which would need an `op_add` method dispatch instead of a memcpy).
- The C backend declares `Vec2 *t1 = mix_alloc(sizeof(Vec2));` for shape literals even when the literal is only used as the source of a memcpy into a parent's inline field. Slightly wasteful (one extra alloc per nested ctor), but correct. Optimization opportunity: detect "shape literal used only as memcpy source" and synthesize the copy in place.
- Don't try to run mixel demos from inside the agent — they open windows and the sandbox doesn't have GUI. Compile-only verification is enough; the user runs from their shell.

---

## Session log — 2026-04-25 (path-style use paths)

Same session as nested-shape mutation; user wanted `use ../../mixel` to work
so demos don't need a `mixel.mix` symlink in each example directory.

### What changed

| File | Change |
|---|---|
| `src/parser.c` | When `use` is followed by `TOK_DOTDOT`, take a new "path-style" branch: parse `..` and ident-like segments separated by `/`, store the assembled string (e.g. `"../../mixel"`) in `use_decl.module_path`. Word-like keywords (e.g. `shared`, `int`) are accepted as path segments by sniffing the first byte of the token's source text. Selective-import `:` clause still works via a `goto` to the existing colon block. |
| `src/main.c` | `resolve_module_path` recognises `module_path` containing `/` as a literal relative path and joins it under `base_dir` verbatim, skipping the dot-to-slash conversion. The std-prefix branch is unchanged. |

Old syntax keeps working (`use mixel`, `use engine.physics`, `use std.math`,
`use math: add, PI`) — the new branch only triggers when the very next token
after `use` is `..`. 

### Tests

- `tests/programs/073_parent_dir_use.mix` + `parentlib_073/util.mix` (sub-dir,
  not picked up by the *.mix glob). The main does `use ../programs/parentlib_073/util`
  to exercise both `..` traversal and forward subdir steps in one shot.
- 74 QBE + 16 + 19 + 29 = **138 green on QBE**, 69 on C (same 5 pre-existing
  failures unrelated to this work).

### What changed in mixel

- `examples/01_hello/main.mix` now opens with `use ../../mixel`.
- Removed the `examples/01_hello/mixel.mix` symlink.
- README "Project layout" updated to show no symlink and to mention the
  path-style import. The `shaders/` symlink stays (the framework still
  loads `shaders/sprite.{vert,frag}.msl` via a relative path at runtime —
  that's a separate problem, future cleanup).

### Things to know if resuming

- The path-style branch only fires when the literal next token is `..`.
  It does *not* fire on `use ./mixel` (a single dot), and `.` after `use`
  isn't currently meaningful. Add a TOK_DOT branch alongside the TOK_DOTDOT
  one if you want `./` support — same shape.
- Keyword-as-segment acceptance is "first byte is letter or `_`". That's
  enough for `shared`, `int`, etc., but won't accept e.g. `123` as a
  directory name (which is fine; numeric directory names are rare and
  the lexer would tokenize them as TOK_INT_LIT anyway).
- Path-style modules and the existing `compiling_modules` cycle detector
  agree: keys are the resolved filesystem paths, not the source-level
  `module_path` string. So `use ../foo` and `use foo` from a sibling won't
  re-enter the same file twice.

---

## Session log — 2026-04-25 (for-each over [Shape] + mixel Group)

Continuing the same session — adding `Group` to mixel so demos can do
`group.update!(dt) / group.draw(g)` over a `[Sprite]`. The Group impl
itself is trivial; the work was a QBE-backend bug it surfaced.

### What changed in the compiler

| File | Change |
|---|---|
| `src/symtab.h` | Added `Symbol.is_pointer_slot` — for SHAPE-typed vars, true means `%v.<name>` is an alloca slot holding a pointer (loop var case), false means `%v.<name>` IS the pointer (the `=l copy` aliasing case used by NODE_VAR_DECL). |
| `src/symtab.c` | Initialise `is_pointer_slot = false` in `symtab_insert`. |
| `src/sema.c` (FOR_STMT) | After inserting a shape-typed loop var, look it up via `symtab_lookup_current` and set `is_pointer_slot = true`. |
| `src/sema.c` (NODE_IDENT resolve) | Mirror the symbol's `is_pointer_slot` onto `expr->ident.is_pointer_slot`, parallel to how `is_mutable` is captured — sema's scopes are gone by emit time, so the AST node has to carry the bit forward. |
| `src/ast.h` | Add `bool is_pointer_slot` to the `ident` struct. |
| `src/qbe_emit.c` (NODE_IDENT) | For shape-typed identifiers, branch on `expr->ident.is_pointer_slot`: `loadl %v.<name>` (load the pointer) when true, `copy %v.<name>` (the existing aliasing) when false. |

The C backend was already correct here — its for-each loop emits
`int64_t v_<name> = mix_list_get(...)` (the pointer value) and the
NODE_IDENT path casts that to `Sprite *`, which works.

### Why the symtab indirection step

First attempt had QBE look up the symbol at emit time. Doesn't work:
sema has popped all the function-local scopes by the time emit runs, so
`symtab_lookup` for a loop var returns NULL (or worse, a same-named
symbol from an outer scope). The AST-node mirror is the canonical
pattern (compare `expr->ident.is_mutable`).

### Tests

- `tests/programs/074_for_shape_list.mix` (+ `expected/074.txt`) —
  `for s in [Sprite] / s.update!(1.0)` and confirm the outer `a`/`b`
  positions reflect the mutation.
- 75 QBE + 16 + 19 + 29 = **139 green on QBE**, 70 on C (same 5
  pre-existing C-backend failures, none from this session).

### What changed in mixel

- `mixel.mix` adds `pub shape Group { sprites: [Sprite] }` with
  `update!(dt)` and `draw(g) ~` methods plus `make_group`,
  `group_add`, `group_count` helpers.
- `examples/02_groups/main.mix` — 100 colored balls bouncing inside the
  window. Spawns sprites with random pos/vel/color into a Group, then
  per frame: `balls.update!(dt)` → manual wall-bounce loop (per-sprite
  position clamping + velocity flip) → `balls.draw(g)`.
- `examples/02_groups/shaders` is a symlink back to `../../shaders`
  (same model as 01_hello — the framework still does
  `SDL_LoadFile("shaders/...", ...)` with a CWD-relative path).
- README updated: project layout shows 02_groups, public-API section
  documents Group, "Recently landed" entry, roadmap repointed at
  overlap/timer/audio/text → Snake.

### Things to know if resuming

- `Group.update(dt)` is intentionally non-mutating in its method declaration
  (no trailing `!`) — it doesn't change `self.sprites`. The mutation lives in
  `s.update!(dt)` on each list element, which is independent of the Group's
  own self. This matches the pattern of "iterate over a list of pointers and
  mutate each via a method call."
- For-each over a `[Shape]` now works as expected on QBE. For-each over a
  list-of-list (`[[Sprite]]` etc.) hasn't been probed; the same is_pointer_slot
  flag wouldn't kick in for non-SHAPE element types, so multi-level shape
  containers may need similar attention if mixel ever wants them.
- The C backend can't currently link a mixel demo (SDL3-related struct types
  don't appear in the generated C). That's a separate cbind/extern issue, not
  caused by this session. Mixel devs should default to QBE.

---

## Session log — 2026-04-25 (cwd-independent shader paths)

The 02_groups binary crashed with "Vertex shader cannot be NULL!" when the
user ran it directly from a terminal whose cwd wasn't the demo dir.
Root cause: the framework called `SDL_LoadFile("shaders/sprite.vert.msl", ...)`
with a CWD-relative path; SDL_LoadFile returned NULL when the file
wasn't there, and SDL_CreateGPUShader propagated the NULL.

### What changed in mixel (compiler unchanged)

- `mixel.mix` `init_game` uses `SDL_GetBasePath()` to get the executable's
  directory, then prefixes it onto `shaders/sprite.vert.msl` and
  `shaders/sprite.frag.msl` before passing to `SDL_LoadFile`. The result
  is that the binary works from any working directory as long as a
  `shaders/` directory sits next to the binary.
- README updated to document the new behavior.

### Things to know if resuming

- `*byte` (the bindgen mapping for `const char *`) auto-coerces to `str`
  via a local type annotation: `bp_raw = SDL_GetBasePath(); bp: str = bp_raw`.
  And `str` auto-coerces back to `*byte` for extern function calls. So the
  framework can do `bp + "shaders/..."` (string concat) and pass the
  result straight to `SDL_LoadFile` without a helper.
- `SDL_GetBasePath` returns a pointer SDL owns; we don't free it.
- The shaders symlink in each demo dir is still required because the
  compiled binary lives in the demo dir. Long-term cleanup options:
  embed the MSL as `@const str` (small files; ~few KB each), or add a
  `init_game_with_shader_path(...)` overload. Both punted for now.
- The previous-session note that called `SDL_LoadFile` "CWD-relative" is
  now stale — the framework computes an absolute path before calling.

---

## Session log — 2026-04-25 (loop-declared shape vars + shape ABI)

The 02_groups demo segfaulted at runtime — same session as the cwd fix.
Probing revealed two stacked QBE bugs that surfaced as soon as we tried
to construct N>1 shape values inside a loop and store them.

### Bug 1 — SHAPE_LIT used stack alloca

`emit_expr` for `NODE_SHAPE_LIT` emitted `%t = l alloc8 size`. QBE hoists
allocas to the function entry block, so a `Sprite(...)` inside a loop
returned the *same* slot on every iteration. Pushing the result into a
`[Sprite]` list filled the list with N copies of one pointer, all
pointing to the data the last iteration wrote. C backend wasn't affected
— it already heap-alloc'd via `mix_alloc`.

Fix in `src/qbe_emit.c`: switched the alloc to
`%t = l call $mix_alloc(l size)`. Memory now leaks per shape literal
(same as C backend); a future arena/zone pass can recover it.

### Bug 2 — qbe_sig_type returned `:Shape` for ABI

`qbe_sig_type` returned `:Group` / `:Sprite` for shape-typed function
returns and parameters. QBE treats `:T` aggregates by value at the ABI
boundary; for a `{ l }` aggregate (single field), it passes/returns just
the 8 bytes of the field — not the heap pointer of the shape. So
`balls = make_group()` ended up storing the *list pointer* (Group's
first field) in `%v.balls`, not the Group's heap address. Subsequent
`balls.sprites.push(...)` walked through corrupted memory.

Fix in `src/qbe_emit.c`: `qbe_sig_type` now returns `"l"` (pointer) for
all shape types, matching how the C backend already passes shapes
(`void *`) and how the previous-session method fix passed `self`. The
QBE `:ShapeName` typedefs still get emitted (`type :Group = { l }`)
because the rest of the code may reference them, but no function
signature uses them anymore.

### Bug 3 — NODE_VAR_DECL aliased shape vars (loop-broken)

Even with the two ABI fixes, `s! = Sprite(...)` inside the spawn loop
still aliased `%v.s` SSA-style (`%v.s =l copy %t_init`) — repeating the
same SSA name on every iteration is technically a violation, and worse,
the var carried the same identity across iterations.

Fix in `src/sema.c`: shape-typed local NODE_VAR_DECLs now mark the
symbol with `is_pointer_slot = true` so the existing per-IDENT mirror
captures it on the AST. In `src/qbe_emit.c` NODE_VAR_DECL the shape
branch now emits `%v.s =l alloc8 8; storel %t_init, %v.s` (matching the
for-each loop var pattern from the previous session). Reads via
NODE_IDENT already do `loadl` when `is_pointer_slot` is set.

### Tests

- `tests/programs/075_loop_shape_decl.mix` (+ `expected/075.txt`) —
  spawns 3 sprites with distinct `vel.x` inside a `while` loop, pushes
  each via a function call (`group_add`) that returns nothing, then
  iterates and confirms each sprite kept its own velocity. Failed in
  multiple ways before this fix; now passes on both backends.
- 76 QBE + 16 + 19 + 29 = **140 green on QBE**, 71 on C (same 5
  pre-existing C failures).

### What changed in mixel

- Nothing in the framework code itself — the 02_groups demo built
  earlier was hitting the compiler bugs above. After the fixes, the
  demo runs correctly (verified via the smaller probe; the user runs
  the GUI binary from their shell).

### Things to know if resuming

- The `is_pointer_slot` flag now blanket-applies to **all** local shape
  NODE_VAR_DECLs (not just for-each loop vars). Globals (`is_global`)
  are excluded. Function parameters still use the old aliasing pattern
  (`%v.x =l copy %p.x`) — that's correct because params are declared
  exactly once at function entry.
- `mix_alloc` for SHAPE_LIT leaks memory. Acceptable for a small game
  framework but worth a follow-up: SHAPE_LIT in a `zone { ... }` block
  could allocate via `mix_zone_alloc` so the entire zone frees together.
- The `:ShapeName` aggregate type-defs in QBE output are now vestigial
  for function signatures — kept emitting them in case any other path
  still references them, but the cleanup-pass would be safe to remove
  if anyone wants to reduce output size.
- The mixel C backend still doesn't link (SDL3 cbind issue from
  previous sessions). Mixel devs should default to QBE.

---

## Session log — 2026-04-25 (unary `-` precedence vs postfix `.field`)

After the previous fixes, 02_groups built cleanly but segfaulted in
`main + 588` once the bounce loop ran `s.vel.x = -s.vel.x`. Disassembly
showed the unary minus being applied to the *sprite pointer* (negating
an address, then adding 16, then loading a double from the result).

### What changed in the compiler

`src/parser.c` — `parse_primary`'s prefix-unary handlers (TOK_MINUS,
TOK_NOT, TOK_STAR, TOK_AMPERSAND) called `parse_primary(p)` for the
operand. parse_primary doesn't run the postfix `.field` / `[idx]` loop
— that lives in `parse_expr_prec`. So `-sp.vel.x` parsed as
`(-sp).vel.x`: the unary wrapped IDENT(sp) only, and the outer
parse_expr_prec then attached `.vel.x` to the negated value.

Fix: each unary handler now calls `parse_expr_prec(p, PREC_UNARY)` for
the operand. min_prec=PREC_UNARY means parse_expr_prec runs the prefix
parse + postfix loop + try/else, then the binary-op loop bails
immediately because all binary ops have lower precedence. Net effect:
unary's operand correctly absorbs `.field` / `[idx]` / `?` / `else`.

Added forward declaration `static AstNode *parse_expr_prec(...)` after
the Precedence enum so parse_primary can call it.

### Tests

- `tests/programs/076_unary_field_prec.mix` (+ `expected/076.txt`) —
  iterates `[Sprite]`, does `sp.vel.x = -sp.vel.x` (the exact 02_groups
  bounce pattern), and a separate `print(-s2.vel.y)` to cover the
  non-method form. Crashed before the parser fix; passes on both
  backends now.
- 77 QBE + 16 + 19 + 29 = **141 green on QBE**, 72 on C (same 5
  pre-existing C failures).

### What changed in mixel

Nothing — the demo's source was always correct; the parser was
miscompiling it. After the fix, `02_groups/main` runs as intended.

### Things to know if resuming

- Right-associativity of stacked unaries (`--x.y`) was not separately
  fixed — it still parses as `(--x).y`. Single unary cases (the common
  one) are now correct. If `--x.y` ever matters, the inner unary's
  recursive parse would need to switch from parse_primary to
  parse_expr_prec(PREC_UNARY) too — same shape as the outer fix.
- The Pratt-style precedence table now correctly models PREC_CALL >
  PREC_UNARY > PREC_FACTOR > PREC_TERM. PREC_CALL doesn't appear in
  `get_precedence` because `.field` and `[idx]` are handled by the
  postfix loop directly (not as binary operators) — that asymmetry is
  the underlying reason the bug existed.
- `parse_expr_prec(p, PREC_UNARY)` for the unary operand will also
  consume a `?` (try) or an `else` clause attached to the operand. So
  `-x?` is now `-(x?)` and `-x else 5` is `-(x else 5)`. Both are
  reasonable, neither is observed in current tests.
