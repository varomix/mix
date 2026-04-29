# MIX LLVM Migration Plan

> Status: proposed
>
> Goal: move MIX from a QBE-first compiler to an LLVM-first compiler without
> destabilizing the language semantics that are now in place.

## Summary

MIX should adopt LLVM as its primary backend now, while the language is still
young and backend assumptions are not yet too expensive to unwind.

The practical migration strategy is:

1. Keep the current frontend: lexer, parser, sema, type system, monomorphized
   generics, and runtime model stay in place.
2. Add LLVM as a new primary backend.
3. Keep the C backend during the transition as the portability fallback.
4. Freeze QBE to bug-fix mode, then remove it once LLVM is the stable default.
5. Add only a small lowering layer between sema and codegen.
6. Bring LLVM up first as textual `.ll` emission plus `llc` / `clang`, then
   add tighter LLVM integration later only if it is still needed.

This is intentionally not a "build a giant backend-neutral compiler framework"
plan. The point of the lowering layer is to stop encoding language semantics
directly inside backend-specific emitters like `src/qbe_emit.c` and
`src/c_emit.c`, not to create a massive new compiler architecture.

## Why Switch Now

LLVM is the right strategic choice for MIX if the language is expected to grow
into:

- desktop targets beyond the current happy path
- mobile targets
- wasm and browser targets
- better debug info and toolchain integration
- stronger native optimization and ABI handling

QBE is still valuable, but only as a small native backend. It is not a good
long-term center of gravity for MIX if the language is intended to ship broadly.

The main reason to do this now instead of later is simple: every month spent
hardening QBE-specific lowering is another month of compiler behavior that will
need to be unwound.

## Current State

Today the compiler effectively has this shape:

```text
lexer -> parser -> sema -> qbe_emit / c_emit -> cc/link
```

The relevant current files are:

- `src/main.c`
- `src/qbe_emit.c`
- `src/c_emit.c`
- `src/sema.c`
- `src/types.c`
- `lib/runtime.c`

This works, but it has a real cost:

- language semantics are duplicated across backends
- aggregate lowering rules are repeated in multiple places
- ABI decisions leak into backend-specific code
- subtle bugs show up as backend bugs instead of frontend-lowering bugs

Recent issues around shape assignment, stack allocation hoisting, foreach shape
bindings, and GPU-facing text/resource paths were all examples of this.

## Compile-Speed Tradeoff

This plan explicitly accepts a compile-speed regression versus QBE.

That regression affects mostly the programs MIX compiles:

- `mix build`
- `mix run`
- `mix test`

It is not a runtime-speed regression. LLVM-generated programs should be as fast
or faster than QBE-generated programs. The cost is compile latency and dev-loop
feel.

This plan still retires QBE in the end because MIX does not want to permanently
maintain two native code paths. That means choosing backend unification over
the fastest-possible compile loop.

Planned mitigations:

- keep dev/debug builds on low optimization settings
- bring LLVM up first via textual `.ll`, which keeps the first integration
  simpler and more inspectable
- keep the C backend as the portability fallback during and after the migration
- revisit caching and incremental-build work later if LLVM compile time becomes
  a real workflow bottleneck

This tradeoff is not accepted on faith. Phase 3 includes an explicit
compile-time measurement gate that compares LLVM wall time against the QBE
baseline before committing to later phases. If the measured ratio is
unworkable, the plan pauses for mitigation or pivot rather than pushing
through to QBE retirement.

## Core Decisions

### 1. LLVM becomes the primary backend

LLVM should become the default native backend once it reaches parity on the
main supported host platform.

### 2. The C backend stays

The C backend remains valuable even after LLVM lands:

- portability fallback
- inspection/debugging of generated code
- unusual platforms with a C compiler but weak LLVM story
- practical bootstrap escape hatch

### 3. QBE enters maintenance mode

QBE should receive only bug fixes during the migration. No new major language
features should be designed around QBE-specific constraints.

### 4. Add a small lowering layer, not a giant MIR project

LLVM IR is not a replacement for frontend lowering discipline. MIX still needs
a compact lowered representation so semantics like:

- value-shape copies
- `ref` / `ref!`
- `Box[T]`
- `Zone`
- list/map/set mutation
- mutable params and writeback loops
- `defer`, `fail`, and `done`

become explicit before codegen.

But this should be small and practical, not a multi-year "compiler framework"
detour.

The scope fence matters:

- LIR does represent allocas, loads/stores, shape copies, field addresses,
  explicit control flow, loop writeback points, and runtime calls.
- LIR does not represent optimization passes, generic instantiation, type
  inference, long-lived frontend type metadata, register allocation, or
  target-specific foreign ABI policy.

### 5. Start with textual LLVM IR and shell-out tooling

The first LLVM backend should emit textual `.ll` and shell out to `llc` and
`clang`.

Why:

- the build stays lighter during bring-up
- emitted IR is readable and diffable
- failures are easier to inspect
- no LLVM library linking is required on day one

Direct LLVM API integration should come later, after correctness exists and
there is a concrete reason to tighten the integration.

### 6. Keep the compiler in C for now

MIX is currently a C compiler. The least disruptive first move is to use the
existing compiler in C and add the new backend incrementally.

If the textual-IR path later needs richer integration, the first step should be
the LLVM C API. If that still becomes painful around debug info or target
machine handling, a small C++ bridge is acceptable later. Neither should be the
starting point.

### 7. Pin one LLVM version

Bring-up should target one LLVM version, not a range.

Initial pin (revised 2026-04-28 during Phase 1 bring-up after discovering
the local toolchain was LLVM 21 rather than 18):

- Homebrew LLVM 21 on macOS — `/opt/homebrew/opt/llvm`

The build should look for the pinned toolchain explicitly. Do not try to be
"LLVM version agnostic" during the migration. Switching pins later is a
deliberate, recorded decision (CHANGELOG decision log), not an implicit
upgrade.

## Proposed Architecture

Target architecture after migration:

```text
lexer
  -> parser
  -> sema
  -> lower
  -> llvm_emit   (primary)
  -> c_emit      (fallback / portability)
```

QBE remains temporarily:

```text
lexer -> parser -> sema -> qbe_emit
```

but should stop being the place where new semantics are introduced.

## The Lowering Layer

Add a small lowered IR or lowered CFG stage. Suggested files:

- `src/lower.h`
- `src/lower.c`
- `src/lir.h`
- `src/lir.c`

This layer should make the following operations explicit:

- local variable slots
- function-entry allocas
- loads and stores
- whole-shape copies as explicit memcpy-style ops
- aggregate field address calculations
- calls to runtime helpers
- structured control flow lowered to blocks and branches
- explicit loop writeback points
- explicit `defer` cleanup order
- explicit return / fail / panic exits

This layer does not need to model every possible optimization. It is only
supposed to give LLVM and C a shared, explicit lowered program shape.

## Phases

## Phase 0 — Baseline and Guardrails

Before new LLVM work starts:

- freeze the current compiler behavior with the existing test suite
- add targeted regression tests for backend-sensitive shape semantics
- preserve current `mixel` compile-sweep coverage
- document the current runtime ABI expectations around shapes, boxes, refs, and zones

Must-have regression areas:

- whole-shape assignment
- shadowing vs reassignment
- mutable foreach writeback
- `at()` / `at_mut!()`
- mutable params
- `Box[T]` promotion and zone reset behavior
- C interop with shapes and pointers

Acceptance:

- `tests/run_tests.sh` green
- `tests/run_error_tests.sh` green
- `mixel/examples/**/main.mix` full compile sweep green

## Phase 1 — LLVM Text IR Bring-Up

Add LLVM as a buildable backend without changing semantics yet.

Work:

- add `--backend llvm`
- keep `--backend qbe` and `--backend c`
- teach `Makefile` to find the pinned LLVM toolchain
- add new files:
  - `src/llvm_emit.h`
  - `src/llvm_emit.c`
- make `--emit-ir` produce LLVM IR (`.ll`) for the LLVM backend
- shell out to `llc` and `clang` initially
- do not link LLVM libraries into `mix` yet
- keep final system linking in `cc` through the existing `main.c` path

Target for this phase:

- `hello.mix` builds and runs on macOS host
- emitted `.ll` is valid
- `llc` + `clang` pipeline works end to end

## Phase 2 — Lowering Layer Bring-Up

Introduce the small lowering stage and move semantic lowering out of
`qbe_emit.c`.

Work:

- lower functions one at a time into explicit blocks and ops
- centralize alloca decisions at function entry
- centralize shape copy decisions
- centralize mutable param treatment
- centralize return/cleanup lowering

The rule here is important:

- if a semantic rule exists in both `qbe_emit.c` and `c_emit.c`, it should
  probably move into lowering

Examples:

- shape assignment via memcpy
- foreach element binding shape copy/writeback
- `done` and cleanup epilogue control flow
- field-address computation for nested shapes
- list element borrow vs copy behavior

Acceptance:

- old backend behavior preserved
- no test regressions
- one small subset of the language can be emitted both from old direct emit
  and the new lowering path

## Phase 3 — LLVM Scalar and Control-Flow Parity

Bring LLVM up on the easy half first.

Work:

- ints, floats, bools, strings
- locals and globals
- basic assignments
- function calls
- if / while / range for
- arithmetic and comparisons
- runtime calls
- string literals and global data

Do not touch the hard aggregate semantics first. Get the basic host pipeline
stable before pulling in shapes and collections.

Compile-time measurement (gate before later phases):

This is the first point in the migration where there is enough working LLVM
output to measure dev-loop cost honestly. Take that measurement before
committing further.

Work:

- record a QBE baseline: full `mixel` compile-sweep wall time, plus individual
  wall times for `hello.mix` and one representative scalar/CFG-heavy demo
- run the same workloads through the LLVM backend at `-O0`
- record the ratio (LLVM wall time / QBE wall time) per workload
- write the numbers into the repo so later phases can reference them

This is a checkpoint, not an automatic early-exit. The number informs whether
to push forward into Phase 4 as planned, or to pause and add dev-loop
mitigations before continuing.

Suggested thresholds:

- ratio under 10x: proceed normally
- ratio 10x to 25x: proceed, but plan dev-loop mitigations (caching,
  incremental builds, parallel codegen) alongside Phase 4
- ratio over 25x: stop and reconsider before continuing. Bringing QBE back is
  not the intended pivot. Options include caching/incremental work, a
  different backend choice (for example a custom or alternative IR backend),
  or a redesigned pipeline. Do not silently push through to Phase 9 in this
  case.

Acceptance:

- the early runtime tests pass on LLVM
- emitted debugless binaries run correctly on macOS host
- compile-time ratio measured and recorded for at least one `mixel` workload
- a written go / pause / pivot decision based on the measured ratio

## Phase 4A — Basic Shapes and Aggregate ABI

This is the beginning of the hard part. It is still smaller than the full
value-semantics surface and should be treated as its own phase.

Work:

- shape layout and copies
- function params and returns for shapes
- field loads and stores

Critical rule:

- shape semantics must be decided once in lowering, not re-invented inside the
  LLVM emitter

Acceptance:

- basic shape tests pass on LLVM
- function-call and field-access behavior matches current semantics

## Phase 4B — Borrows, Boxes, Zones, and Mutable Containers

This is likely the single highest-risk phase in the migration. Most recent
backend bugs have lived here.

Work:

- `ref T` / `ref! T`
- `Box[T]`
- `Zone`
- inline value semantics for list/map/set elements
- `at()` / `at_mut!()`
- `for item in list` and `for item! in list`
- mutable params on shapes and collection elements

Acceptance:

- all value-shape, borrow, box, zone, and collection tests pass on LLVM
- text/HUD-style reassignments like `text! = make_text(...)` are proven safe

## Phase 5 — Advanced Language Features

Bring over the rest of the language surface:

- optionals and results
- tagged unions and match lowering
- `defer`
- `fail`
- lambdas / closures
- generic instantiation output
- `go` / `wait` and shared runtime calls
- conditional compilation
- extern/use-c paths

This phase should include careful ABI review, especially for:

- struct returns
- pointer params
- imported C structs
- function pointers

Acceptance:

- full main test suite passes on LLVM
- error suite still green

## Phase 6 — LLVM Native Integration and Debug Info

Once correctness exists, decide whether the textual `.ll` pipeline is still
sufficient. If it is not, this is the phase where tighter LLVM integration
should happen.

Work:

- optionally replace the shell-out backend core with LLVM C API integration
- emit object files directly if that materially improves the toolchain flow
- keep textual `.ll` emission available for inspection
- line tables for statements and expressions
- function and file metadata
- source mapping through lowered blocks
- parity with the current `--debug` expectation

Nice-to-have later:

- richer local variable location info
- better stack traces

Acceptance:

- direct LLVM integration, if added, matches the textual path semantically
- `lldb` line stepping works on LLVM-built binaries
- source locations are not materially worse than the current backend

## Phase 7 — Platform Expansion

Do not start here. Get host parity first.

Recommended order:

1. macOS arm64 host parity
2. Linux x86_64
3. Linux aarch64
4. Windows x64
5. wasm32-wasi
6. browser wasm
7. iOS
8. Android

Important caveat:

LLVM can produce code for these targets. That does not automatically make the
MIX runtime or `mixel` portable to them. Browser/mobile support still needs a
platform/runtime layer and likely graphics/audio backend work.

## Phase 8 — Default Backend Flip

Once LLVM is solid on the main host:

- make `llvm` the default backend
- keep `c` as supported fallback
- keep `qbe` available but clearly deprecated
- update docs, tutorials, and install instructions

Acceptance:

- host platform test suite green with default backend
- `mixel` examples compile with default backend
- at least a smoke subset of `mixel` runs correctly

## Phase 9 — QBE Retirement

Retire QBE only after LLVM has proven stable.

Suggested retirement gate:

- LLVM passes all compiler tests
- LLVM compiles the full `mixel` example tree
- at least the commonly used `mixel` demos run correctly
- the C backend still works as fallback

After that:

- remove `qbe_emit.c`
- remove QBE-specific docs and install steps
- remove QBE-specific backend conditionals from `main.c`

This is an intentional product decision, not an accident. MIX is choosing one
primary native backend over a permanently split "fast dev backend" and
"shipping backend" model.

## What Gets Reused

The LLVM migration should reuse as much of MIX as possible:

- lexer
- parser
- sema
- symbol table
- type system
- monomorphization model
- runtime library in `lib/runtime.c`
- existing tests
- build/link flag plumbing in `src/main.c`
- C header binding system

This is a backend migration, not a language rewrite.

## What Must Change

The main code paths that need real change are:

- `src/main.c`
- `Makefile`
- new lowering files
- new LLVM emitter files
- docs that currently describe QBE as the main backend

The code that should gradually lose semantic responsibility is:

- `src/qbe_emit.c`
- `src/c_emit.c`

They should become consumers of lowered semantics, not the place where language
behavior is invented.

## Suggested File Plan

Add:

- `src/llvm_emit.h`
- `src/llvm_emit.c`
- `src/lower.h`
- `src/lower.c`
- `src/lir.h`
- `src/lir.c`

Modify:

- `Makefile`
- `src/main.c`
- `README.md`
- `docs/DOCS.md`
- `docs/index.html`
- `docs/c-backend.html`

Leave in place initially:

- `src/qbe_emit.c`
- `src/c_emit.c`

## Testing Strategy

During migration, run three levels of validation:

### 1. Frontend invariants

These should not depend on backend:

- parsing
- sema errors
- type inference
- generic instantiation

### 2. Backend parity

For overlapping targets, compare:

- QBE output behavior
- C backend behavior
- LLVM backend behavior

LLVM does not need to match QBE instruction shape. It must match language
behavior.

### 3. `mixel` validation

This matters because `mixel` is the best real stress test currently in the repo.

Required checks:

- full compile sweep of `mixel/examples/**/main.mix`
- smoke demos: `02_groups`, `03_collide`, `05_random`, `07_text`
- Phase 6 arcade demos after parity

## Risks

### LLVM build complexity

LLVM is heavier than QBE. The build needs to stay practical for normal MIX
development.

Mitigation:

- use the pinned Homebrew LLVM toolchain explicitly
- keep the first integration simple
- do not overbuild unnecessary LLVM components

### Two-backend duplication during transition

LLVM + C + QBE at once is real maintenance cost.

Mitigation:

- freeze QBE early
- move semantics into lowering
- do not add new features directly to QBE unless required for stability

### ABI mistakes on aggregates

This is the highest-risk technical area.

Mitigation:

- explicit lowering for shape copies, field access, params, and returns
- targeted regression tests
- validate generated host ABI against C interop cases

### Web/mobile false confidence

LLVM target support is not the same as finished platform support.

Mitigation:

- treat backend portability and runtime/platform portability as separate workstreams

## Explicit Non-Goals

This migration plan does not aim to:

- build a large abstract optimizer inside MIX
- rewrite the frontend
- redesign the type system
- redesign the value/zone/box model
- solve browser/mobile runtime support in the same change
- keep QBE as an equal long-term backend

## What LLVM Does Not Solve

This migration does not solve the remaining frontend and language-design
questions by itself.

Examples:

- function-type syntax polish
- lambda syntax and capability limits
- continuation/control-flow syntax work
- remaining function-pointer ABI design questions
- name-shadowing ergonomics and related frontend rules

LLVM is backend and reach work. It is not a substitute for frontend language
design.

## Exit Criteria

The LLVM migration is complete when:

- LLVM is the default backend
- the full compiler test suite passes on LLVM
- the `mixel` compile sweep passes on LLVM
- the main host platform works without QBE installed
- the C backend remains available
- QBE is either removed or clearly deprecated

## Recommended Immediate Next Steps

1. Add `--backend llvm` plumbing and a minimal `llvm_emit` skeleton.
2. Make that backend emit textual `.ll` and drive `llc` / `clang`.
3. Add the small lowering layer before porting semantics.
4. Get `hello.mix` building to a native object and final executable via LLVM.
5. Port scalar/control-flow codegen first.
6. Split aggregate work into basic shapes first, then borrows/boxes/zones.
7. Flip the default backend only after `mixel` smoke coverage is real.

That is the shortest path that is still technically sound.
