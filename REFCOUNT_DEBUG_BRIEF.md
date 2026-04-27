# MIX Refcount — Debug Brief for Second Opinion

> Status: 2026-04-26. Phases 1–3.5 of refcount landed; mixel still leaks
> ~30 MB/min and now exhibits a new use-after-free symptom. Looking for
> a fresh review of the design and codegen.

## TL;DR

We're adding shape-allocation refcounting to MIX to fix a long-standing
heap leak. The infrastructure is in place and unit tests pass exactly,
but on the real workload (a Flixel-like 2D engine called `mixel`):

1. **Memory still grows ~30 MB/min** in the simplest demo (100 bouncing
   balls). The leak rate is roughly the same as before refcount.
2. **Sprite physics goes haywire** — bouncing balls suddenly accelerate
   to bizarre speeds. Strong smell of use-after-free: a freed Sprite's
   memory gets reallocated, the new bytes happen to look like a giant
   velocity, and the foreach loop keeps reading from the dangling ptr.

So we have the worst of both worlds: heap is not actually being
reclaimed, yet *something* is being freed too aggressively.

We need a fresh pair of eyes on whether the design is fundamentally
sound and whether the codegen is implementing it correctly.

---

## Background — the original leak

Every MIX `Shape(field: ...)` literal calls `mix_alloc()`. There is no
GC, no destructors, no automatic free. Acknowledged in MIX's own
codegen ([`src/qbe_emit.c:1769`](src/qbe_emit.c)):

> "Heap allocation guarantees fresh memory per construction; the
> tradeoff is that SHAPE_LIT now leaks (same as the C backend)."

Mixel's per-frame work constructs ~10 SDL shapes (`SDL_FColor`,
`SDL_GPUColorTargetInfo`, `SDL_GPUBufferBinding`,
`SDL_GPUTextureSamplerBinding` per draw segment, …) plus user-side
`Color`/`Vec2`. At 60–120 fps these accumulate fast. After ~30 sec
of play the heap fragments badly enough to corrupt CFRunLoop
internals; the next `_CFAutoreleasePoolPop` recurses into garbage and
crashes with a "stack guard" SIGSEGV inside `pump()`.

`02_groups` (100 bouncing balls, no game logic) is the simplest demo
that reproduces the leak — RAM grows steadily in Activity Monitor.

## The chosen fix — reference counting

We picked **Option C** from a 3-option list (the other two were
escape-analysis with stack alloc, and auto-free at scope end). The
user explicitly chose the most thorough fix:

> "If the base which is mix has issues everything has issues, let's
> fix the root of the issue."

Full design plan: [`mix_lang/REFCOUNT_PLAN.md`](mix_lang/REFCOUNT_PLAN.md).

### Memory layout

Every shape allocation is prefixed by 16 bytes of header:

```
[refcount: i64][release_fn: void(*)(void*)] [user fields...]
                                              ^
                                              pointer returned to MIX
```

`mix_shape_alloc(size, release_fn)` adds the header. `mix_release(p)`
reads the header at `p - 16`. `mix_release` is null-safe; refcount
hits 0 → calls `release_fn(p)` (which today just does
`mix_shape_free`); release_fn NULL → falls back to `mix_shape_free`.

### Per-shape release functions

The compiler emits `release_<ShapeName>(void *p)` for every locally-
declared shape — currently a stub that calls `mix_shape_free`.
Imported / C-bound shapes get NULL release_fn (safe — they're just
plain C structs without recursive cleanup needed).

### Codegen rules

| Op | Action |
|---|---|
| `SHAPE_LIT` | `mix_shape_alloc(size, release_fn)`; refcount=1 |
| Local `x = SHAPE_LIT(...)` | store ptr (caller takes transferred ref) |
| Local `x = some_var` (borrowed) | **today: not tracked** (safety) |
| `obj.field = SHAPE_LIT(...)` | memcpy + release source temp |
| `arr.push!(SHAPE_LIT(...))` | runtime retains; we release temp |
| `arr.pop!()` discarded | release the popped value |
| `f(SHAPE_LIT(...))` | release temp after call returns |
| Reassignment of tracked shape local | release old slot value before storing |
| Function exit | release each tracked shape local |

### Pre-init for safety

A var-decl inside `if`/`while` may never execute at runtime, leaving
its stack slot uninitialized. To avoid scope-exit release loading
garbage, we **pre-walk the function body** at codegen time and emit
`alloc8 + storel 0` at function entry for every owned-shape local.
NODE_VAR_DECL skips its own `alloc8`. This was a real bug — caused a
crash in `end_frame + 1592` at address `0xdeaddead`.

---

## What's actually implemented

Files changed:
- `mix_lang/lib/runtime.c` — `MixHeader`, `mix_shape_alloc`,
  `mix_retain`, `mix_release`, `mix_shape_free`. List ops gain
  `is_shape` flag + `element_release_fn`; push/set/remove/insert
  retain/release.
- `mix_lang/src/qbe_emit.c` — emits `release_<Shape>` per local shape;
  SHAPE_LIT routes through `mix_shape_alloc`; LIST_LIT for
  `[Shape]` uses `mix_list_new_shape`; CALL_EXPR / METHOD_CALL release
  owned-shape args after the call; var-decl release-on-reassign;
  scope-exit release; pre-init walker.
- `mix_lang/src/c_emit.c` — same pattern, partial parity (no
  scope-exit release yet on C backend).
- `mix_lang/lib/runtime.c` — `mix_rc_get_alloc_count` /
  `mix_rc_get_free_count` test helpers.
- `mix_lang/src/sema.c` — registers `_mix_rc_alloc_count` /
  `_mix_rc_free_count` builtins.

### Tests passing (89/89)

- `tests/programs/086_shape_refcount_phase1.mix` — basic shape +
  field reads + nested + cross-fn return.
- `tests/programs/087_shape_refcount_phase2.mix` — exact alloc/free
  accounting for inline-shape constructor temps and field-assign
  temps. After Phase 3.5 reassign-release, tracks 200 allocs / 199
  frees in a 100-iter loop (last value released at scope exit).
- `tests/programs/088_shape_refcount_phase3.mix` — shape list
  push (retain) + pop+discard (release). Exact accounting:
  101 allocs, 101 frees after drain.

Plus all 86 pre-existing tests still pass. All 29 mixel demos still
build.

---

## What's wrong now

### Symptom 1: leak rate unchanged on real workload

`02_groups`: 100 sprites bouncing in a 320×240 window. After Phase
3.5 (call-arg release + reassignment release), Activity Monitor still
shows ~30 MB/min RAM growth — same as before refcount. The synthetic
unit tests show the mechanism *works*; the real workload says it
doesn't help in practice.

Hypotheses:

(a) **The allocations causing the leak aren't the ones we instrumented.**
Mixel's per-frame work is dominated by SDL GPU command buffer / draw
encoder / drawable autoreleased Metal objects, not MIX shapes. macOS's
autorelease pool grows because *SDL itself* is creating NSObjects we
don't control. If true, refcount won't help and we should look at
giving SDL the chance to drain its pool (e.g., calling
`SDL_PumpEventsInternal` more aggressively, or wrapping the loop in
an explicit `@autoreleasepool`).

(b) **Our refcount emits aren't actually firing where we think.**
Maybe the LIST_LIT path doesn't see `[Sprite]` as a shape list (the
element type is inferred and might come back NULL or as a generic).
Verifying by inspecting the emitted QBE IR for end_frame would settle
this in one minute.

(c) **Something is retain-only-never-release.** If push retains but
the release path (clear / drain) is never hit because the list lives
forever, the list's elements never go to refcount 0.

Most likely combination: (a) plus (c). For 02_groups specifically,
the `balls` group is created once and lives forever — sprites in it
stay at refcount 1 indefinitely. That's fine if they never were
supposed to be freed (they aren't), so this isn't the leak source.
The leak is per-frame allocations.

### Symptom 2: balls accelerate weirdly (use-after-free)

Far more concerning. The 02_groups main loop:

```mix
while not pump(g)
    balls.update!(dt)               // each sprite: pos += vel*dt
    for s in balls.sprites
        if s.pos.x < 0.0
            s.pos.x = 0.0
            s.vel.x = -s.vel.x      // bounce
        ...
    begin_frame(g, cornflower())
    balls.draw(g)
    end_frame(g)
```

Sprites should bounce off walls at constant speed forever. They
don't — speeds suddenly spike. That's the classic use-after-free
signature: a sprite's bytes are reallocated to something else (a
list internal struct, an SDL buffer); the next time we read
`s.vel.x` we read garbage that happens to look like a giant velocity.

Possible bug locations:

- **Foreach over a shape list.** `for s in balls.sprites` allocates
  `%v.s =l alloc8 8` and stores raw pointers into it (no retain). If
  the list shifts memory or an element is freed mid-iteration, the
  loop variable goes stale. Mostly safe if no one mutates the list
  during the loop, but our retain/release machinery could do so
  indirectly.
- **Setup loop reuses slot `%v.s`.** The setup loop declares
  `s! = make_sprite(...)` 100 times. The pre-init walker collected
  `s` once. The reassign-release fires on iters 2..100, dropping the
  PREVIOUS sprite's refcount. With list_push retaining, the old
  sprite goes from refcount 2 → 1 (list still owns) — should be safe.
  But the FOREACH loop in main uses the SAME variable name `s` and
  the SAME stack slot. If anywhere we accidentally release-on-reassign
  inside the foreach, we'd release a list-owned sprite.
  - Verify: does NODE_FOR_STMT codegen interact with our pre-init
    walker for `s`? My walker only collects NODE_VAR_DECL with owned
    init source; foreach uses NODE_FOR_STMT, so it shouldn't be
    pre-collected. But the foreach's `alloc8 %v.s` would re-allocate
    a slot that we already pre-allocated; QBE might or might not
    accept that.
- **Pre-init walker descends into `if`/`while`/`for`/`match`.** What
  about lambdas, defer blocks, unsafe blocks (handled), inner
  expressions (NOT walked: e.g., a `if x: y = SHAPE_LIT()` would be
  found, but `if expr_with_shape_lit_inside`... wait, expressions
  can't contain var-decls in MIX, so this should be fine).
- **Release-on-reassign for foreach loop var?** Foreach doesn't go
  through NODE_VAR_DECL, so my var-decl tracking doesn't see it. Each
  iter just stores the next ptr. No retain, no release. Borrowed
  semantics; should be fine.

### Other suspicious areas to verify

- **Method call args.** `balls.update!(dt)` is a method call. `dt` is
  float — not shape. Skipped. Good.
- **`balls.draw(g)`.** g is a NODE_IDENT (Game variable). Not owned.
  Skipped. Good.
- **`balls.sprites`** is a list field load. Returns the list ptr.
  No release. Good.
- **`SDL_GPUTextureSamplerBinding` per segment in `end_frame`.** This
  is the per-iteration leak that Phase 3.5 reassign-release was
  supposed to close. Verify with QBE IR inspection.

---

## Concrete next steps for the second agent

1. **Inspect the QBE IR for end_frame.** Run:
   ```sh
   cd /Users/varomix/SynologyDrive/DEV/MIX_DEV/mixel/examples/features/02_groups
   /Users/varomix/SynologyDrive/DEV/MIX_DEV/mix_lang/build/mix build --emit-ir -o /tmp/02g.ssa main.mix
   grep -A 200 "function .* \$end_frame" /tmp/02g.ssa
   ```
   Look at: are SHAPE_LIT temps (clear_color, ct, src, dst, vbinding,
   ibinding, tsbinding) actually getting `mix_release` calls? Is the
   per-segment `tsbinding` being released on each iteration of the
   while loop?

2. **Reproduce the use-after-free.** Build 02_groups, run, watch
   for unstable physics:
   ```sh
   cd .../mixel/examples/features/02_groups && ./main
   ```
   If balls accelerate, set a breakpoint on `mix_release` and check
   what's getting freed mid-loop.

3. **Add an MallocScribble check.** macOS supports `MallocScribble=1`
   env var to fill freed memory with `0x55`. If the speed-spike
   correlates with a sprite's bytes becoming 0x55..., it's
   confirmed use-after-free.
   ```sh
   MallocScribble=1 ./main
   ```

4. **Test isolation.** Apply each Phase 3.5 commit independently and
   run 02_groups after each — find which one introduces the
   acceleration bug.

5. **Question whether the SDL leak is even MIX's problem.** If (a)
   from Symptom 1 is right, no amount of MIX refcount will help.
   Try wrapping the main loop body in a manual @autoreleasepool via
   a tiny C shim and see if RAM stabilizes. If it does, the leak
   is SDL/Metal autoreleased objects, not MIX shapes.

---

## Files of interest

```
mix_lang/lib/runtime.c                   # MixHeader, mix_shape_alloc, list ops
mix_lang/src/qbe_emit.c                  # All the emit* functions
mix_lang/src/c_emit.c                    # C backend (lagging behind QBE for scope-exit)
mix_lang/REFCOUNT_PLAN.md                # Full phased plan
mix_lang/tests/programs/086_*.mix        # Phase 1 test
mix_lang/tests/programs/087_*.mix        # Phase 2 test (200 alloc / 199 free)
mix_lang/tests/programs/088_*.mix        # Phase 3 test (101 alloc / 101 free)

mixel/mixel.mix                          # The framework — see end_frame line ~1314,
                                         # init_game line ~697, draw_quad_uv_tex
mixel/examples/features/02_groups/main.mix  # Simplest leak repro
mixel/examples/arcade/asteroids/main.mix    # More complex, also crashes
```

## What we DON'T know

- Whether the SDL3 GPU API on macOS 26.4.1 has its own autorelease
  leak we can't fix from MIX. macOS 26.4.1 is brand-new (April 2026).
- Whether the reassign-release is correctly sequenced when the new
  value is computed from the old (e.g., `s = transform(s)`). Probably
  broken: emit_expr would load s, then we release s, then we store
  the result of transform into s — but transform held a ptr to the
  freed sprite during its execution.
- Whether nested function calls with shape-typed args correctly
  release in the right order. We release left-to-right after the
  outer call returns, but if intermediate temps are still in flight
  the order matters.

This is a partially-debugged refcount system. Unit tests pass but
real workload exposes both an unclosed leak vector AND an over-
release somewhere. We need somebody to either spot the bug or tell
us the design is wrong.
