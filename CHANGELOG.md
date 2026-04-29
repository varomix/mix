# MIX LLVM Migration Changelog

> Persistent progress log for the LLVM migration described in
> `LLVM_MIGRATION_PLAN.md`. Append-only. Newest entries at the top of each
> section. Do not rewrite history here — record what happened, including
> dead ends.

## Current Status

- **Active phase:** Phase 6 done. **LLVM at 107/107 main + 31/31
  mixel.** Full parity with QBE on every test. The three remaining
  features that previously gated QBE removal (025 tagged unions,
  035 shared, 036 go/wait) all now lower correctly under LLVM.
- **Default backend:** LLVM. QBE is still selectable via
  `--backend qbe` and stays as a parity oracle until you decide
  to delete it. All three deletion criteria are now met:
    1. ✅ 025 / 035 / 036 implemented and passing on LLVM
    2. ✅ Mixel still 31/31 on LLVM
    3. ✅ No QBE-only bug surfaced
  Pulling the trigger is a one-commit change: delete
  `src/qbe_emit.{c,h}`, drop the QBE arm from `src/main.c`, drop
  the QBE option from scripts, drop `QBE = qbe` and the `qbe`
  invocation from the Makefile.
- **Last updated:** 2026-04-28
- **Last updated:** 2026-04-28

## Phase Log

### Phase 7.2 — float32 ABI

- **2026-04-29** — Mixel runtime parity bug: `mixel/01_hello` rendered
  pure blue + bright green instead of cornflower-blue + soft green.
  Root cause: `mix_to_lir(TYPE_FLOAT32) = LIR_TY_F64` mapped C-side
  `float` (4-byte) cbind fields to MIX `double` (8-byte). Storing into
  `SDL_FColor { r, g, b, a: float32 }` emitted four 8-byte stores into
  a 16-byte slot, each store stomping the next field. Fields read back
  as scrambled bit fragments (some channels hit 0 or 1 by coincidence).
  Same bug for any cbind shape with C `float` fields (Vec3, color
  buffers, GPU vertex layouts, …) but only colors were obviously
  wrong — most other f32 fields are within tolerance even when bits
  are mangled.
  Fix:
  - Added `LIR_TY_F32` (renders as `float`) plus `LIR_CONV_FPEXT` /
    `LIR_CONV_FPTRUNC` to the LIR.
  - `mix_to_lir(TYPE_FLOAT32) = LIR_TY_F32`.
  - `float_cast(ctx, v, dst_ty)` helper inserts fpext/fptrunc when
    crossing the f32↔f64 boundary.
  - All field stores/loads (regular shape lit, tagged-union variant
    payload, field reads, field assigns), call-arg coercion, var-decl
    init, and list scalar storage (to/from i64 for boxing) now
    promote/demote at the storage boundary. `print()` always passes
    f64, so f32 values are promoted before the call.
  - `render_float` rounds the immediate through `(double)(float)` when
    the operand type is f32, so the emitted 64-bit hex is exactly
    representable as a 32-bit float (LLVM rejects f64 hex with
    nonzero low bits when assigned to a `float` slot).
  - Makefile lacks header-deps tracking, so a stale build can mask
    enum reordering bugs after adding LIR_TY_F32. `make clean &&
    make` is required when changing lir.h.

### Phase 7.1 — Zero-init shape literal slots

- **2026-04-29** — Mixel runtime parity bug: `mix run` of any demo
  hit `Assertion failure at SDL_CreateGPUShader_REAL ... 'Shader
  sampler count cannot be higher than 16!'`. Cause: shape literals
  like `SDL_GPUShaderCreateInfo(code_size: ..., entrypoint: ...)`
  only set the user-named fields; omitted fields like `num_samplers`
  read uninitialised stack memory from the `alloca [N x i8]` slot
  and SDL asserted on the bogus value. QBE was zeroing the slot via
  `emit_shape_temp(zero_init=true)`; LLVM was not.
  Fix: `lower_shape_lit_into` now emits a `memset(slot, 0, total_size)`
  call before storing user-supplied fields. Spot-checked on six
  demos (01_hello, 03_collide, 15_particles, MixSnake, breakout,
  MixInvaders) — LLVM and QBE now produce byte-equal SDL diagnostics
  (both hit the same downstream `MTLFunction failed` Metal-side
  issue, which is environmental, not backend-related).

### Phase 7 — 107/107 main: tagged unions, shared, go/wait

- **2026-04-28** — Implemented the last three features so LLVM is
  at full main-suite parity with QBE.

  **025 tagged unions** (`shape Shape { Circle(r), Rect(w, h) }`):
  - `lower_shape_lit_into` recognises `is_tagged_union` shapes and
    materialises `{ tag: i64, payload: max-variant-bytes }` — store
    the variant tag at offset 0, store payload fields at
    `8 + variant.field.offset` (matches QBE layout).
  - `lower_match` pre-loads `tag = subj[0]`, then per-arm pattern
    `Variant(a, b)` compares the tag and binds each arg to a fresh
    local sourced from `8 + variant.field[k].offset`. Bound locals
    are popped after the arm body so subsequent arms don't see them.

  **036 go/wait**:
  - `NODE_GO_EXPR` lowers to: alloca an `n*8` byte buffer via
    `mix_alloc`, store each arg as i64 (via `to_storage_i64`), then
    call `mix_task_spawn(@fn, buf, n)` returning the task ptr. No-arg
    case passes a null buffer.
  - `NODE_WAIT_EXPR` lowers to a single `mix_task_wait(handle) → i64`
    call; the i64 result flows through `to_storage_i64`/coercions at
    the assignment site like any other lambda return value.

  **035 shared**:
  - `NODE_SHARED_EXPR` (`shared int(0)`) lowers to
    `mix_shared_new(init_as_i64) → ptr`.
  - `.read()` on a TYPE_SHARED dispatches to `mix_shared_read`.
  - `.update!(lambda)` lowers the lambda (which is already returned
    as a fn-ptr by the existing lambda machinery), coerces to ptr if
    needed, and calls `mix_shared_update(handle, fn_ptr)`. The
    runtime calls the lambda with the current value and stores the
    result.

  **Test gains:** 025, 035, 036. **Final:** LLVM 107/107 main +
  31/31 mixel — full parity with QBE. All three QBE-deletion
  criteria are met; QBE is now safe to remove.

### Phase 6 — LLVM is the default

- **2026-04-28** — Compile-time gate re-measured (median of 5 per
  workload):

  | workload         | qbe (ms) | llvm (ms) | ratio |
  |------------------|----------|-----------|-------|
  | hello            | 130.3    | 143.6     | 1.1x  |
  | 001_arithmetic   | 127.1    | 137.3     | 1.1x  |
  | 007_fibonacci    | 133.6    | 141.2     | 1.1x  |
  | 012_for_range    | 129.1    | 146.0     | 1.1x  |
  | big_scalar       | 132.2    | 133.7     | 1.0x  |
  | big_shapes       |  33.2    |  32.8     | 1.0x  |

  Decision: GO. Flipped `src/main.c` default from `"qbe"` →
  `"llvm"`, updated usage string, updated `tests/run_tests.sh` and
  `tests/run_mixel_sweep.sh` headers + `BACKEND` defaults. QBE
  still selectable. No regressions.

### Phase 5 — Mixel Sweep (batch 5)

- **2026-04-28 (104/107 main + 31/31 mixel LLVM)** — Mixel finally
  compiles end-to-end. The blocking issues:
  - **`g_consts` static cap of 256 was too small**: `use c "SDL3/SDL.h"`
    expands to ~6800 top-level NODE_CONST_DECLs from the SDL header.
    The per-program const table filled up with SDL constants and
    rolled past mixel's own `@const VERTEX_STRIDE = 32` etc., so the
    NODE_IDENT lookup fell through to my "cross-module global on demand"
    path and emitted `@VERTEX_STRIDE = external global i64`. Linker then
    failed: `_VERTEX_STRIDE` not defined. Fix: switched `g_consts` to a
    realloc'd dynamic array via `g_consts_push()`. Same fix needed for
    cond-compiled constants (walk the active branch).
  - **Pointer comparisons with int 0 immediate** (`SDL_IOFromFile(...) == nil`):
    LLVM rejects `icmp eq ptr %t, 0` — the immediate 0 must render as
    `null` when the comparison type is ptr. Fix: in `emit_bin`, force
    LIR_OPND_I64 operands to type=PTR when bin_type is PTR, so the
    operand renderer's existing "i64 0 + ptr → null" rule kicks in.
  - **`xor i1 <i32 value>, true`**: LIR_UN_NOT was hard-coding i1 in
    the emitter, but the lowerer happily fed it i32 values from
    bool-returning runtime helpers. Fix: in emit_un, when un_type is
    not i1 emit `icmp eq <ty>, 0` instead so the result stays i1.
  - **Bare `done` returning void from a non-void function**: text_repro
    has `if frame > 600 { done }` inside main, which is i32-returning.
    The lowerer was always emitting `ret void`. Fix: emit a typed zero
    return based on `ctx->fn->return_type`.
  - **Test gains:** all 31 mixel demos. Net +31 mixel, no main-suite
    change.

### Phase 5 — Advanced Language Features (continued, batch 4)

- **2026-04-28 (mid-flight, 104/107 LLVM)** — Final cleanup batch:
  - **`List[T].new(z)` / `Map.new(z)` / `Set.new(z)`**: zone-allocated
    collection constructors. `mix_list_new_in`, `mix_list_new_shape_in`,
    `mix_map_new_in`, `mix_set_new_in` wired (tests 098, 099).
  - **Slice expressions** `nums[1..3]` / `nums[..end]` / `nums[start..]`
    / `nums[..=end]` lower to `mix_list_slice` runtime call.
  - **Operator overload protocol**: `a + b` (etc.) on shapes dispatches
    to `Shape_op_add` / `op_sub` / `op_mul` / `op_div` / `op_mod` /
    `op_eq` / `op_neq` / `op_lt` / `op_gt` / `op_lte` / `op_gte` when
    the symbol exists. Matches QBE protocol. Test 021.
  - **Field-as-fn-call**: `b.fn(args)` where `fn` is a shape field
    (no `Shape_fn` method) loads the field value and emits an
    indirect call. Args coerced to i64. Test 079.
  - **Shape lit / field-assign coercions**: ptr→int (ptrtoint) when
    a fn-ptr is stored into an int-typed field; int→ptr (inttoptr);
    int→f64 (sitofp); reverse coercions. Test 079 b2.fn = inc.
  - **Module-level globals (`pub running! = true`)**: parsed as
    NODE_VAR_DECL at top level. `lir_module_add_global` registers a
    LLVM `@name = global <type> <init>` declaration; const-foldable
    initializers (int, bool) get static init values. Cross-module
    references hoist `external global` declarations on demand at
    NODE_IDENT lookup time. NODE_ASSIGN dispatches to global stores
    when the name isn't a local. Test 071.
  - **Variant patterns over Optional/Result**: match arms `some(v)`,
    `none`, `ok(v)`, `err(e)` recognized. Bind the inner value via
    `mix_optional_get` / `mix_result_unwrap` /
    `mix_result_unwrap_err`. The bound local is popped after the arm
    body so subsequent arms don't see it. Test 063.
  - **Implicit-return wraps optional/result**: when the function's
    sema return type is `T?` or `Result[T]` and the implicit final
    expression is the unwrapped value (e.g. `42` in `make_some()`
    that returns `int?`), wrap with `mix_optional_some` /
    `mix_result_ok`. Test 063.
  - **Match: empty next-label fix**: the last arm's `next_l` block
    would emit an empty label immediately followed by `merge_l`,
    which LLVM rejects. Now emit a fall-through `br merge_l` from
    the empty block. Affects all matches with no wildcard (021, 063).
  - **TYPE_GENERIC / TYPE_INFER → i64**: previously mapped to PTR
    which broke arithmetic on generic params. The i64 fallback fits
    the common case (integer generics, `has +` / `has ==`); full
    monomorphization would specialize per usage. Tests 027, 064.
  - **Refs as transparent ptrs** in `TOK_AMPERSAND`/`TOK_REF`/`TOK_REF_MUT`:
    for ptr-typed locals (lists/maps/sets/box/ref/str) the `ref x`
    operand returns the LOADED ptr, not the slot. Shape locals still
    return the slot (it IS the shape ptr); int/float locals still
    return the slot ptr (mut writeback). Test 096.
  - **`lower_collection_field`** unwraps `TYPE_REF`/`TYPE_OPTIONAL`
    wrappers before dispatching `.len`. Test 096.
  - **Test gains:** 021, 027, 032, 063, 064, 071, 079, 096, 098, 099.
    Net +10 tests on LLVM with QBE clean at 107/107.

### Phase 5 — Advanced Language Features (continued, batch 3)

- **2026-04-28 (mid-flight, 94/107 LLVM)** — Big batch:
  - **Lambdas + indirect calls**: NODE_LAMBDA hoisted to fresh top-level
    `mix_lambda_<n>` function with i64-only param/return ABI; new
    `LIR_OPND_FN_REF` operand for function symbol addresses (`@name`);
    `lir_emit_call_indirect` for variable/parameter-based indirect
    calls. NODE_IDENT for top-level functions returns `fn_ref`.
    Coercion path for fn-ptr-as-int handles `apply(dbl, 21)` where the
    callee declares `f: int`. Tests 018, 019, 077.
  - **Top-level fn pre-registration**: every `NODE_FN_DECL` registered
    as a callee with its sema-resolved signature *before* any body is
    lowered, so call-site arg-type inference can't shadow the actual
    function signature.
  - **Type alias bug**: sema's `resolve_type_node` for `IDENT` only
    accepted SHAPE/ZONE before — now returns any symtab-bound type, so
    `type Score = int` resolves correctly.
  - **String escape sequences**: `\n`/`\t`/`\r`/`\0`/`\\`/`\"` decoded
    when emitting LLVM string globals (LLVM uses `\HH` hex pairs).
  - **String comparisons** (==, !=, <, <=, >, >=): emit libc `strcmp`
    + integer compare (matches QBE).
  - **More string methods**: `char_at`, `slice`, `sort`, `index_of`,
    `repeat`, `count`, `code` (alias for `mix_ord`).
  - **Set algebra**: `union`/`intersect`/`diff` methods on sets.
  - **`is_known_builtin` predicate**: `lower_builtin` now early-returns
    for unknown names *before* lowering args. Avoided double-lowering
    of collection-literal arguments to user functions (was creating
    duplicate `mix_list_new` calls per `show_int_list([1,2,3])`).
  - **`to_set` element dispatch**: pick `mix_set_from_list` (str) vs
    `mix_set_from_list_int` (int) based on the list's element type.
  - **Bool widening**: ZEXT not SEXT (`true` was widening to -1 i64).
  - **`to_string(bool)` → mix_to_string_int** (matches QBE: prints "1"
    not "true").
  - **`print(file_exists(...))`**: dispatch to print_bool when the AST
    type is bool, even if the LIR value type is int32.
  - **String interp of collections**: dispatch to
    `mix_to_string_list_*` / `mix_to_string_map_*` / `mix_to_string_set_*`
    helpers based on the interpolated expression's MIX type.
  - **`continue` in for-list with mut writeback**: added separate
    `inc_l` block; body always falls through to inc_l, which does the
    writeback + increment, then jumps to cond.
  - **Shape shadowing**: `if`/`while` blocks now save/restore
    `local_count` so vars declared inside the block don't leak out
    (test 090 was reading the shadowed value past the if).
  - **Optional/Result return wrap**: lower_done auto-wraps with
    `mix_optional_some` / `mix_result_ok` when the enclosing function's
    sema return type is the corresponding wrapped type. lower_fail in
    Result context emits `mix_result_err + ret`. else_expr handles
    Result via `mix_result_is_ok` / `mix_result_unwrap`. Bare `none`
    materializes a real `mix_optional_none()` ptr (so caller can call
    `mix_optional_has` without dereffing NULL). Sema's NODE_ELSE_EXPR
    falls back to the fallback's type when value is bare `none`.
  - **`?` try operator**: NODE_TRY_EXPR lowers to `has_v` branch:
    error path returns the wrapped value as-is; ok path unwraps via
    `mix_result_unwrap` / `mix_optional_get`.
  - **Boxes**: `box(zone, value)` and `promote(zone, src)` lower to
    `mix_box_clone(zone, src_ptr, sizeof(boxed))`. `mix_box_check`
    inserted in field reads, field writes, method dispatch (via
    `unwrap_box_runtime`), and the box source of `promote` to get the
    payload pointer. Mut method receivers on boxes load the slot then
    `mix_box_check` (vs shape-typed mut receivers which pass the slot
    directly).
  - **Zone runtime**: `zone_create`, `zone_destroy`, `zone_reset`,
    `zone_alloc` builtins wired.
  - **Result/Optional ret-type from symtab**: lower_function_inner
    consults the symtab type for `~` functions with `fail` (sema
    wraps the return in TYPE_RESULT). The function definition's
    LIR signature now matches the call sites.
  - **Float coercion at call sites**: int→f64 (sitofp via i64) and
    f64→int (fptosi) added to lower_user_call_into.
  - **`type_of`**: added `Zone`, `Box`, `ref`, `fn` to the dispatch.
  - **collection field via ref**: `lower_collection_field` unwraps
    `TYPE_REF`/`TYPE_OPTIONAL` wrappers before dispatching `.len`.
  - **Test gains:** 018, 019, 024, 037, 039, 041, 042, 043, 045, 047,
    050, 051, 061, 077, 085, 087, 088, 090, 092, 093, 095. Net
    +22 tests on LLVM with QBE clean at 107/107.

### Phase 5 — Advanced Language Features (continued, batch 2)

- **2026-04-28 (mid-flight, 72/107 LLVM)** — Added: comprehensive
  builtin dispatch (bytes/alloc/peek/poke/chr/ord/random/time/
  shell/file_*/to_int/to_float/to_string/sizeof/type_of/promote/min/
  max/abs/sqrt/floor/ceil/round/sin/cos/tan/atan2/pow/log/exp), more
  list methods (sort/reverse/contains/index_of/remove/insert/join),
  string methods (split/concat/contains/starts_with/ends_with/replace/
  join), map indexing (`map[key]`) and methods (has/get/set/remove),
  set methods (add/has/remove), set iteration (`for x in set`),
  string concatenation via `+`, string equality via `==`/`!=`,
  computed-field-as-method (`r.area` where area is a 0-arg method),
  ref/box unwrap for field access + method dispatch, `LIR_CONV_PTRTOINT`
  and `LIR_CONV_INTTOPTR` (replace bitcast misuse), per-arg coercion
  in user calls (sext/zext/trunc/inttoptr/ptrtoint), pointer-comparison
  emit, fix for `continue` in for-range that skipped the increment.
- Strict callee-signature dedup relaxed to "first-registration wins"
  so cbind extern + call-site re-registration with widened types
  doesn't error. Coercion at the call site handles the actual ABI.

### Phase 5 — Advanced Language Features (continued)

- **2026-04-28 (mid-flight)** — Massive batch: collections (lists, maps,
  sets), refs/deref/ptr arithmetic, zones (as block passthrough),
  modules (compile_module already handles compilation; lowering is a
  no-op), break/continue (loop label stack), defer (LIFO at every
  return path), fail (panic call), unsafe blocks, match (statement +
  expression-as-implicit-return), optionals (`expr else default`),
  list comprehensions, casts, conditional compilation, string methods
  (len, upper, lower, trim, reverse), char-at indexing.
  - **LIR additions:** `LIR_OP_PTR_OFFSET` reused for ptr arithmetic,
    `LIR_CONV_BITCAST` added for double↔i64 (list scalar storage)
    and ptr↔i64 (ptr arithmetic fallback).
  - **Lists:** mix_list_new / push / get / set / ptr / len wired.
    Scalar elements bit-cast through i64 storage; shape elements use
    *_bytes variants and per-element memcpy/init.
  - **Maps/Sets:** mix_map_new / get / set / has / remove and
    mix_set_new / add / has / remove. `map[key]` indexing,
    `map.has(k)`, etc.
  - **For-list:** loop counter + mix_list_len + per-iteration
    list_get/list_ptr. Mutable foreach (`for x! in list`) writes the
    slot back via mix_list_set on each iteration.
  - **Match:** chain of icmp+br with merge label. For non-void
    function bodies whose last stmt is a match, allocate a result
    slot, each value-producing arm body stores into it; after merge,
    load and return.
  - **Optionals:** `none` lowers to a null ptr. `expr else default` for
    optional types: branch on mix_optional_has, get value via
    mix_optional_get, else use fallback.
  - **Casts:** int↔float via sitofp/fptosi; int widening via sext/zext
    (signedness from the source AST's MixType); narrowing via trunc.
  - **Defer:** `defer` collects stmts; every emit_ret_void/value path
    runs them in LIFO order before the actual ret.
  - **Fail:** `fail "msg"` → mix_panic("msg") + ret void.
  - **break/continue:** loop-depth stack on LowerCtx; while/for_range/
    for_list push break+continue labels and pop on exit.
  - **Variable shadowing fix:** `lower_var_decl` always allocates a
    fresh alloca slot (MIX permits same-name shadow with different
    type — `y = 7; y! = "seven"`).

### Phase 5 — Advanced Language Features (partial)

- **2026-04-28 (partial)** — Methods on shapes, top-level `@const` decls,
  string interpolation, and proper variable-shadowing semantics now lower
  through LIR. **LLVM passes 26/107** (was 20 after Phase 4B partial).
  - **Methods on shapes (test 015):** `lower_function` factored to take an
    optional `MethodCtx` (mangled name, self_shape, self_is_mutable). For
    each `NODE_SHAPE_DECL`, `lower_program` iterates `methods[]` and
    calls `lower_method`, which routes through the same body-lowering
    code path with a synthesized `self` first param. NODE_METHOD_CALL
    rewrites at lowering time to a synthetic `NODE_CALL_EXPR` named
    `Shape_method` with `obj` prepended to the arg list — reusing
    `lower_user_call` and its mut-param machinery.
  - **Bare-field reads in method bodies** (`radius * radius`): NODE_IDENT
    falls through to `current_shape` field lookup, GEP+load like a
    normal field expression. Sema already rewrites bare-field WRITES to
    `self.field` assigns, so reads were the only remaining gap.
  - **`@const` decls (test 010):** `lower_program` walks decls in two
    passes — first collects all `NODE_CONST_DECL` into a process-local
    table; then lowers function bodies. NODE_IDENT lookups consult this
    table after scope_lookup fails. Const values are inlined at use
    sites (matches QBE's behavior).
  - **String interpolation (test 014):** `NODE_STRING_INTERP` lowers as
    a chain of `mix_str_concat` calls. Non-string parts dispatch through
    `mix_to_string_int`/`_float`/`_bool` based on the part's LIR type
    (with the same SEXT/ZEXT widening rule from Phase 4B for ints).
  - **Variable shadowing (test 060):** `lower_var_decl` always allocates
    a fresh slot rather than reusing an existing scope entry. MIX
    permits both same-type and type-changing shadowing (e.g.,
    `y = 7; y! = "seven"`); reusing the old slot stomped the type and
    printed garbage.
  - Free pickups (no targeted work): 068_shape_field_mut,
    072_nested_shape_mut.
- **Explicit non-goals for this Phase 5 chunk** (carry forward to a
  continued Phase 5 effort): collections (lists/maps/sets), refs/boxes/
  zones, optionals/tagged unions, defer/fail, lambdas/closures, generic
  instantiation output, modules (use), unsafe-pointer arithmetic
  (NODE_DEREF_ASSIGN + ptr+offset arithmetic), `go`/`wait` concurrency.
  The plan's Phase 5 acceptance ("full main test suite passes on LLVM")
  is genuinely a multi-day effort — what's delivered here is the
  cleanly-tractable subset that didn't require new runtime ABI work.

### Phase 4B — Mutable Params + Positional Shapes (partial)

- **2026-04-28 (partial)** — Implemented mutable shape params, mutable
  scalar params, and positional shape literals. **LLVM passes 20/107**
  (was 17 after Phase 4A). All three targeted tests pass with output
  identical to QBE: 029_positional_shapes, 078_mut_shape_param,
  092_mut_scalar_param.
  - Mutable shape params: at the call site, look up the callee's
    `param_mutable[]` from the symtab's TYPE_FUNC. For each arg whose
    param is mut + shape, pass the local's storage ptr directly
    (aliased) instead of materializing a temp + memcpy. Mutations
    inside the callee now propagate back to the caller.
  - Mutable scalar params: at the function definition, mut scalar
    params get a LIR signature of `ptr` (instead of the scalar type) so
    the callee receives the caller's storage pointer. Inside the body,
    the local's `value_id` is the param's ptr SSA value (one alloca +
    store + load to get a stable id; mem2reg cleans it up at -O1+).
    Reads load through it, writes store through it — same code paths as
    other scalar locals.
  - Mutable scalar args via field expressions (`set_to(c.n, ...)`):
    pass the field address (GEP) instead of the loaded value.
  - Positional shape literals just worked once the unsigned-widening
    bug below was fixed — sema rewrites positional `Color(0, 255, 0,
    255)` to NODE_SHAPE_LIT and fills in field_names from the shape's
    declaration, so my Phase 4A literal lowering already handled it.
- **Explicit non-goals for Phase 4B (deferred to Phase 5):** lists,
  maps, sets, `Box[T]`, `Zone`, `ref T` / `ref! T` (as user-facing
  types), `at()` / `at_mut!()`, methods on shapes, mutable foreach with
  writeback (`for item! in list`), tagged unions, optionals,
  string interpolation, lambdas, generic instantiation. The plan's full
  Phase 4B acceptance ("all value-shape, borrow, box, zone, and
  collection tests pass on LLVM") is genuinely huge — the realistic
  next chunk is collections (lists/maps/sets), which I'm deferring to
  Phase 5 as its own focused effort. The mixel sweep still requires
  most of the deferred surface, so it stays at 0/31 LLVM.

### Phase 4A — Basic Shapes and Aggregate ABI

- **2026-04-28 (complete)** — Shape declarations, shape literals, field
  reads, field writes, and shape pass/return through user-defined
  functions all work end-to-end on LLVM. **LLVM passes 17/107** (was 10
  after Phase 3). Both Phase 4A target tests pass with output identical
  to QBE: 008_shapes, 009_shape_funcs.
  - Free pickups (no targeted work): 056_unions, 070_mut_reassign,
    086_shape_refcount_phase1, 089_done_shape_return, 105_shape_reassign.
  - LIR additions: `LIR_OP_ALLOCA_BYTES` (size + align for aggregates),
    `LIR_OP_PTR_OFFSET` (constant byte offset → field address),
    `LIR_OP_MEMCPY` (whole-shape copy). Module-level `uses_memcpy` flag
    so `@llvm.memcpy.p0.p0.i64` is declared exactly when needed.
  - Shape ABI for Phase 4A (recorded in `docs/RUNTIME_ABI.md` for
    consumers): shape values flow as ptrs to stack storage. Function
    params take `ptr` (sema enforces immutability for non-mut params).
    Shape returns are transformed at lowering: source-level
    `foo(...) -> Shape` becomes LIR `void @foo(ptr %_sret, ...)`. The
    caller allocates the result slot and passes its pointer as the
    hidden first arg. This matches the existing C-struct memcpy-by-ptr
    pattern locked in by test 106 in Phase 0.
  - Field offsets/sizes/alignments are read directly from
    `MixType.shape.fields[i]` (sema computes them). LIR does not carry
    its own shape-type table — kept the surface small per the Decision
    #4 scope fence.
  - One material init-vs-copy decision in lowering: `name = shape_lit`
    writes fields directly into name's slot (no temp+memcpy). The same
    helper (`lower_init_into`) is reused for `done expr` returns,
    shape-returning calls, and shape-typed call args. Each such
    "destination" gets the value materialized directly.

### Phase 3 — LLVM Scalar and Control-Flow Parity

- **2026-04-28 (complete)** — Scalar arithmetic, integer + float comparisons,
  locals via alloca/load/store, conditional and unconditional branches,
  multi-function modules with parameters, and extern bindings now lower
  through LIR and emit valid LLVM IR. **LLVM passes 10/107 main tests**
  (was 1 in Phase 1/2). All Phase 3 target tests pass with output
  identical to QBE: 000, 001, 002, 003, 004, 005, 006, 007, 012.
  - LIR vocabulary expanded (`src/lir.{h,c}`):
    `LIR_OP_ALLOCA / LOAD / STORE / BIN / UN / CONV / BR / BR_COND / LABEL`,
    `LirBinOp` (add/sub/mul/div/mod, eq/ne/lt/le/gt/ge, and/or),
    `LirUnOp` (neg/not), `LirConvKind` (sext/zext/trunc/sitofp/fptosi),
    `LirOpndKind` (added F64, BOOL), runtime callee table on `LirModule`.
  - `src/lower.c` rewritten as a proper expression+statement walker with
    per-function local scope. New emit-helpers wrap branch/return so the
    block-terminated invariant is enforced (LLVM rejects code after a
    terminator without a label). Implicit final-expr return for
    non-main user functions; `done expr` lowered to RET; loop counters
    use alloca + increment in the loop tail.
  - `src/llvm_emit.c` extended with renderers for every new op,
    `icmp/fcmp` dispatch by operand type, and a per-callee `declare`
    table (no more hardcoded runtime decls).
  - **Extern bindings:** `NODE_EXTERN_BLOCK` is now lowered. Signatures
    are pulled from the symtab's `TYPE_FUNC` rather than from raw AST
    type-nodes (sema doesn't write resolved_type back to type-node ASTs
    for params). 006_extern works as a result.
- **2026-04-28** — Phase 3 compile-time measurement gate (per the plan).
  See "Compile-Time Measurements" below for the table and the GO
  decision.

### Phase 2 — Lowering Layer Bring-Up

- **2026-04-28 (complete)** — `LirModule` and `lower_program()` introduced;
  LLVM emitter rewritten as a LIR consumer. The hello.mix subset now
  reaches LLVM IR through `AST → src/lower.c → LirModule → src/llvm_emit.c`.
  - New files:
    - `src/lir.{h,c}` — `LirModule`, `LirFunc`, `LirInstr` (CALL, RET),
      `LirOpnd` (string / param / value / i64 / none). Phase 2 vocabulary
      is deliberately small; the structure is shaped for Phase 3+
      additions (alloca, load, store, br, br_cond, arithmetic) without
      redesign.
    - `src/lower.{h,c}` — AST → LIR walker. Same fail-loud pattern as
      Phase 1: out-of-scope code raises `mix_error` with a clear
      `lowering (Phase 2): ... — not yet implemented` message at the
      source location.
  - `src/llvm_emit.{h,c}` rewritten: now takes a `LirModule` instead of
    walking the AST. Renderer only — no language-level decisions left
    here. Switch-based `emit_instr()` dispatch is purely additive for
    Phase 3.
  - `src/main.c` LLVM dispatch now routes through `lower_program()` then
    `llvm_emit_module()`. QBE and C paths unchanged (still direct-AST
    consumers — the plan's Phase 5+ work).
  - `Makefile` adds `lir.c` and `lower.c` to `COMPILER_SRCS`.
  - **Acceptance met:** hello.mix-class programs now emit through *both*
    paths — the QBE direct-AST path and the LLVM lower→LIR→emit path —
    with identical observable behavior. QBE regressions clean
    (107/107 main, 31/31 mixel). LLVM coverage unchanged from Phase 1
    (1/107 by design — same source scope, just re-architected).
  - One small lessons-learned: the first cut of `ret 0` rendered as
    `ret i64 0` in a function declared to return `i32`, because the
    integer literal helper hardcoded `LIR_TY_I64`. Fixed by adding
    `lir_opnd_int_typed(imm, type)` and threading the function's return
    type through. Captured here rather than in Failed Approaches because
    it never shipped — caught during Phase 2 verification.

### Phase 1 — LLVM Text IR Bring-Up

- **2026-04-28 (complete)** — `mix build --backend llvm examples/hello.mix`
  produces a working binary that prints "Hello, MIX!". End-to-end pipeline:
  `mix → src/llvm_emit.c → text .ll → clang -c → .o → cc link`. No LLVM
  libraries linked into `mix` itself (per Decision #5).
  - New files: `src/llvm_emit.{h,c}` — minimal Phase 1 emitter, ~200 lines.
    Hello.mix subset only. Anything else raises `mix_error` with a clear
    "LLVM backend (Phase 1): ... — not yet implemented" message at the
    source location.
  - `src/main.c` gained an LLVM dispatch branch that mirrors the C path:
    write `.ll` → `clang -c` → `.o` → final `cc` link. `compile_module()`
    signature gained `bool use_llvm_backend` (mutually exclusive with
    `use_c_backend`).
  - `Makefile` adds `src/llvm_emit.c` to `COMPILER_SRCS`. No LLVM toolchain
    discovery required at compile time — `clang` is invoked at runtime via
    PATH.
  - `--backend llvm` and `--emit-ir` (which now writes `.ll` for LLVM)
    surfaced via `--help`.
  - Regression check: QBE baselines unchanged (107/107 main, 31/31 mixel).
  - LLVM backend baseline: **1/107 main tests pass** (just the hello-style
    `002_hello`). Expected. Phase 3 will lift this by adding scalar +
    control-flow support; Phase 4A/4B add aggregates.
- **2026-04-28** — Pinned LLVM version revised from 18 to 21 in
  `LLVM_MIGRATION_PLAN.md` (Decision #7) after discovering only LLVM 21.1.2
  is installed locally. Recorded in Decision Log.

### Phase 0 — Baseline and Guardrails

- **2026-04-28 (complete)** — Phase 0 done. Artifacts:
  - All four QBE suites baselined green (see Backend Parity Table).
  - C backend baselined honestly: known broken (89/106 main tests, 0/31
    mixel demos). Not currently a working fallback. Recorded as a risk,
    not fixed in Phase 0 scope.
  - New regression test `tests/programs/106_c_shape_abi.mix` added,
    locking in shape-pointer ABI across the C boundary for both small
    (16-byte) and large (64-byte) shapes.
  - New mixel compile sweep `tests/run_mixel_sweep.sh` added, accepts a
    backend arg so future phases can re-baseline.
  - `tests/run_tests.sh` now honors `MIX_BACKEND=...` env var for
    backend-parameterized parity runs.
  - Runtime ABI written down in `docs/RUNTIME_ABI.md`. This is the
    contract Phase 3+ LLVM emitter must hit.
  - One real bug discovered while building test 106: `&` on an
    *immutable* shape binding produces a garbage pointer. See Failed
    Approaches.
- **2026-04-28** — Phase 0 kicked off. Confirmed clean baseline: `make`
  builds, `tests/run_tests.sh` reports 106 passed / 0 failed on QBE backend.
  Migration plan finalized in `LLVM_MIGRATION_PLAN.md`. CHANGELOG started.

## Compile-Time Measurements

Phase 3 measurement gate. Median of 5 runs of `mix build`, wall time
including frontend + codegen + clang/qbe + final cc link. Measured on
macOS arm64 (M-series), Homebrew LLVM 21.1.2.

Workloads tested through both QBE and LLVM (-O0):

| Workload                | QBE (ms) | LLVM (ms) | Ratio |
|-------------------------|----------|-----------|-------|
| examples/hello.mix      | 113      | 119       | 1.1x  |
| 001_arithmetic          | 116      | 121       | 1.0x  |
| 007_fibonacci           | 121      | 124       | 1.0x  |
| 012_for_range           | 123      | 124       | 1.0x  |
| big_scalar (synthetic)  | 119      | 117       | 1.0x  |

The synthetic `big_scalar` is `tests/scratch/big_scalar.mix` — a
~30-line scalar/CFG-heavy mix of recursion, loops, and arithmetic.

**Decision: GO.** Ratio is well under the 10x threshold across all
measured workloads. Push forward to Phase 4A.

**Caveat to revisit at end of Phase 4B:** these are small programs.
At this scale wall time is dominated by `mix` startup, frontend, and
the final `cc` link — actual LLVM codegen is a small fraction. The
real ratio test happens once mixel-class workloads (hundreds of
functions, the 2500-line mixel framework, plus 31 demos) can compile
through LLVM. Re-measure on the mixel compile sweep at the end of
Phase 4B and update this table. If the ratio there exceeds 25x, revisit
per the plan's threshold table.

The measurement script lives at `tests/measure_compile_time.sh` and is
re-runnable as workloads get larger.

Phase 4A re-measurement (added `big_shapes` synthetic — V3/Box shapes,
field reads, shape-return functions):

| Workload                | QBE (ms) | LLVM (ms) | Ratio |
|-------------------------|----------|-----------|-------|
| examples/hello.mix      | 116      | 121       | 1.0x  |
| 001_arithmetic          | 119      | 117       | 1.0x  |
| 007_fibonacci           | 119      | 116       | 1.0x  |
| 012_for_range           | 117      | 117       | 1.0x  |
| big_scalar (synthetic)  | 122      | 118       | 1.0x  |
| big_shapes (synthetic)  | 27       | 25        | 0.9x  |

Still well under threshold. GO continues into Phase 4B.

Phase 4B partial re-measurement (after mut params + positional shapes):

| Workload                | QBE (ms) | LLVM (ms) | Ratio |
|-------------------------|----------|-----------|-------|
| examples/hello.mix      | 122      | 129       | 1.1x  |
| 001_arithmetic          | 120      | 128       | 1.1x  |
| 007_fibonacci           | 127      | 132       | 1.0x  |
| 012_for_range           | 126      | 136       | 1.1x  |
| big_scalar (synthetic)  | 122      | 145       | 1.2x  |
| big_shapes (synthetic)  | 34       | 32        | 0.9x  |

Still under threshold. The big_scalar ratio nudged up slightly (1.2x);
likely measurement noise at this scale, but worth keeping an eye on.
GO continues into Phase 5.

## Backend Parity Table

Pass rates per backend across the test suites. Updated after each phase
with material backend changes.

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | 1/107   |
| 2026-04-28 | tests/run_error_tests.sh       | 36/36   | n/a     | n/a     |
| 2026-04-28 | tests/run_error_message_tests.sh | 19/19 | n/a     | n/a     |
| 2026-04-28 | tests/run_fmt_tests.sh         | 29/29   | n/a     | n/a     |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 3:

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **10/107** |

After Phase 4A:

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **17/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 4B (partial — mut params + positional shapes only):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **20/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

Newly passing on LLVM: 029_positional_shapes, 078_mut_shape_param,
092_mut_scalar_param. Mixel still 0/31 because it depends on lists,
shape methods, and several other Phase 5 items.

After Phase 5 (partial — methods, consts, string interp, shadowing):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **26/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 5 mid-flight (collections + control + most language surface):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **50/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 5 mid-flight (batch 2 — builtins, methods, str ops, fixes):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **72/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 5 mid-flight (batch 3 — lambdas, optionals/Result, boxes,
strings, escape sequences, scope shadowing, indirect calls):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **94/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 5 mid-flight (batch 4 — zone collections, slice, op
overload, fn fields, module globals, variant patterns):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **104/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | 0/31    |

After Phase 5 mixel sweep (batch 5 — dynamic const table, ptr-cmp
null, untyped not, bare done):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **104/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | **31/31** |

After Phase 7 (tagged unions, shared, go/wait):

| Date       | Suite                          | QBE     | C       | LLVM    |
|------------|--------------------------------|---------|---------|---------|
| 2026-04-28 | tests/run_tests.sh             | 107/107 | 89/106  | **107/107** |
| 2026-04-28 | tests/run_mixel_sweep.sh       | 31/31   | 0/31    | **31/31** |

LLVM at full parity with QBE on every test. All deletion criteria
for QBE are now met.

Remaining work to reach the plan's Phase 5 acceptance (full main
suite + mixel):

- Lambdas as function pointers + indirect calls (~3 tests)
- Generics with `@T` params (sema mostly does it; lowering needs to
  accept TYPE_GENERIC params) (~4 tests)
- Tagged unions (variant constructors, match arms by tag) (~5 tests)
- Computed fields (method-as-field-access shortcut) (~2 tests)
- Larger list method surface (sort, contains, index_of, insert,
  remove, reverse, slice, join) (~8 tests)
- `?` try operator, Result type (~3 tests)
- `go`/`wait` (~1 test)
- Larger string method surface (split, replace, char-at indexing
  done; join, etc.) (~5 tests)
- Float32 vs Float64 distinction (~2 tests)
- Std library wrapper modules (~5 tests)
- `use c` struct interop (cbind generates extern decls referencing
  shape types — same machinery should work but needs verification
  per-header) — required for mixel's SDL3 surface.
- Default parameter values (~1 test)
- Mixel debug pass (likely many small ABI issues with SDL3, GPU,
  Metal-specific paths)

Newly passing on LLVM: 008_shapes, 009_shape_funcs (targeted), plus
free pickups 056_unions, 070_mut_reassign, 086_shape_refcount_phase1,
089_done_shape_return, 105_shape_reassign. The mixel sweep is still
0/31 — Phase 4B (refs/boxes/zones, mutable containers, mutable
foreach) is what it actually needs.

QBE total: 222/222 across all suites (regression-clean after Phase 3).
C total: 89/137 (still broken on mixel, 17 main-suite failures).
LLVM total: 10/138 — was 1/138 after Phase 2. The 10 passing tests are:
000_return_zero, 001_arithmetic, 002_hello, 003_if_else, 004_functions,
005_while, 007_fibonacci, 012_for_range, 062_assign_vs_decl, plus
006_extern (which Phase 3's extern-block lowering enabled). Phase 4A
will lift this number significantly as shape support lands.

## Failed Approaches and Lessons

Record approaches that were tried and abandoned. Especially important for
the high-risk areas called out in the plan: aggregate ABI, shape
assignment, ref/box/zone semantics, mutable foreach writeback. Future
phases should consult this section before re-trying anything that lives
here.

### 2026-04-28 — `&` on immutable shape binding produces garbage

While building Phase 0 regression test `106_c_shape_abi.mix`, the first
draft used immutable bindings (`a = Vec2(...)`) and took their address
with `&a` to pass to a C function. Result: the C side received a
non-storage pointer and read garbage memory. Switching to mutable
bindings (`a! = Vec2(...)`) fixed it.

Reproducer:

```mix
shape Vec2
    x, y: float

extern "libc"
    v_x(v: *Vec2) -> float

main()
    a = Vec2(x: 3.0, y: 4.0)
    print(v_x(&a))    // prints garbage
    a! = Vec2(x: 3.0, y: 4.0)
    print(v_x(&a))    // prints 3
```

Lesson for the LLVM migration: this is a frontend/sema decision masquerading
as a backend behavior. Decide explicitly whether the new pipeline:

- (a) preserves it (and rejects `&immutable_binding` in sema), or
- (b) lifts the restriction by spilling immutable bindings to stack when
  their address is taken.

Do not silently change behavior here.

### 2026-04-28 — Unsigned int widening surfaced via uint8 fields

Phase 4B test 029_positional_shapes uses a `Color` shape with four
`uint8` fields. Initial run printed `-1` for `c1.r` where `c1.r = 255`.

Cause: my `to_i64` widening helper unconditionally used `LIR_CONV_SEXT`
when narrowing-widening to i64 for `mix_print_int(i64)`. SEXT on i8
treats the high bit as sign, so 0xFF (255) sign-extended becomes -1.

Fix: added `widen_to_i64(ctx, loc, val, ast_node->resolved_type)` that
picks SEXT vs ZEXT by inspecting the source AST node's `MixType`. Any
of TYPE_UINT8/16/32/64 or TYPE_BYTE → ZEXT; otherwise SEXT.

Lesson: the LIR operand type alone doesn't tell us signedness. Either
LIR needs unsigned variants (LIR_TY_U8, etc.) or call sites need to
consult the source MixType. Phase 4B picked the latter as the cheaper
fix; if signedness ambiguity bites elsewhere, revisit option A.

### 2026-04-28 — LLVM doesn't accept C99 hex-float literal format

First Phase 4A IR for 008_shapes used `printf("%a", v)` to render double
constants, producing C99 hex-float form like `0x1.8p+1`. LLVM rejects
that — it expects the IEEE-754 raw 64-bit pattern as `0x` followed by
exactly 16 hex digits.

Reproducer: `store double 0x1.8p+1, ptr %t1` →
`error: expected ',' after store operand` from clang's IR parser.

Fix: render via the bit pattern of the double:

```c
union { double d; unsigned long long u; } pun;
pun.d = v;
fprintf(out, "0x%016llX", pun.u);
```

Lesson: LLVM IR's float constant syntax is NOT C99-compatible despite
what the leading `0x` suggests. Caught during Phase 4A bring-up.

### 2026-04-28 — `float32` confusion with C `float`

While writing the C helper for test 106, I used `float` in the C side
to match MIX `float`. They do not match. MIX `float` is C `double`
(8 bytes), per `src/types.c:114-127`. Only `float32` matches C `float`.
This is recorded in `docs/RUNTIME_ABI.md` and must remain a noisy
gotcha until/unless the language renames the types.

## Decision Log

Material decisions that change direction from the original plan. Cite the
plan section being revised.

### 2026-04-28 — LLVM version pin revised: 18 → 21

Plan section: "Core Decisions / 7. Pin one LLVM version" originally said
Homebrew LLVM 18. The host machine only has LLVM 21.1.2 installed
(`/opt/homebrew/opt/llvm`); LLVM 18 is symlinked but points at 21.

Decision: **pin to LLVM 21.** Updated `LLVM_MIGRATION_PLAN.md` Decision
#7. No code change beyond using the system `clang` (which is the LLVM 21
clang). Phase 1 emitted IR validates clean against LLVM 21's parser.

### 2026-04-28 — C backend status acknowledged as broken (not fixed in Phase 0)

Plan section: "Core Decisions / 2. The C backend stays" calls C the
"portability fallback." Phase 0 baseline shows the C backend currently
fails 17/106 main tests and all 31 mixel demos.

Decision: **leave the C backend broken for now.** Fixing it is out of
Phase 0 scope and would derail the migration before it starts. The plan's
fallback claim should be read as aspirational. Revisit after Phase 4B
(once aggregate semantics live in lowering) — at that point the C emitter
becomes a thinner consumer of LIR and may be cheaper to repair.

If LLVM bring-up itself stalls and we need a working alternative
backend before then, this decision must be revisited.
