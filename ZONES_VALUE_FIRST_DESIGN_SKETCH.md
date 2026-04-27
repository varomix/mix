# MIX — Value Shapes + Zones + Explicit Promotion

> Status: 2026-04-26. Exploratory design sketch. This is the recommended
> direction if MIX is optimizing for games/graphics with C-like
> performance and a predictable cost model.

## TL;DR

MIX should not treat every `shape` as a heap object, whether that heap is
refcounted or "whatever the current zone is."

The preferred model is:

- `shape` is a **value type** by default.
- `zone` is a first-class **allocator / arena** for dynamic storage.
- Escaping data from a shorter-lived allocator to a longer-lived owner
  requires **explicit promotion**.
- APIs that allocate dynamic storage should prefer **explicit allocator
  arguments** so lifetime is visible at the call site.

This keeps small math/graphics data cheap and obvious, while still giving
the language strong support for frame allocators, level allocators, and
bulk-reset workflows.

This sketch explicitly rejects two extremes:

- **Heap + refcount for every shape**: too much hidden runtime overhead
  and too much compiler/runtime complexity in hot paths.
- **Every shape allocates in the current zone**: too implicit, too easy
  to create dangling-lifetime bugs, and too far from the value-like way
  MIX already uses shapes such as `Vec2`, `Color`, SDL structs, and config
  records.

## Why This Fits MIX

MIX is drifting toward the needs of games, graphics, tooling, and C
interop. In that environment, the most important property is not
"automatic ownership everywhere." It is:

- low hidden cost
- easy reasoning about layout and lifetime
- strong bulk-allocation tools
- no surprise retain/release traffic in the render loop

Small aggregates like vectors, colors, rectangles, transforms, and plain
config structs want value semantics. They should behave like structs, not
objects.

At the same time, engines and real-time tools absolutely do want arenas:

- frame scratch
- transient render command buffers
- level-owned data
- asset caches
- parser/builder scratch space

So the right split is:

- **values** for ordinary shapes
- **zones** for dynamic storage with obvious bulk lifetimes
- **boxed / ref forms** for shared identity and aliasing

That is closer to C, Zig, Odin, and the parts of Rust that people reach
for in engine code than to ARC/GC-style object systems.

## Core Model

### 1. `shape` means value semantics

At the language level, a `shape` is a value:

- locals hold values
- parameters receive values
- returns produce values
- fields inline other values by default

The compiler is free to optimize placement:

- registers
- stack
- inline storage inside larger aggregates
- hidden heap escape if MIX ever chooses to support that as an internal
  optimization

But that is an implementation detail. The programmer-facing meaning of
`shape` is "plain value data," not "heap object with lifetime machinery."

### 2. `zone` is an allocator, not the meaning of every shape

A zone is an arena handle used by things that actually need dynamic
storage:

- lists
- maps
- sets
- strings / string builders
- buffers
- `Box[T]` allocations
- dynamic graph structures

Zones should be explicit runtime values. Engines may expose conventional
zones such as frame, level, and long-lived allocators, but those are
runtime/library conventions, not mandatory language primitives.

Core MIX should ship the `Zone` type plus allocator primitives such as
`zone_create`, `zone_reset`, and diagnostic builtins. It should not bake
magic names like `_frame`, `_long`, or `_init` into the language. A game
framework such as `mixel` can then expose conventional zones explicitly,
for example as `g.frame_zone` and `g.long_zone`.

### 3. Shared identity is opt-in

If a program wants aliasing or shared identity, it should opt into a
separate indirection form. This sketch chooses a concrete split:

- `T` is a plain value type.
- `ref T` is a shared borrowed alias. `ref! T` is a mutable borrowed alias.
  Borrows do not own storage and cannot escape the borrow site.
- `Box[T]` is an owning stable-address allocation in a zone.
- `*T` remains an unsafe raw pointer for FFI and low-level code.

Syntax choice for this sketch:

- borrow types are spelled `ref T` and `ref! T`
- borrow expressions are spelled `ref x` and `ref! x`
- owning stable-address construction is spelled `box(zone, value)`

Handles are still a valid library-level pattern for ECS-style systems, but
they are not the core language default. The key distinction is that a
plain `Sprite` value, a borrowed `ref Sprite`, and an owning `Box[Sprite]`
are three different things.

### 4. Borrowing and mutation are explicit

The design must distinguish read-only access from write-through access.

- `for x in items` yields a shared borrow for the loop body. It is
  read-only.
- `for x! in items` yields a mutable borrow into the underlying element.
  Field writes and mutating method calls write through to the container.
- A non-mutating method receives `self` as a shared borrow.
- A mutating method declared with `!` receives `self!` as a mutable
  borrow into caller storage.

This is the rule that preserves the most common `mixel` pattern:
iterating a sprite list and mutating each element in place.

### 5. Collections store values inline by default

For performance and cache locality, `List[T]` and `Map[K, V]` store value
types inline by default.

- `List[Sprite]` stores `Sprite` inline, not one heap object per sprite.
- `Map[int, Sprite]` stores values inline.
- If stable address or shared identity is required, the type becomes
  explicit: `List[Box[Sprite]]`, `Map[int, Box[Sprite]]`, and so on.

This implies an explicit distinction between reading an element and
borrowing it mutably:

- `x = items[i]` reads a value.
- `x = items.at(i)` yields a shared borrow (`ref T`) without copying.
- `x! = items.at_mut!(i)` yields a mutable borrow (`ref! T`) into storage.

The exact indexing API can change, but the semantic split should remain.

Performance-sensitive code should prefer `at(i)` / `at_mut!(i)` for field
access through large inline elements. A future optimizer may legally fold
some read-only `items[i].field` cases into direct loads, but the explicit
borrow accessors are the baseline semantics.

### 6. Escapes require explicit promotion

If data allocated under one allocator must outlive that allocator, the
program must say so explicitly:

- `promote(long_zone, value_or_ref)`
- `clone_into(long_zone, value_or_ref)`

The exact name can change, but the operation must be visible in source.

MIX should not silently auto-promote frame or scratch data into long-lived
storage by default. That would damage the cost model in exactly the code
where users care most.

### 7. C interop stays value-oriented

Imported C structs should map to MIX value shapes.

- Passing a value shape to an extern that expects `*T` may auto-take its
  address if the value is addressable.
- If the C callee mutates through that pointer, MIX should require mutable
  storage such as `x!`, `for x! in items`, `items.at_mut!(i)`, or `Box[T]`.
- For `use c`, the conservative default should be that any imported `*T`
  parameter may mutate through the pointer unless the binding layer knows
  it is `const T *`. A future binder can relax this when it preserves C
  constness.
- C functions returning structs should lower to MIX value returns with the
  correct platform ABI, including `sret`-style lowering where required.

## How It Looks In Code

The syntax below is illustrative. This sketch intentionally picks one
spelling for clarity:

- `ref T` / `ref! T` for borrow types
- `ref x` / `ref! x` for explicit borrow expressions
- `box(zone, value)` for `Box[T]` construction
- `at(i)` / `at_mut!(i)` for borrowed indexed access

The exact spellings can still change, but the semantics should not.

### Plain shapes are values

```mix
shape Vec2
    x, y: float

shape Color
    r, g, b, a: float

main()
    p = Vec2(x: 10.0, y: 20.0)
    tint = Color(r: 1.0, g: 0.5, b: 0.2, a: 1.0)

    q = p
    q.x = q.x + 5.0

    print(p.x)   // 10.0
    print(q.x)   // 15.0
```

Intent:

- no hidden heap allocation implied by `Vec2(...)`
- no allocator argument needed for a pure value
- copy/update behavior stays obvious for math and graphics code

### Dynamic containers allocate from an explicit zone

```mix
shape Sprite
    pos: Vec2
    vel: Vec2
    tint: Color

main()
    frame = zone_create("frame", 2 * 1024 * 1024)
    level = zone_create("level", 16 * 1024 * 1024)

    sprites = List[Sprite].new(level)

    i! = 0
    while i < 100
        s = Sprite(
            pos: Vec2(x: to_float(i), y: 0.0),
            vel: Vec2(x: 1.0, y: 2.0),
            tint: Color(r: 1.0, g: 1.0, b: 1.0, a: 1.0),
        )
        sprites.push(s)
        i = i + 1
```

Intent:

- `Sprite(...)` is still a value
- the list is the thing that allocates dynamic storage
- allocator choice is visible where dynamic ownership begins

### Mutating methods and mutating iteration are explicit

```mix
pub shape Group
    sprites: List[Sprite]

    update(dt: float) !
        for s! in sprites
            s.update!(dt)

    draw(g: Game) ~
        for s in sprites
            s.draw(g)
```

Intent:

- mutating receivers are declared explicitly with `!`
- `for s! in sprites` means "borrow each element mutably"
- `for s in sprites` stays read-only and cannot silently mutate a copy

### Indexed mutation is explicit for inline storage

```mix
main()
    sprites = List[Sprite].new(level)

    current = sprites[0]           // reads a value copy
    view = sprites.at(0)           // ref Sprite (no copy)
    slot! = sprites.at_mut!(0)     // ref! Sprite
    if view.pos.x < 0.0
        slot.pos.x = 32.0
    slot.vel.y = -slot.vel.y
```

Intent:

- inline value storage remains cache-friendly
- mutation through containers is explicit
- pointer-like semantics only appear when the programmer asks for them
- large element field reads can avoid full-value copies through `at(i)`

### Frame scratch uses a frame zone

```mix
draw_scene(g: Game, sprites: List[Sprite], frame: Zone)
    verts = VertexBuffer.new(frame, 4096)
    cmds = DrawList.new(frame)

    for s in sprites
        cmds.add_sprite(s)
        verts.write_sprite(s)

    submit(g, cmds, verts)

main() ~
    frame = zone_create("frame", 4 * 1024 * 1024)
    level = zone_create("level", 32 * 1024 * 1024)
    sprites = load_level(level)

    while not pump(g)
        zone_reset(frame)
        draw_scene(g, sprites, frame)
```

Intent:

- frame scratch is explicit
- reset is cheap and obvious
- no hidden retain/release traffic in the hot path

### `in zone` is ergonomic sugar for local scratch

```mix
main()
    frame = zone_create("frame", 4 * 1024 * 1024)

    in zone frame
        tmp_points = List[Vec2].new(frame)
        tmp_points.push(Vec2(x: 0.0, y: 0.0))
        tmp_points.push(Vec2(x: 10.0, y: 20.0))
        rasterize(tmp_points)

    zone_reset(frame)
```

Intent:

- `in zone` is fine as local convenience
- it is not the universal semantic meaning of shape construction
- allocating APIs still surface the allocator explicitly

### Long-lived shared identity is opt-in

```mix
shape Entity
    id: int
    pos: Vec2
    health: int

main()
    long = zone_create("long", 64 * 1024 * 1024)

    e = box(long, Entity(
        id: 7,
        pos: Vec2(x: 0.0, y: 0.0),
        health: 100,
    ))

    render_list = List[Box[Entity]].new(long)
    lookup = Map[int, Box[Entity]].new(long)

    render_list.push(e)
    lookup.set(e.id, e)
```

Intent:

- aliasing/shared identity is explicit
- a boxed form is different from a plain value shape
- lists/maps of boxes are natural for engine lookup tables and scene state

### Invalid escape without promotion

```mix
main()
    frame = zone_create("frame", 1 * 1024 * 1024)
    long = zone_create("long", 32 * 1024 * 1024)

    saved = List[Box[Sprite]].new(long)

    in zone frame
        s = box(frame, Sprite(
            pos: Vec2(x: 1.0, y: 2.0),
            vel: Vec2(x: 0.0, y: 0.0),
            tint: white(),
        ))

        saved.push(s)   // invalid: frame-owned box escapes to long-lived list
```

Intent:

- a shorter-lived allocation cannot silently leak into a longer-lived
  owner
- MIX should reject this, or at minimum define it as invalid and catch it
  in debug/runtime checking if static rejection is not ready yet

### Explicit promotion to longer-lived storage

```mix
main()
    frame = zone_create("frame", 1 * 1024 * 1024)
    long = zone_create("long", 32 * 1024 * 1024)

    saved = List[Box[Sprite]].new(long)

    in zone frame
        s = box(frame, Sprite(
            pos: Vec2(x: 1.0, y: 2.0),
            vel: Vec2(x: 0.0, y: 0.0),
            tint: white(),
        ))

        keep = promote(long, s)
        saved.push(keep)
```

Intent:

- lifetime extension is visible and explicit
- no silent heap lifting
- performance-sensitive code stays auditable

## Escape And Promotion Rules

The core rule is simple:

> A value or reference allocated from a shorter-lived allocator cannot be
> stored into a longer-lived owner unless the program explicitly promotes
> or clones it into the longer-lived allocator.

Examples of longer-lived owners:

- a list or map backed by a longer-lived zone
- a field inside a boxed/ref object owned by a longer-lived zone
- a cached global or module-level structure
- data captured by a task or callback that can outlive the current scope

Recommended policy:

- **Primary rule**: explicit promotion in source
- **Compile-time rule**: `ref T` cannot escape its borrow site
- **Debug-time rule**: `Box[T]` allocated in resettable zones should carry
  allocator/generation metadata so invalid stores and stale dereferences
  can trap in debug builds
- **Avoid by default**: silent auto-promotion

This is the right compromise for MIX:

- predictable costs in shipping builds
- explicit lifetime transitions
- room for debug tooling without turning the language into a lifetime
  theorem prover

MIX v1 should not attempt full region inference. The design stays
explicit:

- borrows are non-escaping by rule
- boxes are the explicit owning indirection
- promotion is the explicit lifetime bridge
- debug checks catch misuse of resettable-zone boxes during development

Closures inherit the same rule:

- a closure that captures `ref T` or `ref! T` is itself non-escaping
- such a closure cannot be stored, returned, or passed into tasks /
  callbacks that may outlive the borrow
- escaping closures must capture values or `Box[T]`, not borrowed refs

Illustrative example:

```mix
watch(s: ref Sprite) -> fn()   // invalid if returned closure escapes `s`
    fn()
        print(s.pos.x)
```

## C Interop

This model should preserve good C interop rather than fighting it.

- Imported C structs remain value shapes.
- A MIX value can be passed by value to a C function expecting a struct.
- A MIX value can be passed to a `*T` parameter via auto-address when the
  source is addressable.
- Mutating C calls require mutable storage, not a temporary value copy.
- Inline-list storage and mutable borrows line up naturally with C-style
  APIs that want a pointer into a stable array element.

The important ABI work is backend lowering, not changing the language
memory model:

- struct return must use the correct target ABI
- pointer-taking calls must respect addressability
- `Box[T]` should lower to a stable-address pointer type

## Diagnostics And Test Metrics

The current `_mix_rc_*` counters are tied to the transitional refcount
model and should not survive as the primary test hooks.

The zone-based model should expose per-zone diagnostics such as:

- `_mix_zone_alloc_bytes(z)`
- `_mix_zone_high_water(z)`
- `_mix_zone_reset_count(z)`

This lets tests assert allocator behavior directly, for example:

- frame-zone usage returns to zero after a reset
- a frame's high-water mark stays under a known budget
- reset count advances exactly once per frame

These metrics are a better fit for allocator- and lifetime-oriented tests
than object-level retain/release counters.

## Migration And Refcount Retirement

This design is an end state, but it needs an explicit path from current
MIX.

Recommended migration shape:

- Add the new semantic vocabulary first: `ref T`, `Box[T]`, `for x! in
  items`, and mutable element access such as `at_mut!`.
- Move list/map storage for value types to inline layout.
- Port `mixel` and the demos to explicit mutable iteration and explicit
  boxed indirection where stable identity is truly needed.
- Keep tests and demos green at each stage rather than attempting a
  flag-day rewrite.

What happens to the current refcount work should be stated plainly:

- Per-shape refcounting is transitional scaffolding, not the target
  semantics of `shape`.
- Once value-first shapes and allocator-backed dynamic storage are in
  place, refcount-based shape lowering should be removed from the default
  path.
- Refcount-oriented diagnostics and tests should be replaced by allocator
  and zone diagnostics, such as zone usage / reset checks and borrow /
  promotion regressions.

## Comparison With Alternatives

### Heap + refcount by default

Pros:

- uniform runtime model
- easy to express shared graphs

Cons:

- per-object header overhead
- retain/release traffic in hot paths
- difficult codegen and edge cases
- cycle problem appears as soon as users build graph-like data
- small value-like shapes pay object-like costs

For MIX's target domain, this is the wrong default.

### Current-zone allocation for every shape

Pros:

- fast arena allocation
- simple bulk reset story

Cons:

- makes every shape implicitly pointer/lifetime-carrying
- hides the real boundary between value semantics and dynamic storage
- makes escaping references easy to get wrong
- overuses allocator context for data that should just be plain values

This is better than universal refcount for some workloads, but still too
implicit as the core semantics.

### Full region inference / lifetime typing

Pros:

- strongest safety story
- could reject invalid escapes statically

Cons:

- high compiler complexity
- pushes MIX toward Rust-like lifetime machinery
- not aligned with the language's desired feel

This may be worth borrowing ideas from later, but it should not be the
starting point.

## Recommended Direction

The preferred memory model for MIX is:

- **value-first shapes**
- **explicit zones for dynamic storage**
- **explicit boxed/reference forms for shared identity**
- **explicit promotion for lifetime extension**
- **explicit allocator parameters in allocating APIs**

This model gives MIX the performance profile and mental model that fit
games and graphics:

- small structs stay cheap
- frame scratch is natural
- long-lived storage is deliberate
- hidden runtime overhead stays low
- lifetime transitions remain visible in source

In short:

`shape` should feel like a struct. `zone` should feel like an allocator.
`box`/`ref` should feel like opting into shared identity. `promote`
should be the explicit bridge between lifetimes.
