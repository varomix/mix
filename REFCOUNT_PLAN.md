# MIX — Shape Refcount Memory Management Plan

> Historical note: this plan is superseded. MIX no longer uses per-shape
> refcounting as its primary model. The implemented direction is
> value-first shapes plus explicit `ref` / `ref!`, `Box[T]`, `Zone`, and
> allocator-backed collections. See `docs/DOCS.md` and
> `ZONES_VALUE_FIRST_DESIGN_SKETCH.md`.

> Created: 2026-04-26. Drives the fix for MIX's pervasive shape-allocation
> leak that surfaced as crashes in mixel demos after ~30 seconds of play.

## Context

Every `Shape(field: ...)` literal in MIX calls `mix_alloc()` and is never
freed. There is no GC, no refcount, and no destructor. List `pop!()`,
`group_clear`, scope exits — none of them release shape memory.

This is acknowledged in the codegen itself
([`qbe_emit.c:1769-1774`](src/qbe_emit.c)):

> "Heap allocation guarantees fresh memory per construction; the tradeoff
> is that SHAPE_LIT now leaks (same as the C backend)."

### Symptom

Mixel's per-frame work constructs ~10 SDL shapes (`SDL_FColor`,
`SDL_GPUColorTargetInfo`, `SDL_GPUBufferBinding`, `SDL_GPUTextureSamplerBinding`,
…) plus user-side `Vec2`/`Color`/etc. At 60-120fps these accumulate fast.
After ~30 seconds, malloc fragmentation gets bad enough to corrupt CFRunLoop
internals; the next `_CFAutoreleasePoolPop` recurses into garbage and
crashes with a "stack guard" SIGSEGV inside `pump()`.

`02_groups` (100 bouncing balls, no asteroid logic) reproduces the leak
visibly in Activity Monitor — RAM grows steadily. Every mixel demo has
this, just hidden by short play sessions.

### Why we picked refcount over alternatives

Considered three options:

1. **Stack-allocate non-escaping shape literals** (escape analysis + QBE
   `alloc8`). Fastest fix, but only catches non-escaping literals. Sprites
   in groups, tweens in lists, etc. still leak. ~80% solution.
2. **Auto-free at scope exit for non-escaping locals.** Same coverage as
   #1; simpler codegen, slower runtime (heap alloc/free instead of stack).
3. **Refcounting on shapes.** Closes the entire leak. Sprites in groups
   are freed when groups clear. ~100% solution. Pervasive instrumentation.

User decision: **#3.** "If the base which is mix has issues everything
has issues, let's fix the root of the issue."

---

## Design

### Memory layout

Every shape allocation gets a 16-byte header before the user pointer:

```
[refcount: i64][release_fn: ptr]  [user fields...]
                                   ^
                                   pointer returned to MIX code
```

`mix_alloc` actually allocates `size + 16`, writes the header, returns
the offset pointer. `mix_release(p)` reads the header at `p - 16`.

### Per-shape release function

The compiler emits one `release_<ShapeName>(ptr)` per declared shape:

- For each shape-typed field at known offset: call that field's release
- For each list-typed field: call `mix_list_release` (which releases each
  element using its stored release-fn, then frees the buffer)
- Call `mix_free(ptr - 16)` at the end

For tagged unions, the function branches on tag at offset 0 and dispatches
to the variant-specific release path.

### Runtime API

```c
// Header is implicit; size is the user-visible shape size.
void *mix_alloc(int64_t size, void (*release_fn)(void *));

void  mix_retain(void *p);   // refcount++
void  mix_release(void *p);  // refcount--; if 0, release_fn(p); free
```

`mix_retain` and `mix_release` are no-ops on `NULL`.

### List integration

`MixList` gains a `void (*element_release_fn)(void *)` field stored at
list creation time. Operations:

| Op            | Behavior                                            |
|---------------|-----------------------------------------------------|
| `push!(x)`    | runtime calls `retain(x)` before storing            |
| `pop!()`      | returns ptr without releasing — caller takes owns   |
| `set [i] = x` | `retain(new); release(old); store new`              |
| `get [i]`     | returns ptr without retain (caller borrows)         |
| `clear`       | release every element; len = 0                       |
| list free     | release every element, then free buffer             |

### Codegen instrumentation rules

| Construct                    | Emitted action                                  |
|-----------------------------|--------------------------------------------------|
| `SHAPE_LIT`                 | alloc + memset + write fields + retain (init=1) |
| `x = y` (shape locals)      | `retain(y); release(x_old); x = y`              |
| `obj.field = x`             | `retain(x); release(obj.field_old); store`      |
| `arr[i] = x`                | runtime `mix_list_set` handles                  |
| `arr.push!(x)`              | runtime retains                                 |
| local var goes out of scope | release                                         |
| function returns shape      | caller takes ownership; caller releases         |
| function param (read-only)  | callee borrows; no retain on entry/exit         |
| function param (`mut!`)     | callee borrows; no retain                       |
| C function call             | borrow (C doesn't keep ptrs); no retain         |

### Cycles

Refcounting cannot reclaim cycles. Documented as a known limitation.
Mixel has none today; if a future construct (e.g., scene graph with
parent/child links) needs them, we revisit.

### Concurrency

Refcount is not atomic. MIX is single-threaded for now. If/when threads
arrive, the inc/dec ops switch to `__atomic_*` builtins.

---

## Phased rollout

Each phase is a self-contained commit with a regression test. We never
merge a half-broken compiler — if a phase reveals a design flaw, we stop
and reconsider before proceeding.

### Phase 1 — Runtime infrastructure + simplest case ✅
- [x] Add `MixHeader` struct (refcount, release_fn) to `lib/runtime.c`
- [x] Add `mix_shape_alloc(size, release_fn)` (kept legacy `mix_alloc`
      for raw byte buffers used by `alloc()` builtin)
- [x] Add `mix_retain(p)`, `mix_release(p)`, `mix_shape_free(p)`
- [x] Codegen: emit per-shape `release_<ShapeName>` (Phase 1 stub —
      just calls `mix_shape_free`; Phase 2 adds field recursion)
- [x] Codegen: SHAPE_LIT calls `mix_shape_alloc`. For locally-declared
      shapes, references the local release fn; for C-imported shapes,
      passes 0 (runtime falls back to `mix_shape_free`).
- [x] Both backends: QBE + C
- [x] Test `tests/programs/086_shape_refcount_phase1.mix` — verifies
      construction + field reads + cross-fn return + nested shapes
      all still work with the new header in place.
- [x] All 87 existing tests still pass (no regressions)
- [x] Mixel `02_groups` and `arcade/asteroids` build and run

**Deferred to later phases:**
- Scope-exit release for shape-typed locals — needs careful handling
  of escape-via-list-push (otherwise mixel breaks). Fits naturally
  with Phase 3 (lists retain on push, balancing the scope release).
- Reassignment release (`x = y` releases old `x`) — same reason.
- 1M-iteration RSS-flat test — leak isn't actually closed yet; deferred
  to Phase 3 when lists handle retain/release.

### Phase 2 — Inline shape fields (constructor & assign temporaries) ✅

> **Revised after reading codegen.** MIX stores shape fields **inline**
> (Vec2 lives as bytes inside Sprite, not as a separate allocation),
> matching C struct layout. This means `release_<ParentShape>` does
> *not* need to recursively free inline shape children — they're
> released with the parent. The actual leak in nested shape patterns
> comes from constructor temporaries: `Sprite(pos: Vec2(...))` allocs
> a Vec2 (refcount=1), `memcpy`s it into the Sprite's bytes, and
> never frees the temporary. Same for `obj.field = SomeShape(...)`.

- [x] Codegen: after `memcpy` of an inline-shape field source in
      SHAPE_LIT, if the source AST is owned (SHAPE_LIT or shape-returning
      function/method call), emit `mix_release(src)` to free the temp.
- [x] Codegen: same fix in field-assign codegen for inline shape fields.
- [x] Both backends: QBE + C, identical output verified.
- [x] Runtime: alloc/free counters (`mix_rc_get_alloc_count`,
      `mix_rc_get_free_count`) for accounting tests.
- [x] MIX builtins: `_mix_rc_alloc_count()` and `_mix_rc_free_count()`
      exposed via sema + both backends.
- [x] Test `tests/programs/087_shape_refcount_phase2.mix` — exact
      counter accounting: 100 nested-Sprite constructions allocate
      200 (100 Sprites + 100 Vec2 temps) and free 100 (the Vec2 temps);
      50 field-assignments allocate 50 and free 50.
- [x] All 88 tests pass; mixel `asteroids` still runs.

**Still leaking after Phase 2:**
- Top-level Sprite parents (no scope-exit release yet)
- Function-call shape sources used as field values (only SHAPE_LIT
  detected as owned; conservative for C-imported functions)
- Lists of shapes (Phase 3)

### Phase 3 — List integration ✅ (with limitations)
- [x] Runtime: `MixList` gains `is_shape` flag + `element_release_fn`
- [x] Runtime: `mix_list_new_shape(release_fn)` constructor for shape lists
- [x] Runtime: `mix_list_push`/`mix_list_set`/`mix_list_remove`/`mix_list_insert`
      retain/release elements when the list is shape-typed
- [x] `mix_list_pop` transfers ownership (no release, caller takes it)
- [x] Codegen (QBE + C): `LIST_LIT` for `[Shape]` types calls
      `mix_list_new_shape` with the right release_fn (or 0 for
      C-imported shapes)
- [x] Codegen: after `list.push!(SHAPE_LIT)`, release the SHAPE_LIT
      temp so the list is the sole owner (refcount=1)
- [x] Codegen: discarded owned-shape expression statements release
      (handles `infos.pop!()` discard, bare `make_sprite()` discard)
- [x] Codegen (QBE): scope-exit release for shape-typed locals whose
      init source was OWNED (SHAPE_LIT or shape-returning call). For
      borrowed sources we don't release at scope exit — that was an
      attempted optimization that double-released and crashed mixel.
- [x] Test `tests/programs/088_shape_refcount_phase3.mix` — exact
      accounting: 100 SHAPE_LITs pushed = 100 allocs / 0 frees;
      drained by pop+discard = 100 allocs / 100 frees; int lists
      bypass refcount (0 allocs).
- [x] All 89 tests pass; **all 29 mixel demos build**; 02_groups +
      asteroids run.

**Phase 3 limitations** (deferred to Phase 3.5):
- Shape-typed locals initialized from BORROWED sources (variable copy,
  field load, list index, foreach var) are not tracked → still leak.
  Adding retain-on-borrow + release-on-exit naively breaks foreach
  because `for s in list.sprites` would retain on every iteration.
  Proper fix needs scope tracking (release at end-of-block for loop
  vars, function exit for top-level vars).
- Mid-function `return` statements skip scope-exit release →
  shape locals declared along the way leak. Need a single exit label.
- Reassignment of shape locals (`s = new_shape`) leaks the previous
  value. Needs `release(old); store(new)` instrumentation.
- Function call args that are owned-shape sources (`f(SHAPE_LIT)`,
  `f(make_x())`) are not released after the call returns. The
  callee borrows; the temp leaks. Most-impactful case: `make_sprite(...,
  make_color(...))` leaks the Color. Easy to add: release each owned-
  shape arg after every CALL_EXPR / METHOD_CALL.

### Phase 4 — Tagged unions
- [ ] Codegen: `release_<ShapeName>` for tagged union dispatches on tag
- [ ] Test: tagged union with shape-typed variants; construct/release each

### Phase 5 — Both backends to parity
- [ ] Verify QBE and C backends produce identical refcount behavior
- [ ] Run full `make test-all`; everything still passes

### Phase 6 — Mixel sweep
- [ ] Build all 28 mixel demos; no compile regressions
- [ ] Run `02_groups` for 5+ minutes; RSS stays flat in Activity Monitor
- [ ] Run `asteroids` for 5+ minutes; no crash

### Phase 7 — Regression + spec
- [ ] Add `tests/programs/NNN_shape_refcount_*.mix` covering each phase
- [ ] Update `MIX-spec_v01.md` with the memory model: "shapes are
      reference-counted; cycles are unreclaimed"
- [ ] Update `notes.md` and `~/.claude/.../memory/project_mix_known_limits.md`
      to reflect the leak fix

---

## Estimated effort

- Phase 1: 3-4 hours (runtime + simplest codegen + test)
- Phase 2: 2-3 hours (recursive release; field assign instrumentation)
- Phase 3: 4-5 hours (list integration is the trickiest bit)
- Phase 4: 1-2 hours (tagged union dispatch)
- Phase 5: 1-2 hours (backend parity check)
- Phase 6: 1-2 hours (mixel sweep)
- Phase 7: 1-2 hours (tests + doc)

**Total: 13-20 hours, ~2-4 days of focused work.**

---

## Status

- 2026-04-26: Plan written.
- 2026-04-26: Phase 1 complete. Infrastructure landed: runtime header,
  per-shape release fns, SHAPE_LIT routes through `mix_shape_alloc` on
  both backends. 86 → 87 tests passing. Mixel demos still run. **No
  behavior change for users yet** — leak is unchanged because we
  haven't wired up scope-exit / reassignment / list release. The header
  is now in place and ready for Phases 2-3 to start releasing.
- 2026-04-26: Phase 2 complete. Inline-shape constructor + field-assign
  temporaries are now released. Added `_mix_rc_alloc_count` /
  `_mix_rc_free_count` builtins for tests. 87 → 88 tests passing
  with exact alloc/free accounting. **First real leak fix landed:**
  patterns like `Sprite(pos: Vec2(...))` no longer leak the inline
  Vec2 temp. Asteroids still runs.
- 2026-04-26: Phase 3 complete. Shape lists retain on push, transfer
  on pop, release on remove. Discarded owned-shape expression results
  released. Conservative scope-exit release for shape locals whose
  init source was OWNED (SHAPE_LIT / shape-returning call). 88 → 89
  tests passing. **All 29 mixel demos build; 02_groups + asteroids
  run.** Phase 3.5 will close the remaining gaps (borrowed-source
  scope-exit, mid-function returns, reassignment, call-arg release).
