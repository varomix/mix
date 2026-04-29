# MIX Runtime ABI

> Status: written 2026-04-28 as a Phase 0 guardrail for the LLVM migration.
> Source of truth: `src/types.c` (`type_size`, `type_alignment`,
> `type_to_qbe_type`) and `src/qbe_emit.c`.
>
> This document records the ABI the QBE backend produces today. Any new
> backend (LLVM, future C-backend changes) must match this contract or
> document an intentional break in `CHANGELOG.md`.

## Scalar Types

| MIX type        | Size (bytes) | Alignment | C equivalent | QBE type |
|-----------------|--------------|-----------|--------------|----------|
| `bool`          | 1            | 1         | `_Bool`      | `b`      |
| `byte`          | 1            | 1         | `uint8_t`    | `b`      |
| `int8` / `uint8`   | 1         | 1         | `int8_t` / `uint8_t`   | `b` |
| `int16` / `uint16` | 2         | 2         | `int16_t` / `uint16_t` | `h` |
| `int32` / `uint32` | 4         | 4         | `int32_t` / `uint32_t` | `w` |
| `float32`       | 4            | 4         | `float`      | `s`      |
| `int` / `int64` / `uint64` | 8 | 8       | `int64_t` / `uint64_t` | `l` |
| `float` / `float64` | 8        | 8         | **`double`** (not `float`) | `d` |
| `*T` (ptr)      | 8            | 8         | `T*`         | `l`      |
| `ref T` / `ref! T` | 8         | 8         | `T*`         | `l`      |
| `Box[T]`        | 8            | 8         | opaque pointer | `l`    |
| `str`           | 8            | 8         | `const char*` | `l`     |
| `Zone`          | 8            | 8         | opaque handle | `l`    |

**Critical:** MIX `float` is 64-bit (C `double`), not 32-bit. Code interop
with C must use `double` to match. Use `float32` if 32-bit is required.

`bool` is 1 byte. Earlier QBE codegen used 4-byte `w` for bool fields and
corrupted the following struct field — see test `083_bool_field_overlap`.
Any new backend must keep bool 1 byte in shape layout.

## Shape Layout

Shapes are laid out like C structs:

- fields appear in declaration order
- each field is aligned to its own `type_alignment`
- the shape's overall alignment is the max field alignment
- `total_size` is rounded up to the shape's alignment
- no automatic padding insertion beyond per-field alignment requirements
- no field reordering

This layout is **memcpy-compatible with the equivalent C `struct`**, which
makes shape pointers usable as C struct pointers across `extern "C"` (or
`extern "libc"`) declarations. The Phase 0 regression test
`106_c_shape_abi.mix` locks this in.

## Shape Calling Convention

- **By value:** shape values are copied. The QBE backend implements this
  with explicit `memcpy` calls (`emit_shape_copy`). The new lowering layer
  (Phase 2) is the right place to make this explicit and reusable.
- **Returns:** functions returning a shape return by value (the caller
  passes a hidden destination pointer in the QBE lowering). Acceptance for
  Phase 4A includes verifying this still works for both small and large
  shapes.
- **Pointers:** `&shape_value` produces a pointer with C struct layout.
  See "Known Quirk" below for the immutable-binding exception.

## Box, Ref, Zone

- `Box[T]` is a single pointer-sized handle. The pointee lives in zone
  memory. The runtime helpers manage promotion, retention, and zone reset.
- `ref T` and `ref! T` are raw pointers (`T*`) at the ABI level. The
  difference is purely a sema-level mutability check.
- `Zone` is an opaque pointer-sized handle. All zone operations go through
  the runtime in `lib/runtime.c`.

## Runtime Library Linkage

The compiler links every program against `lib/runtime.c` (precompiled to
`build/runtime.o`). The runtime exports the helpers MIX programs call
implicitly: print, string formatting, list/map/set helpers, zone
management, box promotion, etc. The new backend must call these helpers
with the same signatures and the same expectations.

## C Interop

- `extern "libc"` and `extern "C"` declarations bind to ordinary C
  symbols. MIX uses the host C ABI (System V on Linux, AAPCS64 on
  macOS/arm64, etc.) — no MIX-specific wrapping.
- Shape pointers passed across `extern` are usable as C struct pointers
  (see "Shape Layout").
- `use c "header.h"` runs the cbind path, which parses C headers and
  emits MIX bindings. cbind's struct-mapping is currently
  partially-working (see CHANGELOG: SDL_Event nested types fail under the
  C backend). LLVM bring-up should not depend on cbind correctness.

## Known Quirks Worth Preserving (or Intentionally Breaking)

These are pre-existing behaviors the new backend will encounter. Decide
per-item whether to replicate or fix in writing — do not change silently.

1. **`&` of an immutable shape binding gives a garbage pointer.** With
   `a = Vec2(...)`, `&a` does not point at usable storage. Workaround
   today is `a! = Vec2(...)`. Discovered while building test 106 on
   2026-04-28. The LLVM migration should either preserve this for source
   compatibility (and reject `&immutable` in sema) or lift the
   restriction. Either way, decide explicitly.

2. **C backend cannot compile `mixel`** today (0/31 demos). 17/106 main
   tests also fail under C backend. The plan treats C as a fallback;
   that fallback is currently aspirational, not actual. Recorded in
   CHANGELOG. Not a Phase 0 fix.

3. **`use c` SDL_Event nested struct types fail** under the C backend
   (forward declarations not emitted for nested struct fields). Captured
   in CHANGELOG.

## Reference Files

- Type sizes/alignments: `src/types.c`
- QBE codegen for shapes/scalars: `src/qbe_emit.c`
- C backend codegen: `src/c_emit.c`
- Runtime helpers: `lib/runtime.c`
- Regression tests for ABI: `tests/programs/106_c_shape_abi.mix`,
  `tests/programs/083_bool_field_overlap.mix`
