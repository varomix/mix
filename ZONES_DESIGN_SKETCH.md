# MIX — Zone-based Memory Design Sketch

> Historical note: this document captures the first zone-only pivot idea.
> It is superseded by the value-first design in
> `ZONES_VALUE_FIRST_DESIGN_SKETCH.md` and by the implemented compiler
> behavior. MIX does not use the "everything allocates in the current
> zone" model described here.

> Status: 2026-04-26. Exploratory design — not committed to. Written to
> evaluate whether moving from refcount to zones is the right call for
> MIX, given its game/graphics positioning.

## TL;DR

Replace per-shape refcounting with a small set of **arena zones** that
the program owns and resets. 95% of game allocations have obvious
lifetimes (per-frame, per-level, per-program) — make those lifetimes
first-class.

```mix
zone _frame  ~  reset every frame, contains scratch SHAPE_LITs
zone _long   ~  program lifetime, contains entities + assets
zone _init   ~  setup-only, freed after init_game returns
```

Per-frame work auto-targets `_frame`; one O(1) bump-pointer reset at
end-of-frame frees everything in one shot. No retain, no release, no
release_fn pointer per shape, no header overhead.

---

## Why the change is worth considering

After implementing refcount we got a **100x leak reduction** but:

- Every shape allocation pays 16 bytes of header overhead.
- Every assignment / arg-pass / scope-exit pays a `mix_retain` or
  `mix_release` call.
- Codegen now has 6+ instrumentation rules (var-decl, assign,
  field-assign, push, pop-discard, call-arg, scope-exit) and we still
  found cases where they interact wrong.
- We will hit the cycle problem the moment someone writes a scene
  graph with parent/child links.
- The remaining ~100 bytes/frame is SDL/Cocoa autoreleased objects
  we can't fix from MIX side.

Zones throw out all of that complexity for the common case. They cost
the programmer a small amount of thinking ("which zone does this go
in?") in exchange for **zero runtime overhead, zero leaks by
construction**, and a model that maps directly to how games actually
think about memory.

This isn't speculative — it's the consensus that Zig, Odin, Jai, and
the Handmade-Hero school of game dev have all landed on independently.

---

## Core concepts

### A zone is a bump-pointer arena

```c
typedef struct {
    char  *base;         // start of backing memory
    char  *end;          // base + capacity
    char  *cursor;       // next free byte
    char  *high_water;   // optional — peak usage for diagnostics
} MixZone;

void *mix_zone_alloc(MixZone *z, int64_t size, int64_t align);
void  mix_zone_reset(MixZone *z);     // cursor = base
void  mix_zone_destroy(MixZone *z);   // free(z->base)
```

Allocate is bump-the-cursor. Reset is one assignment. No per-allocation
bookkeeping.

### Three built-in zones

| Zone     | Lifetime                              | Reset by         |
|----------|---------------------------------------|------------------|
| `_long`  | program duration                      | `destroy_game`   |
| `_frame` | one render frame                      | `end_frame`      |
| `_init`  | between `init_game` start and end     | `init_game`      |

Plus user can declare their own:
```mix
@zone level_zone(8 * 1024 * 1024)   // 8MB for a level
```

### Default zone is lexically scoped

Every allocation goes to a "current" zone. Setting it is block-scoped.

```mix
main() ~
    g = init_game(...)            // current = _long, so g lives in _long

    sprites! = make_group()       // also _long
    for i in 0..100
        s = make_sprite(...)      // also _long
        group_add(sprites, s)

    while not pump(g)
        in zone _frame
            color = cornflower()  // → _frame
            begin_frame(g, color) // arg already in _frame
            sprites.draw(g)
            end_frame(g)
        zone_reset(_frame)        // O(1)
```

The `in zone X` block:
- Saves the previous current-zone
- Sets X as current for the body
- Restores on exit

It does NOT auto-reset; reset is explicit, because reset is what
defines lifetime. (Optional sugar: `frame zone X { ... }` could mean
"in zone X + reset on exit" if we want.)

### Per-allocation override with `@`

If you're in `_frame` but need one specific value to outlive the
frame, escape with `@`:

```mix
in zone _frame
    color = cornflower()                       // _frame
    forever_color = make_color(...) @ _long    // _long, escapes
```

This is the explicit ownership-transfer marker. Three-character cost.

### Library functions accept "the current zone" implicitly

`make_sprite`, `Vec2(...)`, `[Sprite]`, etc. don't take a zone
parameter. They allocate from whatever the caller's current zone is.
This keeps API surface clean.

If a library function NEEDS to override (e.g., `init_game` always
allocates the Game in `_long` regardless of caller context), it does
so explicitly:

```mix
pub init_game(...) -> Game ~
    in zone _long                             // override caller
        Game(...)                             // always _long
```

---

## What the codegen actually emits

### Without zones (today, after refcount)

```
ct = SDL_GPUColorTargetInfo(...)
# emits:
%t = call $mix_shape_alloc(l 64, l $release_SDL_GPUColorTargetInfo)
... fill fields ...
storel %t, %v.ct__v_<hash>
# (later, at scope exit:)
%old = loadl %v.ct__v_<hash>
call $mix_release(l %old)
```

### With zones

```
ct = SDL_GPUColorTargetInfo(...)
# emits:
%t = call $mix_zone_alloc(l <current_zone_ptr>, l 64, l 8)
... fill fields ...
storel %t, %v.ct
# (no scope-exit work — zone reset handles it)
```

The zone pointer is a thread-local global (`mix_current_zone`) updated
by `in zone X` blocks. Codegen does:

```
# entering `in zone _frame`
%t1 = loadl $mix_current_zone
%t2 = loadl $g_zone__frame
storel %t2, $mix_current_zone
... body ...
# exit:
storel %t1, $mix_current_zone
```

That's the entire instrumentation. Compare to the 6 rules of refcount.

---

## The hard cases

### Mutable shared ownership

If two long-lived containers need to share a value, both need a
pointer to it AND it must outlive both. With zones:

```mix
in zone _long
    sprite = make_sprite(...)
    primary_group.add(sprite)
    layer_index.put(sprite.id, sprite)
```

Both lists hold raw pointers to the same `_long`-allocated sprite. No
refcount needed because `_long` doesn't get reset until program exit.

If you remove from one list, the other still has it — no use-after-
free because nothing got freed.

If you "really" want to drop the sprite mid-game (level transition):
- Either destroy the level zone (frees everything in it)
- Or just clear() the lists; the sprite memory leaks until the zone
  resets, which is fine for game semantics

### Pointers escaping their zone (footgun)

```mix
saved! = make_group()    // _long

while not pump(g)
    in zone _frame
        s = make_sprite(...)
        group_add(saved, s)   // BUG — `saved` lives forever, `s` dies
    zone_reset(_frame)         // s is now garbage; saved holds dangling ptr
```

Options for handling:

**Option A — programmer's responsibility (Zig/Odin/Jai approach):**
Document, don't enforce. The mental model is "if it escapes, allocate
in the right zone." Casey Muratori et al would call this fine.

**Option B — runtime check via generational pointers:**
Each zone-reset bumps a generation counter; pointers carry the
generation; deref panics if stale. Cheap (~1 cycle/access). Catches
the bug at first deref instead of in production.

**Option C — compile-time region inference:**
Track zone lifetime in the type system. Reject if a longer-lived
container holds a shorter-lived value. Powerful, complex (this is
Rust-with-extra-steps).

Recommend **A first**, add **B** if it bites in practice. Skip C —
fights the "Python clarity" goal.

### `done` / mid-function returns

Default-zone restoration uses RAII pattern internally — if a function
exits early, the saved zone pointer is restored automatically.

Codegen for `in zone X` block: emit save/restore around the body, AND
emit restore at every `done`/`fail`/return-from-this-block point. The
existing scope-exit infrastructure (the agent's
`emit_function_epilogue`) generalizes to this — same single-exit
discipline.

### Library composition

Function authors should think about which zone their results belong in:
- "Construct and return" functions (`make_sprite`, `Vec2(...)`)?
  Result lives wherever the caller's current zone is. Default behavior.
- "Long-lived" allocators (`init_game`, `load_texture`)? Override with
  `in zone _long`.
- "Per-frame" helpers (`begin_frame`)? Override with `in zone _frame`
  internally so callers don't have to remember.

This becomes part of API documentation: "this function allocates in
_long" / "this function allocates in caller's current zone".

---

## What we'd rip out

If we fully migrate:
- `MixHeader` (16 bytes/shape)
- `mix_shape_alloc`, `mix_retain`, `mix_release`, `mix_shape_free`
- Per-shape `release_<Name>` functions in codegen
- The 6 codegen instrumentation rules (var-decl release-on-reassign,
  field-assign source-release, list-push source-release, expr-stmt
  discard-release, call-arg release, scope-exit release)
- Pre-walker for shape locals + zero-init
- The shape-aware list machinery (`is_shape` flag, push/pop/set
  retain/release on `MixList`)

What we keep:
- Per-shape struct layout (no header changes)
- All the lifetime knowledge encoded in 89-91 tests
- The `_mix_rc_alloc_count` instrumentation (rename it
  `_mix_zone_*_count` and use it to verify zone resets actually free)

---

## Migration plan (if we pivot)

Roughly 4 phases, each ~half-day to a day:

### Zones-1: Runtime + the three built-ins
- Implement `MixZone`, `mix_zone_alloc`, `mix_zone_reset`, etc.
- Define `_long`, `_frame`, `_init` as global zones initialized at
  program start.
- Thread-local `mix_current_zone` defaults to `_long`.

### Zones-2: Codegen for SHAPE_LIT routing
- Replace `mix_shape_alloc` calls with `mix_zone_alloc` against
  current-zone pointer.
- Emit `in zone X` block as save/restore of `mix_current_zone`.
- Emit `expr @ X` as scoped override for one allocation.
- Drop the release-fn argument; drop per-shape `release_<Name>`
  emission.
- Drop scope-exit release calls.

### Zones-3: Strip refcount machinery
- Remove `MixHeader`, retain/release runtime.
- Remove all six codegen instrumentation paths.
- Remove the pre-walker.
- Lists go back to plain `mix_list_new` (no `is_shape` flag).

### Zones-4: Mixel adoption
- `init_game`: wrap body in `in zone _init` for scratch + return Game
  in `_long`. After init, reset `_init`.
- `begin_frame`: enter `in zone _frame`.
- `end_frame`: exit `in zone _frame`, then `zone_reset(_frame)`.
- Long-lived containers (groups, atlases, tilemaps): `in zone _long`
  at construction site.
- Verify all 29 demos still work.

After phase 4, 02_groups should leak 0 bytes/frame from MIX's side.
The remaining ~100 bytes/frame from SDL/Metal would also drop because
it's mostly autoreleased Cocoa objects piling up between drains —
eliminating MIX's heap pressure lets the autorelease pool drain
faster.

---

## Tradeoffs honestly

### What we lose

- **Convenience for shared ownership:** today you can do
  `list1.push(x); list2.push(x)` without thinking. With zones, you
  think "what zone holds these lists, and is `x` allocated in a zone
  that lives at least as long?" 
- **Refcount auto-cleanup of intermediate values:** today
  `s = make_sprite(...)` followed by `s = make_sprite(...)` frees the
  first one automatically. With zones, the first sprite is "leaked"
  until the zone resets — fine for `_frame` (resets every frame),
  could be wasteful for long-running zones if a hot loop builds up
  intermediates.
- **Refcount handles cycles correctly given weak/strong:** zones don't
  care about cycles at all (just live until reset), but you also
  can't selectively free one node of a graph.

### What we gain

- **Zero-overhead allocation:** bump pointer.
- **Zero-overhead reset:** one pointer write.
- **No leak by construction:** zone reset frees everything.
- **No use-after-free within a zone:** all allocations equally valid
  until reset.
- **Predictable performance:** no GC pauses, no refcount cascades on
  release of containers.
- **Smaller compiler:** ~1000 lines of refcount codegen vs ~100 lines
  of zone codegen.
- **Smaller runtime:** no header overhead per allocation.
- **Better fit for games:** matches how engines have been written for
  decades.
- **MIX-spec_v01.md already mentions zones:** we'd be implementing
  the language as designed, not bolting on a new model.

---

## Open questions

1. **Syntax for zone declaration / blocks.** `zone X { ... }` ?
   `in zone X: ...` ? `with X: ...` ?
2. **Implicit @ marker vs explicit `pin`/`copy_to`.**
3. **Should `_frame` reset be automatic on `end_frame()` or should the
   user opt in?**
4. **What about heap-allocated lists/maps/strings — do they go through
   zones too?** (Probably yes, otherwise they're a separate leaky path
   we have to track.)
5. **Threading.** `mix_current_zone` thread-local — fine. But two
   threads sharing a zone need locks. For mixel's single-threaded
   model, no issue.
6. **Existing library code that constructs shapes for return.** Most
   would just inherit caller's zone, which is correct by default.
   Some (`init_game`, `load_texture`) need explicit `_long`.

---

## My recommendation

This is a real strategic question. I'd suggest:

1. **Don't rip out the refcount work yet.** It's currently doing real
   good (100x improvement) and the unit tests + counters are valuable.
2. **Build a tiny prototype**: implement a `MixZone` runtime, a
   `mix_zone_alloc` builtin, and rewrite `02_groups` to use it
   manually (no language changes yet — just call the runtime
   directly). See if the model feels right.
3. **If yes**, then design the syntax (`in zone`, `@`, etc.) and do
   Zones-2 through Zones-4.
4. **If no** (zones turn out to fight some MIX feature we haven't
   thought of), keep refcount and look at other improvements.

This protects the current improvement while letting us test the
hypothesis. The full migration is ~4 days of focused work; the
prototype is half a day.

The bigger question this raises: **MIX wants to be a games-first
systems language, and games have memory patterns that refcount/GC
were designed around but not for.** Zones aren't novel — they're the
right answer for this domain that nobody has put into a Python-clean
syntax yet. There's room here to do something distinctive.
