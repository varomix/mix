# MIX bug — `for x in self.field` over inline shape list crashes / corrupts stack

> 2026-04-27. Discovered while diagnosing "framerate broken" report on
> mixel. This is the actual root cause; the `Fix macOS frame memory
> growth` SDL changes are red herrings.

## Symptom

mixel demos lock up immediately after launch. Window opens; nothing
renders or animates. Have to force-kill. SDL settings (`SDL_SetGPU*`,
`SDL_WaitAndAcquireGPUSwapchainTexture`, etc.) are unrelated — the
hang reproduces even after reverting all of them.

## Minimal reproducer

```mix
shape Big
    a: float
    b: float
    c: float
    d: float
    e: float
    f: float
    g: float
    h: float
    width: float

    show(prefix: str) ~
        print(prefix + " width=" + to_string(to_int(self.width)))

shape Container
    items: [Big]

    each(prefix: str) ~
        for it in self.items
            it.show(prefix)

main()
    c! = Container(items: [])
    c.items.push!(Big(a: 1.0, b: 2.0, c: 3.0, d: 4.0,
                      e: 5.0, f: 6.0, g: 7.0, h: 8.0, width: 999.0))
    c.each("loop")
```

Run: **SEGFAULT (exit 139).**

This is exactly the pattern in `mixel/mixel.mix:389`:
```mix
pub shape Group
    sprites: [Sprite]

    update(dt: float) !
        for s! in self.sprites
            s.update!(dt)

    draw(g: Game) ~
        for s in self.sprites
            s.draw(g)
```

## Diagnostic trail

1. **mixel demos hang on first frame.** Tracking with file-write
   probes inside the loop shows `balls.draw(g)` is the first call
   that doesn't return.
2. **`balls.draw(g)` with empty group works**; with 1+ sprite it
   hangs. So the foreach body executing is what triggers it.
3. **Inline foreach in user code works.** Writing
   `for s in balls.sprites: s.draw(g)` at the call site (NOT inside
   `Group.draw` method) doesn't crash.
4. **Memory-corruption smoking gun:** instrumented the loop with a
   counter `iter` and printed it before/after each step. After
   `balls.draw(g)`, `iter` jumps to `4620693217682128896`.
   - `4620693217682128896` = `0x4020000000000000`
   - As an IEEE-754 double, that bit pattern is `8.0`
   - `8.0` is the sprite's `width` field
   - So `iter`'s stack slot got overwritten with a sprite field value
5. **Minimum reproducer (above) crashes the same way**, no SDL
   involved. Container has a `[Big]` field, method iterates it,
   reads a single field of the borrowed element → SEGFAULT.
6. **Plain (non-method, non-self.field) version works.** Exact same
   shape list iterated at top-level main works fine. Only the
   "iterating a shape-list field of self" path is broken.

## Suspected location

`src/qbe_emit.c`, the `NODE_FOR_STMT` path for inline shape lists
when the iterable is a `NODE_FIELD_EXPR` on `self`. The codegen
likely:

- Loads `self.items` and treats the result as a pointer to a value
  rather than a pointer to a list.
- Or computes the loop's stride / length using the wrong type info
  when the list comes through a method-receiver field load.
- Or aliases the loop var slot with the receiver's frame storage.

The corruption pattern (stack-adjacent locals overwritten with what
looks like inline shape bytes) suggests the loop body's element
binding writes `elem_size` bytes through a pointer that aliases
caller-stack instead of the list's backing buffer.

## Why mixel works in some configurations

- Empty groups don't trigger the bad codegen (loop body never runs).
- Inline iteration at the call site bypasses the broken
  field-of-self path.
- Tests like `074_for_shape_list` may exercise a different path
  (shape list as a local, not as a field).

## Suggested next steps

1. **Triage in the compiler.** Check the foreach codegen for the
   case `iterable.kind == NODE_FIELD_EXPR && iterable.object is
   self && iterable.field.type is TYPE_LIST && elem.kind ==
   TYPE_SHAPE && list.is_inline`.
2. **Add `091_foreach_self_field_inline_list.mix`** with the
   minimal reproducer above as a regression test.
3. **Workaround in mixel** (so demos run while compiler is
   fixed): inline the `Group.update`/`Group.draw` bodies at the
   call site. Ugly but unblocks ports until the compiler fix lands.

## Files of interest

- `mix_lang/src/qbe_emit.c` — `case NODE_FOR_STMT:`, the `is_list`
  branch around the shape-list-with-elem-size path.
- `mix_lang/lib/runtime.c` — `mix_list_*` value-list ops (these are
  fine; the issue is on the codegen side).
- `mixel/mixel.mix:382-391` — `Group.update` and `Group.draw` are the
  current trigger sites in mixel.

## Status of investigation

- Hypothesis confirmed: codegen bug, not SDL.
- Repro is small (~20 lines) and deterministic.
- Have not yet looked at the QBE IR diff between working
  (top-level foreach) and broken (method foreach over self.field)
  versions; that's the obvious next debug step.
