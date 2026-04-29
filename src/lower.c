#include "lower.h"
#include "errors.h"
#include "types.h"
#include "token.h"
#include "arena.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// Phase 4A lowering
//
// Adds basic shape support on top of Phase 3:
//   - shape locals/params/returns
//   - shape literals (Vec2(x: ..., y: ...))
//   - field reads and writes
//
// Shape ABI for Phase 4A:
//   - Shape values flow as pointers to stack storage (alloca'd by the
//     producer or pre-allocated by the consumer).
//   - Shape function params: passed as `ptr`. Sema enforces immutability
//     for non-mut params, so callees can read directly through the ptr
//     without copying.
//   - Shape function returns: the public MIX signature `foo(...) -> Shape`
//     becomes `void @foo(ptr %retptr, ...)` in LIR/LLVM. The caller
//     allocates the return slot and passes its pointer as the first arg.
//   - Shape locals: `alloca [N x i8] align A`, with N=total_size and
//     A=alignment from sema.
//
// Out-of-scope (Phase 4B / 5): refs/boxes/zones, list/map/set elements,
// at()/at_mut!(), mutable foreach writeback, methods, generics, defer,
// match, optionals, tagged unions, lambdas, closures.
// ============================================================================

typedef struct {
    const char *name;
    int         value_id;       // SSA ptr (alloca for scalars, ptr for shapes)
    LirType     scalar_type;    // for scalar locals; LIR_TY_VOID when shape
    MixType    *shape_type;     // non-NULL when this Local holds a shape ptr
    bool        is_param_ptr;   // true: value_id is a raw param ptr (no alloca);
                                //  shape params live as the caller's ptr directly
} Local;

typedef struct {
    LirModule *mod;
    LirFunc   *fn;
    SymTab    *symtab;          // callee mutability lookup
    Local     *locals;
    int        local_count;
    int        local_capacity;
    bool       block_terminated;

    // For shape-returning functions, this is the SSA value of the hidden
    // first-arg result pointer. -1 when the function does not return a
    // shape. Lowering writes the final expression's shape into *retptr.
    int        sret_value_id;
    MixType   *sret_shape_type;

    // The MIX return type of the enclosing function (NULL for void or
    // when not yet known). Used by `done` and bare `none` to wrap into
    // mix_optional_some/none when the function signature is `T?`.
    MixType   *fn_return_mix_type;

    // Phase 5: when lowering a method body, this is the enclosing shape's
    // MixType. Bare ident references that name a field of this shape are
    // rewritten on the fly to `self.field` reads.
    MixType   *current_shape;

    // Loop label stacks for break/continue.
    int        loop_break[64];
    int        loop_continue[64];
    int        loop_depth;

    // Deferred statements (LIFO). Emitted at every return path.
    AstNode   *deferred[64];
    int        defer_count;
} LowerCtx;

// Phase 5: top-level const decl table — interned by lower_program so
// NODE_IDENT lowering can substitute the const's value expression.
typedef struct {
    const char *name;
    AstNode    *value;
} ConstEntry;

static ConstEntry *g_consts;
static int         g_const_count;
static int         g_const_capacity;

static void g_consts_push(const char *name, AstNode *value) {
    if (g_const_count >= g_const_capacity) {
        int new_cap = g_const_capacity ? g_const_capacity * 2 : 256;
        ConstEntry *fresh = realloc(g_consts, new_cap * sizeof(ConstEntry));
        if (!fresh) return; // best-effort; OOM here is unrecoverable
        g_consts = fresh;
        g_const_capacity = new_cap;
    }
    g_consts[g_const_count].name = name;
    g_consts[g_const_count].value = value;
    g_const_count++;
}

// Phase 5: monotonically increasing per-program counter that names
// hoisted lambda functions ($mix_lambda_<n>). Reset at lower_program().
static int        g_lambda_counter;

// Module-level mutable globals (`pub running! = true` at file scope).
typedef struct {
    const char *name;       // exported / referenced name (no mangling)
    MixType    *mix_type;
    LirType     lir_type;
    AstNode    *init_expr;  // for the initializer code in main()
    bool        is_pub;     // tells the emitter whether to omit `internal`
} GlobalEntry;
static GlobalEntry g_globals[256];
static int         g_global_count;

static GlobalEntry *find_global(const char *name) {
    for (int i = 0; i < g_global_count; i++) {
        if (strcmp(g_globals[i].name, name) == 0) return &g_globals[i];
    }
    return NULL;
}

// ---- Forward decls ---------------------------------------------------------

static void    lower_stmt          (LowerCtx *ctx, AstNode *stmt);
static LirOpnd lower_expr          (LowerCtx *ctx, AstNode *expr);
static void    lower_init_into     (LowerCtx *ctx, LirOpnd dst,
                                     MixType *shape_type, AstNode *expr);
static LirOpnd field_address       (LowerCtx *ctx, AstNode *fe,
                                     ShapeFieldInfo **fi_out);
static LirOpnd lower_lambda        (LowerCtx *ctx, AstNode *lam);
static LirOpnd unwrap_box_runtime  (LowerCtx *ctx, SrcLoc loc, LirOpnd val,
                                     MixType *t);
static bool    needs_box_check     (MixType *t);

// ---- Diagnostics -----------------------------------------------------------

static void unsupported(AstNode *node, const char *what) {
    SrcLoc loc = node ? node->loc : (SrcLoc){0};
    mix_error(loc, "lowering (Phase 4A): %s — not yet implemented", what);
}

// ---- Type mapping ----------------------------------------------------------

static LirType mix_to_lir(MixType *t) {
    if (!t) return LIR_TY_VOID;
    switch (t->kind) {
        case TYPE_VOID:    return LIR_TY_VOID;
        case TYPE_BOOL:    return LIR_TY_I1;
        case TYPE_BYTE:
        case TYPE_INT8:    case TYPE_UINT8:    return LIR_TY_I8;
        case TYPE_INT32:   case TYPE_UINT32:   return LIR_TY_I32;
        case TYPE_INT:     case TYPE_INT64:    case TYPE_UINT64:
        case TYPE_INT16:   case TYPE_UINT16:   return LIR_TY_I64;
        case TYPE_FLOAT:   case TYPE_FLOAT64:  return LIR_TY_F64;
        case TYPE_FLOAT32: return LIR_TY_F32;
        case TYPE_STR:
        case TYPE_PTR:
        case TYPE_REF:
        case TYPE_BOX:     return LIR_TY_PTR;
        case TYPE_SHAPE:   return LIR_TY_PTR;     // shape values flow as ptrs
        // Phase 5+: collections, optionals, zones — all opaque pointers
        // managed by the runtime helpers.
        case TYPE_LIST:
        case TYPE_MAP:
        case TYPE_SET:
        case TYPE_OPTIONAL:
        case TYPE_RESULT:
        case TYPE_ZONE:
        case TYPE_FUNC:
        case TYPE_SHARED:
        case TYPE_TASK:    return LIR_TY_PTR;
        case TYPE_GENERIC:
        case TYPE_INFER:   return LIR_TY_I64; // Best-effort: most generic
                                             // uses with `has +`/`has ==`
                                             // are over int. Without per-
                                             // instance monomorphization
                                             // we can't pick per-call.
        case TYPE_NAMED:   return LIR_TY_PTR;  // opaque named type
        default:           return LIR_TY_VOID;
    }
}

static bool is_int_lir(LirType t) {
    return t == LIR_TY_I1 || t == LIR_TY_I8 || t == LIR_TY_I32 || t == LIR_TY_I64;
}

// AArch64 / x86-64 SysV C ABI: integer-only structs ≤ 8 bytes pass in
// a single integer register, not as a pointer. cbind brings these in
// as MIX shapes — we still treat them as shape values internally, but
// at C-extern call boundaries we load the slot as a sized int and pass
// that. Returns LIR_TY_VOID for shapes we don't want to pass-by-value
// (too big, or contain floats — float structs need HFA handling).
//
// This is only invoked for `extern` C callees. MIX-internal calls keep
// the existing pointer-passing ABI.
static LirType shape_int_value_lir(MixType *t) {
    if (!t || t->kind != TYPE_SHAPE) return LIR_TY_VOID;
    if (t->shape.is_tagged_union) return LIR_TY_VOID;
    int sz = t->shape.total_size;
    if (sz <= 0 || sz > 8) return LIR_TY_VOID;
    // Reject if any field is float — those need HFA (separate FP regs).
    for (int i = 0; i < t->shape.field_count; i++) {
        MixType *ft = t->shape.fields[i].type;
        if (ft && (ft->kind == TYPE_FLOAT || ft->kind == TYPE_FLOAT32 ||
                   ft->kind == TYPE_FLOAT64))
            return LIR_TY_VOID;
    }
    if (sz == 1)               return LIR_TY_I8;
    if (sz <= 4)               return LIR_TY_I32;
    return LIR_TY_I64;
}

static bool is_float_lir(LirType t) {
    return t == LIR_TY_F32 || t == LIR_TY_F64;
}

// Convert a float operand between f32 and f64. Returns the operand
// unchanged if its type already matches `dst`. Non-float operands are
// returned as-is — callers should mix in int-side coercions separately.
static LirOpnd float_cast(LowerCtx *ctx, SrcLoc loc, LirOpnd v, LirType dst) {
    if (!is_float_lir(v.type) || v.type == dst) return v;
    if (dst == LIR_TY_F32 && v.type == LIR_TY_F64) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_FPTRUNC,
                                LIR_TY_F64, LIR_TY_F32, v);
        return lir_opnd_value(r, LIR_TY_F32);
    }
    if (dst == LIR_TY_F64 && v.type == LIR_TY_F32) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_FPEXT,
                                LIR_TY_F32, LIR_TY_F64, v);
        return lir_opnd_value(r, LIR_TY_F64);
    }
    return v;
}

static bool mix_is_shape(MixType *t) {
    return t && t->kind == TYPE_SHAPE;
}

// ---- Local scope -----------------------------------------------------------

static Local *scope_add(LowerCtx *ctx, const char *name) {
    if (ctx->local_count >= ctx->local_capacity) {
        int new_cap = ctx->local_capacity ? ctx->local_capacity * 2 : 16;
        Local *fresh = arena_alloc(ctx->mod->arena, new_cap * sizeof(Local));
        if (ctx->locals) memcpy(fresh, ctx->locals,
                                ctx->local_count * sizeof(Local));
        ctx->locals = fresh;
        ctx->local_capacity = new_cap;
    }
    Local *l = &ctx->locals[ctx->local_count++];
    memset(l, 0, sizeof(*l));
    l->name = arena_strdup(ctx->mod->arena, name);
    return l;
}

static Local *scope_lookup(LowerCtx *ctx, const char *name) {
    for (int i = ctx->local_count - 1; i >= 0; i--) {
        if (strcmp(ctx->locals[i].name, name) == 0) return &ctx->locals[i];
    }
    return NULL;
}

// ---- Block-terminator wrappers --------------------------------------------

static void emit_label(LowerCtx *ctx, SrcLoc loc, int label_id) {
    lir_emit_label(ctx->fn, loc, label_id);
    ctx->block_terminated = false;
}
static void emit_br(LowerCtx *ctx, SrcLoc loc, int label) {
    if (ctx->block_terminated) return;
    lir_emit_br(ctx->fn, loc, label);
    ctx->block_terminated = true;
}
static void emit_br_cond(LowerCtx *ctx, SrcLoc loc, LirOpnd cond, int t, int e) {
    if (ctx->block_terminated) return;
    lir_emit_br_cond(ctx->fn, loc, cond, t, e);
    ctx->block_terminated = true;
}
static void run_defers(LowerCtx *ctx) {
    if (ctx->defer_count == 0) return;
    int saved = ctx->defer_count;
    ctx->defer_count = 0;     // disarm so a defer-inside-defer doesn't loop
    for (int i = saved - 1; i >= 0; i--) {
        lower_stmt(ctx, ctx->deferred[i]);
    }
    ctx->defer_count = saved;
}

static void emit_ret_void(LowerCtx *ctx, SrcLoc loc) {
    if (ctx->block_terminated) return;
    run_defers(ctx);
    lir_emit_ret_void(ctx->fn, loc);
    ctx->block_terminated = true;
}
static void emit_ret_value(LowerCtx *ctx, SrcLoc loc, LirOpnd value) {
    if (ctx->block_terminated) return;
    run_defers(ctx);
    lir_emit_ret_value(ctx->fn, loc, value);
    ctx->block_terminated = true;
}

// ---- Runtime helper signatures --------------------------------------------

static void register_print_str  (LirModule *m, SrcLoc loc) { LirType p[]={LIR_TY_PTR}; lir_register_callee(m,loc,"mix_print_str",  LIR_TY_VOID,p,1); }
static void register_print_int  (LirModule *m, SrcLoc loc) { LirType p[]={LIR_TY_I64}; lir_register_callee(m,loc,"mix_print_int",  LIR_TY_VOID,p,1); }
static void register_print_float(LirModule *m, SrcLoc loc) { LirType p[]={LIR_TY_F64}; lir_register_callee(m,loc,"mix_print_float",LIR_TY_VOID,p,1); }
static void register_print_bool (LirModule *m, SrcLoc loc) { LirType p[]={LIR_TY_I32}; lir_register_callee(m,loc,"mix_print_bool", LIR_TY_VOID,p,1); }
static void register_set_args   (LirModule *m)             { LirType p[]={LIR_TY_I32,LIR_TY_PTR}; lir_register_callee(m,(SrcLoc){0},"mix_set_args",LIR_TY_VOID,p,2); }

// ---- Coercions -------------------------------------------------------------

static LirOpnd to_i32(LowerCtx *ctx, SrcLoc loc, LirOpnd v) {
    if (v.type == LIR_TY_I32) return v;
    int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_ZEXT, v.type, LIR_TY_I32, v);
    return lir_opnd_value(r, LIR_TY_I32);
}
static bool mix_is_unsigned(MixType *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        case TYPE_BYTE: case TYPE_BOOL:
            return true;
        default:
            return false;
    }
}

static LirOpnd to_i64(LowerCtx *ctx, SrcLoc loc, LirOpnd v) {
    if (v.type == LIR_TY_I64) return v;
    if (!is_int_lir(v.type))  return v;
    int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_SEXT, v.type, LIR_TY_I64, v);
    return lir_opnd_value(r, LIR_TY_I64);
}

// Widen any narrow int operand to i64. Picks ZEXT for unsigned source
// types and SEXT for signed types, using the AST node's resolved_type as
// the source of truth.
static LirOpnd widen_to_i64(LowerCtx *ctx, SrcLoc loc, LirOpnd v, MixType *src) {
    if (v.type == LIR_TY_I64) return v;
    if (!is_int_lir(v.type))  return v;
    LirConvKind k = mix_is_unsigned(src) ? LIR_CONV_ZEXT : LIR_CONV_SEXT;
    int r = lir_emit_conv(ctx->fn, loc, k, v.type, LIR_TY_I64, v);
    return lir_opnd_value(r, LIR_TY_I64);
}

// ---- Bin op token mapping --------------------------------------------------

static LirBinOp tok_to_bin(TokenKind op, bool *ok) {
    *ok = true;
    switch (op) {
        case TOK_PLUS:    return LIR_BIN_ADD;
        case TOK_MINUS:   return LIR_BIN_SUB;
        case TOK_STAR:    return LIR_BIN_MUL;
        case TOK_SLASH:   return LIR_BIN_DIV;
        case TOK_PERCENT: return LIR_BIN_MOD;
        case TOK_EQEQ:    return LIR_BIN_EQ;
        case TOK_NEQ:     return LIR_BIN_NE;
        case TOK_LT:      return LIR_BIN_LT;
        case TOK_LTE:     return LIR_BIN_LE;
        case TOK_GT:      return LIR_BIN_GT;
        case TOK_GTE:     return LIR_BIN_GE;
        case TOK_AND:     return LIR_BIN_AND;
        case TOK_OR:      return LIR_BIN_OR;
        default: *ok = false; return LIR_BIN_ADD;
    }
}

// ---- print() ---------------------------------------------------------------

// ---- Lists ----------------------------------------------------------------

// Coerce a scalar value to i64 for list storage. Strings/pointers stay
// as ptr (caller-side LIR_TY_PTR maps cleanly to i64 in the runtime
// because they're the same width); booleans and narrow ints use ZEXT/SEXT;
// floats use bitcast.
static LirOpnd to_storage_i64(LowerCtx *ctx, SrcLoc loc, LirOpnd v, MixType *src) {
    if (v.type == LIR_TY_F32) {
        // Promote to f64 first so the bitcast width matches.
        v = float_cast(ctx, loc, v, LIR_TY_F64);
    }
    if (v.type == LIR_TY_F64) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_BITCAST,
                                LIR_TY_F64, LIR_TY_I64, v);
        return lir_opnd_value(r, LIR_TY_I64);
    }
    if (v.type == LIR_TY_PTR) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_PTRTOINT,
                                LIR_TY_PTR, LIR_TY_I64, v);
        return lir_opnd_value(r, LIR_TY_I64);
    }
    return widen_to_i64(ctx, loc, v, src);
}

static LirOpnd from_storage_i64(LowerCtx *ctx, SrcLoc loc, LirOpnd v_i64,
                                  MixType *target)
{
    LirType lt = mix_to_lir(target);
    if (lt == LIR_TY_I64) return v_i64;
    if (lt == LIR_TY_F64) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_BITCAST,
                                LIR_TY_I64, LIR_TY_F64, v_i64);
        return lir_opnd_value(r, LIR_TY_F64);
    }
    if (lt == LIR_TY_F32) {
        // i64 → f64 → fptrunc to f32 (storage was widened on the way in).
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_BITCAST,
                                LIR_TY_I64, LIR_TY_F64, v_i64);
        return float_cast(ctx, loc, lir_opnd_value(r, LIR_TY_F64), LIR_TY_F32);
    }
    if (lt == LIR_TY_PTR) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_INTTOPTR,
                                LIR_TY_I64, LIR_TY_PTR, v_i64);
        return lir_opnd_value(r, LIR_TY_PTR);
    }
    if (is_int_lir(lt)) {
        int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_TRUNC,
                                LIR_TY_I64, lt, v_i64);
        return lir_opnd_value(r, lt);
    }
    return v_i64;
}

static void register_runtime(LirModule *mod, SrcLoc loc, const char *name,
                              LirType ret, LirType *params, int n)
{
    lir_register_callee(mod, loc, name, ret, params, n);
}

static LirOpnd lower_list_lit(LowerCtx *ctx, AstNode *expr) {
    MixType *list_type = expr->resolved_type;
    MixType *elem = (list_type && list_type->kind == TYPE_LIST)
        ? list_type->list.elem_type : NULL;

    int list_v;
    if (elem && elem->kind == TYPE_SHAPE) {
        LirType ps[] = { LIR_TY_I64 };
        register_runtime(ctx->mod, expr->loc, "mix_list_new_shape",
                         LIR_TY_PTR, ps, 1);
        LirOpnd args[] = { lir_opnd_int_typed(elem->shape.total_size, LIR_TY_I64) };
        list_v = lir_emit_call(ctx->fn, expr->loc, "mix_list_new_shape",
                                 LIR_TY_PTR, args, 1);
    } else {
        register_runtime(ctx->mod, expr->loc, "mix_list_new",
                         LIR_TY_PTR, NULL, 0);
        list_v = lir_emit_call(ctx->fn, expr->loc, "mix_list_new",
                                 LIR_TY_PTR, NULL, 0);
    }
    LirOpnd list_p = lir_opnd_value(list_v, LIR_TY_PTR);

    // Push each element.
    for (int i = 0; i < expr->list_lit.element_count; i++) {
        AstNode *e = expr->list_lit.elements[i];
        if (elem && elem->kind == TYPE_SHAPE) {
            // Materialize element into a temp, then push_bytes.
            int tmp = lir_emit_alloca_bytes(ctx->fn, e->loc,
                                              elem->shape.total_size,
                                              elem->shape.alignment);
            LirOpnd tmp_p = lir_opnd_value(tmp, LIR_TY_PTR);
            lower_init_into(ctx, tmp_p, elem, e);
            LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
            register_runtime(ctx->mod, e->loc, "mix_list_push_bytes",
                             LIR_TY_VOID, ps, 2);
            LirOpnd args[] = { list_p, tmp_p };
            lir_emit_call(ctx->fn, e->loc, "mix_list_push_bytes",
                            LIR_TY_VOID, args, 2);
        } else {
            LirOpnd v = lower_expr(ctx, e);
            LirOpnd as_i64 = to_storage_i64(ctx, e->loc, v, e->resolved_type);
            LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
            register_runtime(ctx->mod, e->loc, "mix_list_push",
                             LIR_TY_VOID, ps, 2);
            LirOpnd args[] = { list_p, as_i64 };
            lir_emit_call(ctx->fn, e->loc, "mix_list_push",
                            LIR_TY_VOID, args, 2);
        }
    }
    return list_p;
}

static LirOpnd lower_index_expr(LowerCtx *ctx, AstNode *ie) {
    AstNode *obj = ie->index_expr.object;
    MixType *ot = obj->resolved_type;

    if (ot && ot->kind == TYPE_LIST) {
        LirOpnd list = lower_expr(ctx, obj);
        LirOpnd idx  = lower_expr(ctx, ie->index_expr.index);
        LirOpnd idx_i64 = to_i64(ctx, ie->loc, idx);
        MixType *elem = ot->list.elem_type;
        if (elem && elem->kind == TYPE_SHAPE) {
            LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
            register_runtime(ctx->mod, ie->loc, "mix_list_ptr",
                             LIR_TY_PTR, ps, 2);
            LirOpnd args[] = { list, idx_i64 };
            int r = lir_emit_call(ctx->fn, ie->loc, "mix_list_ptr",
                                    LIR_TY_PTR, args, 2);
            return lir_opnd_value(r, LIR_TY_PTR);
        }
        LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, ie->loc, "mix_list_get",
                         LIR_TY_I64, ps, 2);
        LirOpnd args[] = { list, idx_i64 };
        int r = lir_emit_call(ctx->fn, ie->loc, "mix_list_get",
                                LIR_TY_I64, args, 2);
        LirOpnd raw = lir_opnd_value(r, LIR_TY_I64);
        return from_storage_i64(ctx, ie->loc, raw, elem);
    }
    if (ot && ot->kind == TYPE_MAP) {
        // Map indexing: mix_map_get returns int64 (raw value).
        LirOpnd m   = lower_expr(ctx, obj);
        LirOpnd k   = lower_expr(ctx, ie->index_expr.index);
        LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
        register_runtime(ctx->mod, ie->loc, "mix_map_get",
                         LIR_TY_I64, ps, 2);
        LirOpnd args[] = { m, k };
        int r = lir_emit_call(ctx->fn, ie->loc, "mix_map_get",
                                LIR_TY_I64, args, 2);
        LirOpnd raw = lir_opnd_value(r, LIR_TY_I64);
        return from_storage_i64(ctx, ie->loc, raw, ot->map.val_type);
    }
    if (ot && ot->kind == TYPE_STR) {
        // String char-at: mix_str_char_at(s, idx) → ptr (one-char string)
        LirOpnd s   = lower_expr(ctx, obj);
        LirOpnd idx = lower_expr(ctx, ie->index_expr.index);
        LirOpnd idx_i64 = to_i64(ctx, ie->loc, idx);
        LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, ie->loc, "mix_str_char_at",
                         LIR_TY_PTR, ps, 2);
        LirOpnd args[] = { s, idx_i64 };
        int r = lir_emit_call(ctx->fn, ie->loc, "mix_str_char_at",
                                LIR_TY_PTR, args, 2);
        return lir_opnd_value(r, LIR_TY_PTR);
    }
    unsupported(ie, "index into non-list/map/str");
    return lir_opnd_none();
}

// `list.len` / `map.len` / etc. → runtime helper. Returns true if the
// caller should use *out as the lowered value; false if it's not a
// recognized special field (caller falls back to normal field access).
static bool lower_collection_field(LowerCtx *ctx, AstNode *fe, LirOpnd *out) {
    AstNode *obj = fe->field_expr.object;
    MixType *ot = obj ? obj->resolved_type : NULL;
    const char *name = fe->field_expr.field_name;
    if (!ot || !name) return false;
    // Refs to collections (`ref [int]`) carry the underlying collection
    // type at runtime; the borrow handle is just a transparent pointer.
    while (ot && (ot->kind == TYPE_REF || ot->kind == TYPE_OPTIONAL)) {
        if (ot->kind == TYPE_REF) ot = ot->ref.base;
        else ot = ot->optional.inner;
    }
    if (!ot) return false;

    if (ot->kind == TYPE_LIST && strcmp(name, "len") == 0) {
        LirOpnd list = lower_expr(ctx, obj);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, fe->loc, "mix_list_len", LIR_TY_I64, ps, 1);
        LirOpnd args[] = { list };
        int r = lir_emit_call(ctx->fn, fe->loc, "mix_list_len",
                                LIR_TY_I64, args, 1);
        *out = lir_opnd_value(r, LIR_TY_I64);
        return true;
    }
    if (ot->kind == TYPE_MAP && strcmp(name, "len") == 0) {
        LirOpnd m = lower_expr(ctx, obj);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, fe->loc, "mix_map_len", LIR_TY_I64, ps, 1);
        LirOpnd args[] = { m };
        int r = lir_emit_call(ctx->fn, fe->loc, "mix_map_len",
                                LIR_TY_I64, args, 1);
        *out = lir_opnd_value(r, LIR_TY_I64);
        return true;
    }
    if (ot->kind == TYPE_SET && strcmp(name, "len") == 0) {
        LirOpnd s = lower_expr(ctx, obj);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, fe->loc, "mix_set_len", LIR_TY_I64, ps, 1);
        LirOpnd args[] = { s };
        int r = lir_emit_call(ctx->fn, fe->loc, "mix_set_len",
                                LIR_TY_I64, args, 1);
        *out = lir_opnd_value(r, LIR_TY_I64);
        return true;
    }
    if (ot->kind == TYPE_STR && strcmp(name, "len") == 0) {
        LirOpnd s = lower_expr(ctx, obj);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, fe->loc, "mix_str_len", LIR_TY_I64, ps, 1);
        LirOpnd args[] = { s };
        int r = lir_emit_call(ctx->fn, fe->loc, "mix_str_len",
                                LIR_TY_I64, args, 1);
        *out = lir_opnd_value(r, LIR_TY_I64);
        return true;
    }
    return false;
}

// ---- print ----------------------------------------------------------------

static void lower_print(LowerCtx *ctx, AstNode *call) {
    if (call->call.arg_count != 1) {
        unsupported(call, "print() with arity != 1");
        return;
    }
    AstNode *arg = call->call.args[0];

    if (arg->kind == NODE_STRING_LIT) {
        int sid = lir_intern_string(ctx->mod, arg->string_lit.value,
                                     arg->string_lit.length);
        register_print_str(ctx->mod, call->loc);
        LirOpnd args[] = { lir_opnd_string(sid) };
        lir_emit_call(ctx->fn, call->loc, "mix_print_str", LIR_TY_VOID, args, 1);
        return;
    }

    LirOpnd val = lower_expr(ctx, arg);
    if (mix_error_count() > 0) return;

    // Treat sema-bool values (e.g. helpers that return int32 but represent
    // a bool, like file_exists) as bools so print emits "true"/"false".
    bool ast_is_bool = arg->resolved_type && arg->resolved_type->kind == TYPE_BOOL;

    if (is_float_lir(val.type)) {
        // print() always takes a double; promote f32 to f64 first.
        if (val.type == LIR_TY_F32) val = float_cast(ctx, call->loc, val, LIR_TY_F64);
        register_print_float(ctx->mod, call->loc);
        LirOpnd args[] = { val };
        lir_emit_call(ctx->fn, call->loc, "mix_print_float", LIR_TY_VOID, args, 1);
    } else if (val.type == LIR_TY_I1 || ast_is_bool) {
        register_print_bool(ctx->mod, call->loc);
        LirOpnd args[] = { to_i32(ctx, call->loc, val) };
        lir_emit_call(ctx->fn, call->loc, "mix_print_bool", LIR_TY_VOID, args, 1);
    } else if (is_int_lir(val.type)) {
        register_print_int(ctx->mod, call->loc);
        // Choose ZEXT vs SEXT based on the AST argument's resolved type.
        LirOpnd args[] = { widen_to_i64(ctx, call->loc, val, arg->resolved_type) };
        lir_emit_call(ctx->fn, call->loc, "mix_print_int", LIR_TY_VOID, args, 1);
    } else if (val.type == LIR_TY_PTR) {
        // Dispatch print(collection) by container kind + element type.
        MixType *at = arg->resolved_type;
        if (at && at->kind == TYPE_LIST) {
            MixType *elem = at->list.elem_type;
            const char *fn_name = "mix_print_list_int";
            if (elem) {
                if (elem->kind == TYPE_STR)               fn_name = "mix_print_list_str";
                else if (elem->kind == TYPE_BOOL)         fn_name = "mix_print_list_bool";
                else if (elem->kind == TYPE_FLOAT ||
                         elem->kind == TYPE_FLOAT64 ||
                         elem->kind == TYPE_FLOAT32)      fn_name = "mix_print_list_float";
            }
            LirType ps[] = { LIR_TY_PTR };
            register_runtime(ctx->mod, call->loc, fn_name, LIR_TY_VOID, ps, 1);
            LirOpnd args[] = { val };
            lir_emit_call(ctx->fn, call->loc, fn_name, LIR_TY_VOID, args, 1);
        } else if (at && at->kind == TYPE_MAP) {
            MixType *vt = at->map.val_type;
            const char *fn_name = (vt && vt->kind == TYPE_STR)
                ? "mix_print_map_str" : "mix_print_map";
            LirType ps[] = { LIR_TY_PTR };
            register_runtime(ctx->mod, call->loc, fn_name, LIR_TY_VOID, ps, 1);
            LirOpnd args[] = { val };
            lir_emit_call(ctx->fn, call->loc, fn_name, LIR_TY_VOID, args, 1);
        } else if (at && at->kind == TYPE_SET) {
            MixType *et = at->set.elem_type;
            const char *fn_name = (et && type_is_integer(et))
                ? "mix_print_set_int" : "mix_print_set";
            LirType ps[] = { LIR_TY_PTR };
            register_runtime(ctx->mod, call->loc, fn_name, LIR_TY_VOID, ps, 1);
            LirOpnd args[] = { val };
            lir_emit_call(ctx->fn, call->loc, fn_name, LIR_TY_VOID, args, 1);
        } else {
            register_print_str(ctx->mod, call->loc);
            LirOpnd args[] = { val };
            lir_emit_call(ctx->fn, call->loc, "mix_print_str", LIR_TY_VOID, args, 1);
        }
    } else {
        unsupported(arg, "print() of unsupported type");
    }
}

// ---- Shape-aware materialization -------------------------------------------
//
// Lower an expression that produces a shape value, writing the result
// directly into the destination pointer `dst`. This avoids an extra
// alloca + memcpy for the common case of `local = shape_expr`.

// Forward decl for shape-lit-into-dst.
static void lower_shape_lit_into(LowerCtx *ctx, LirOpnd dst,
                                  MixType *shape_type, AstNode *lit);

// Lower a call expression that returns a shape, writing the result into
// `dst`. The callee is shape-returning so its LIR signature has been
// transformed to take a sret-style first arg.
static void lower_shape_call_into(LowerCtx *ctx, LirOpnd dst,
                                   MixType *shape_type, AstNode *call);

// Wrapper that dispatches on the source expression kind.
static void lower_init_into(LowerCtx *ctx, LirOpnd dst, MixType *shape_type,
                             AstNode *expr)
{
    if (!expr) return;
    if (expr->kind == NODE_SHAPE_LIT) {
        lower_shape_lit_into(ctx, dst, shape_type, expr);
        return;
    }
    if (expr->kind == NODE_CALL_EXPR &&
        expr->resolved_type && expr->resolved_type->kind == TYPE_SHAPE) {
        lower_shape_call_into(ctx, dst, shape_type, expr);
        return;
    }
    // Generic case: lower the expression to a ptr, memcpy from src to dst.
    LirOpnd src = lower_expr(ctx, expr);
    if (mix_error_count() > 0) return;
    if (src.type != LIR_TY_PTR) {
        unsupported(expr, "shape initializer that is not a shape value");
        return;
    }
    lir_emit_memcpy(ctx->fn, expr->loc, dst, src,
                     shape_type->shape.total_size,
                     shape_type->shape.alignment);
}

static ShapeFieldInfo *find_field(MixType *shape, const char *name) {
    for (int i = 0; i < shape->shape.field_count; i++) {
        if (strcmp(shape->shape.fields[i].name, name) == 0)
            return &shape->shape.fields[i];
    }
    return NULL;
}

static void lower_shape_lit_into(LowerCtx *ctx, LirOpnd dst,
                                  MixType *shape_type, AstNode *lit)
{
    // Zero the destination slot before storing user-supplied fields.
    // MIX shape literals only mention the fields the user names; any
    // omitted field must read back as zero — sema doesn't reject
    // partial literals (SDL constructors like SDL_GPUShaderCreateInfo
    // rely on this). Without this memset, unset fields like
    // `num_samplers` read whatever happened to be on the stack and
    // SDL asserts on the bogus value. QBE does the same via its
    // emit_shape_temp(zero_init=true) on NODE_SHAPE_LIT.
    if (shape_type && shape_type->kind == TYPE_SHAPE &&
        shape_type->shape.total_size > 0) {
        LirType mps[] = { LIR_TY_PTR, LIR_TY_I32, LIR_TY_I64 };
        register_runtime(ctx->mod, lit->loc, "memset",
                         LIR_TY_VOID, mps, 3);
        LirOpnd margs[] = {
            dst,
            lir_opnd_int_typed(0, LIR_TY_I32),
            lir_opnd_int_typed(shape_type->shape.total_size, LIR_TY_I64),
        };
        lir_emit_call(ctx->fn, lit->loc, "memset", LIR_TY_VOID, margs, 3);
    }

    // Tagged union variant constructor (`Circle(radius: 5.0)` → store
    // tag=0 at offset 0, then variant fields at offset 8+).
    if (shape_type && shape_type->shape.is_tagged_union) {
        ShapeVariant *sv = type_find_variant(shape_type, lit->shape_lit.shape_name);
        if (!sv) {
            unsupported(lit, "tagged union variant not found");
            return;
        }
        // Store tag (i64) at offset 0.
        int tag_addr = lir_emit_ptr_offset(ctx->fn, lit->loc, dst, 0);
        lir_emit_store(ctx->fn, lit->loc, LIR_TY_I64,
                        lir_opnd_int_typed(sv->tag, LIR_TY_I64),
                        lir_opnd_value(tag_addr, LIR_TY_PTR));
        // Store variant payload fields at offset 8 + variant.field.offset.
        for (int i = 0; i < lit->shape_lit.field_count && i < sv->field_count; i++) {
            AstNode *fexpr = lit->shape_lit.field_values[i];
            ShapeFieldInfo *f = &sv->fields[i];
            LirOpnd field_addr = lir_opnd_value(
                lir_emit_ptr_offset(ctx->fn, fexpr->loc, dst, 8 + f->offset),
                LIR_TY_PTR);
            if (mix_is_shape(f->type)) {
                lower_init_into(ctx, field_addr, f->type, fexpr);
                continue;
            }
            LirOpnd v = lower_expr(ctx, fexpr);
            LirType ft = mix_to_lir(f->type);
            if (v.type != ft) {
                if (is_int_lir(v.type) && is_int_lir(ft)) {
                    int r = lir_emit_conv(ctx->fn, fexpr->loc,
                                           ft == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                           v.type, ft, v);
                    v = lir_opnd_value(r, ft);
                } else if (is_int_lir(v.type) && is_float_lir(ft)) {
                    LirOpnd w = widen_to_i64(ctx, fexpr->loc, v, fexpr->resolved_type);
                    int r = lir_emit_conv(ctx->fn, fexpr->loc, LIR_CONV_SITOFP,
                                           LIR_TY_I64, LIR_TY_F64, w);
                    v = float_cast(ctx, fexpr->loc,
                                     lir_opnd_value(r, LIR_TY_F64), ft);
                } else if (is_float_lir(v.type) && is_float_lir(ft)) {
                    v = float_cast(ctx, fexpr->loc, v, ft);
                } else if (v.type == LIR_TY_PTR && is_int_lir(ft)) {
                    int r = lir_emit_conv(ctx->fn, fexpr->loc, LIR_CONV_PTRTOINT,
                                           LIR_TY_PTR, ft, v);
                    v = lir_opnd_value(r, ft);
                }
            }
            lir_emit_store(ctx->fn, fexpr->loc, ft, v, field_addr);
        }
        return;
    }

    // Walk every field arg: compute field address, lower the value,
    // store / memcpy into the slot.
    for (int i = 0; i < lit->shape_lit.field_count; i++) {
        const char *fname = lit->shape_lit.field_names[i];
        AstNode    *fexpr = lit->shape_lit.field_values[i];
        ShapeFieldInfo *f = find_field(shape_type, fname);
        if (!f) {
            unsupported(lit, "shape literal field not in shape");
            return;
        }
        LirOpnd field_addr = lir_opnd_value(
            lir_emit_ptr_offset(ctx->fn, fexpr->loc, dst, f->offset),
            LIR_TY_PTR);

        if (mix_is_shape(f->type)) {
            // Nested shape field: recurse with the field address as the new dst.
            lower_init_into(ctx, field_addr, f->type, fexpr);
            continue;
        }

        // Scalar field. Lower the value, coerce if needed, store.
        LirOpnd v = lower_expr(ctx, fexpr);
        LirType ft = mix_to_lir(f->type);
        if (v.type != ft) {
            if (is_int_lir(v.type) && is_int_lir(ft)) {
                int r = lir_emit_conv(ctx->fn, fexpr->loc,
                                       ft == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                       v.type, ft, v);
                v = lir_opnd_value(r, ft);
            } else if (v.type == LIR_TY_PTR && is_int_lir(ft)) {
                // Function-pointer-as-int field (e.g. `fn: int` holding @dbl).
                int r = lir_emit_conv(ctx->fn, fexpr->loc, LIR_CONV_PTRTOINT,
                                       LIR_TY_PTR, LIR_TY_I64, v);
                LirOpnd as_i64 = lir_opnd_value(r, LIR_TY_I64);
                if (ft != LIR_TY_I64) {
                    int r2 = lir_emit_conv(ctx->fn, fexpr->loc, LIR_CONV_TRUNC,
                                             LIR_TY_I64, ft, as_i64);
                    v = lir_opnd_value(r2, ft);
                } else {
                    v = as_i64;
                }
            } else if (is_int_lir(v.type) && ft == LIR_TY_PTR) {
                int r = lir_emit_conv(ctx->fn, fexpr->loc, LIR_CONV_INTTOPTR,
                                       v.type, LIR_TY_PTR, v);
                v = lir_opnd_value(r, LIR_TY_PTR);
            } else if (is_int_lir(v.type) && is_float_lir(ft)) {
                LirOpnd w = widen_to_i64(ctx, fexpr->loc, v, fexpr->resolved_type);
                int r = lir_emit_conv(ctx->fn, fexpr->loc, LIR_CONV_SITOFP,
                                       LIR_TY_I64, LIR_TY_F64, w);
                v = float_cast(ctx, fexpr->loc,
                                 lir_opnd_value(r, LIR_TY_F64), ft);
            } else if (is_float_lir(v.type) && is_float_lir(ft)) {
                v = float_cast(ctx, fexpr->loc, v, ft);
            }
        }
        lir_emit_store(ctx->fn, fexpr->loc, ft, v, field_addr);
    }
}

// ---- Calls (forward-decl helpers) ------------------------------------------

static LirOpnd lower_user_call_into(LowerCtx *ctx, LirOpnd dst,
                                     MixType *shape_ret, AstNode *call);
static LirOpnd lower_user_call(LowerCtx *ctx, AstNode *call);

static void lower_shape_call_into(LowerCtx *ctx, LirOpnd dst,
                                   MixType *shape_type, AstNode *call)
{
    (void)shape_type;
    lower_user_call_into(ctx, dst, call->resolved_type, call);
}

// Look up a callee's per-param mutability in the symbol table. Returns
// NULL when the callee isn't a known TYPE_FUNC (e.g., builtins). Sema
// guarantees the array length matches the call's positional arg count.
static const bool *callee_param_mutable(LowerCtx *ctx, const char *name) {
    if (!ctx->symtab) return NULL;
    Symbol *s = symtab_lookup(ctx->symtab, name);
    if (!s || !s->type || s->type->kind != TYPE_FUNC) return NULL;
    return s->type->func.param_mutable;
}

// Lower a user-defined call. `dst` is the result-pointer when the callee
// returns a shape; ignored otherwise.
static LirOpnd lower_user_call_into(LowerCtx *ctx, LirOpnd dst,
                                     MixType *shape_ret, AstNode *call)
{
    AstNode **args = call->call.args;
    int n = call->call.arg_count;

    // Arg lowering. For each shape-typed arg, we materialize a temporary
    // and pass its pointer. Other args are lowered directly. Mutable
    // params skip the temp materialization and pass the caller's storage
    // pointer directly so writes propagate back.
    int extra = (shape_ret ? 1 : 0);
    int total_args = extra + n;

    LirOpnd *lir_args = NULL;
    LirType *param_types = NULL;
    if (total_args > 0) {
        lir_args = arena_alloc(ctx->mod->arena, total_args * sizeof(LirOpnd));
        param_types = arena_alloc(ctx->mod->arena, total_args * sizeof(LirType));
    }

    int idx = 0;
    if (shape_ret) {
        lir_args[idx] = dst;
        param_types[idx] = LIR_TY_PTR;
        idx++;
    }

    const bool *param_mut = callee_param_mutable(ctx, call->call.name);

    for (int i = 0; i < n; i++) {
        AstNode *a = args[i];
        MixType *at = a->resolved_type;
        bool arg_is_mut = param_mut ? param_mut[i] : false;

        if (arg_is_mut && a->kind == NODE_IDENT) {
            // Mutable param: pass the caller's storage ptr directly.
            Local *l = scope_lookup(ctx, a->ident.name);
            if (!l) {
                unsupported(a, "mutable arg referencing unknown local");
                return lir_opnd_none();
            }
            // For shape-typed locals the alloca slot already IS the shape
            // pointer.
            // For mut scalar params (e.g. `v!: int`), the slot is the
            // backing storage and must be passed directly so the callee
            // can write through it.
            // For box/ref locals where the scalar_type is PTR (the slot
            // stores a heap-payload pointer, e.g. `a! = box(...)`), we
            // need to load the slot AND mix_box_check before passing.
            if (l->shape_type) {
                lir_args[idx] = lir_opnd_value(l->value_id, LIR_TY_PTR);
            } else if (l->scalar_type == LIR_TY_PTR && needs_box_check(at)) {
                int v = lir_emit_load(ctx->fn, a->loc, LIR_TY_PTR,
                                       lir_opnd_value(l->value_id, LIR_TY_PTR));
                LirOpnd lv = lir_opnd_value(v, LIR_TY_PTR);
                lir_args[idx] = unwrap_box_runtime(ctx, a->loc, lv, at);
            } else {
                lir_args[idx] = lir_opnd_value(l->value_id, LIR_TY_PTR);
            }
            param_types[idx] = LIR_TY_PTR;
        } else if (arg_is_mut && a->kind == NODE_FIELD_EXPR) {
            // Mutable scalar arg accessed via a field expression — pass
            // the field address (GEP) instead of the loaded value so the
            // callee writes back to the caller's storage.
            ShapeFieldInfo *f = NULL;
            LirOpnd field_addr = field_address(ctx, a, &f);
            if (!f) return lir_opnd_none();
            lir_args[idx] = field_addr;
            param_types[idx] = LIR_TY_PTR;
        } else if (mix_is_shape(at)) {
            int tmp = lir_emit_alloca_bytes(ctx->fn, a->loc,
                                              at->shape.total_size,
                                              at->shape.alignment);
            LirOpnd tmp_p = lir_opnd_value(tmp, LIR_TY_PTR);
            lower_init_into(ctx, tmp_p, at, a);
            // Pass-by-value to extern C: small int-only structs go in an
            // integer register per the C ABI. We can only tell here if
            // the callee is *not* a MIX-defined function in this module.
            // cbind-imported C functions and prior pre-registered extern
            // decls land in this branch.
            bool callee_is_local = false;
            for (int fi = 0; fi < ctx->mod->func_count; fi++) {
                if (strcmp(ctx->mod->funcs[fi]->name, call->call.name) == 0) {
                    callee_is_local = true;
                    break;
                }
            }
            LirType ival = callee_is_local ? LIR_TY_VOID
                                            : shape_int_value_lir(at);
            if (ival != LIR_TY_VOID) {
                int v = lir_emit_load(ctx->fn, a->loc, ival, tmp_p);
                lir_args[idx] = lir_opnd_value(v, ival);
                param_types[idx] = ival;
            } else {
                lir_args[idx] = tmp_p;
                param_types[idx] = LIR_TY_PTR;
            }
        } else {
            lir_args[idx] = lower_expr(ctx, a);
            param_types[idx] = lir_args[idx].type;
        }
        idx++;
    }
    if (mix_error_count() > 0) return lir_opnd_none();

    // Resolve return type and coerce args. Prefer existing callee
    // registration (e.g., extern decls processed earlier).
    LirType ret_type = LIR_TY_VOID;
    bool found = false;
    LirCalleeDecl *existing = NULL;
    for (int i = 0; i < ctx->mod->callee_count; i++) {
        if (strcmp(ctx->mod->callees[i].name, call->call.name) == 0) {
            existing = &ctx->mod->callees[i];
            ret_type = existing->return_type;
            found = true;
            break;
        }
    }
    if (existing && existing->param_count == total_args) {
        // Coerce each arg to the registered param type to avoid IR
        // signature mismatches (commonly an extern's int32 vs MIX int's
        // i64 lowering, or a fn-ptr passed where `int` is declared).
        for (int i = 0; i < total_args; i++) {
            LirType expected = existing->param_types[i];
            if (lir_args[i].type == expected) continue;
            if (is_int_lir(lir_args[i].type) && is_int_lir(expected)) {
                int sw = (lir_args[i].type==LIR_TY_I1)?1:(lir_args[i].type==LIR_TY_I8)?8:(lir_args[i].type==LIR_TY_I32)?32:64;
                int dw = (expected==LIR_TY_I1)?1:(expected==LIR_TY_I8)?8:(expected==LIR_TY_I32)?32:64;
                LirConvKind k = (dw < sw) ? LIR_CONV_TRUNC : LIR_CONV_SEXT;
                int r = lir_emit_conv(ctx->fn, call->loc, k,
                                        lir_args[i].type, expected, lir_args[i]);
                lir_args[i] = lir_opnd_value(r, expected);
            } else if (lir_args[i].type == LIR_TY_PTR && is_int_lir(expected)) {
                // Two cases share this branch:
                // (1) Small-struct-by-value to an extern C callee — the
                //     arg is a shape ptr (alloca slot), the callee
                //     declares the int type the C ABI uses for the
                //     struct (i8/i32/i64). Load the slot as that int.
                // (2) Function pointer (ptr) being squeezed into an int
                //     param, e.g. `apply(dbl, 21)` where apply takes
                //     `f: int`. Use ptrtoint.
                MixType *arg_mix = (i >= extra && args[i - extra])
                    ? args[i - extra]->resolved_type : NULL;
                bool arg_is_shape = arg_mix && arg_mix->kind == TYPE_SHAPE;
                if (arg_is_shape) {
                    int v = lir_emit_load(ctx->fn, call->loc, expected, lir_args[i]);
                    lir_args[i] = lir_opnd_value(v, expected);
                } else {
                    // ptr → i64 → narrow if needed.
                    int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_PTRTOINT,
                                            LIR_TY_PTR, LIR_TY_I64, lir_args[i]);
                    LirOpnd as_i64 = lir_opnd_value(r, LIR_TY_I64);
                    if (expected == LIR_TY_I64) {
                        lir_args[i] = as_i64;
                    } else {
                        int r2 = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_TRUNC,
                                                 LIR_TY_I64, expected, as_i64);
                        lir_args[i] = lir_opnd_value(r2, expected);
                    }
                }
            } else if (is_int_lir(lir_args[i].type) && expected == LIR_TY_PTR) {
                // int → i64 → ptr.
                LirOpnd v = lir_args[i];
                if (v.type != LIR_TY_I64) {
                    int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_SEXT,
                                            v.type, LIR_TY_I64, v);
                    v = lir_opnd_value(r, LIR_TY_I64);
                }
                int r2 = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_INTTOPTR,
                                         LIR_TY_I64, LIR_TY_PTR, v);
                lir_args[i] = lir_opnd_value(r2, LIR_TY_PTR);
            } else if (is_int_lir(lir_args[i].type) && is_float_lir(expected)) {
                // int → f64 → maybe fptrunc to f32.
                LirOpnd w = widen_to_i64(ctx, call->loc, lir_args[i],
                                            args[i - extra]->resolved_type);
                int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_SITOFP,
                                        LIR_TY_I64, LIR_TY_F64, w);
                lir_args[i] = float_cast(ctx, call->loc,
                                            lir_opnd_value(r, LIR_TY_F64),
                                            expected);
            } else if (is_float_lir(lir_args[i].type) && is_int_lir(expected)) {
                // f → f64 → fptosi → maybe trunc.
                LirOpnd as_f64 = float_cast(ctx, call->loc,
                                              lir_args[i], LIR_TY_F64);
                int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_FPTOSI,
                                        LIR_TY_F64, LIR_TY_I64, as_f64);
                LirOpnd as_i64 = lir_opnd_value(r, LIR_TY_I64);
                if (expected == LIR_TY_I64) {
                    lir_args[i] = as_i64;
                } else {
                    int r2 = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_TRUNC,
                                             LIR_TY_I64, expected, as_i64);
                    lir_args[i] = lir_opnd_value(r2, expected);
                }
            } else if (is_float_lir(lir_args[i].type) && is_float_lir(expected)) {
                lir_args[i] = float_cast(ctx, call->loc, lir_args[i], expected);
            }
        }
    }
    if (!found) {
        ret_type = shape_ret ? LIR_TY_VOID : mix_to_lir(call->resolved_type);
        lir_register_callee(ctx->mod, call->loc, call->call.name,
                             ret_type, param_types, total_args);
    }

    int rid = lir_emit_call(ctx->fn, call->loc, call->call.name,
                              ret_type, lir_args, total_args);
    if (rid < 0) return lir_opnd_none();
    return lir_opnd_value(rid, ret_type);
}

static LirOpnd lower_user_call(LowerCtx *ctx, AstNode *call) {
    MixType *rt = call->resolved_type;
    if (mix_is_shape(rt)) {
        // Materialize into a temp; return the temp's ptr as the call's value.
        int tmp = lir_emit_alloca_bytes(ctx->fn, call->loc,
                                          rt->shape.total_size,
                                          rt->shape.alignment);
        LirOpnd tmp_p = lir_opnd_value(tmp, LIR_TY_PTR);
        lower_user_call_into(ctx, tmp_p, rt, call);
        return tmp_p;
    }
    return lower_user_call_into(ctx, lir_opnd_none(), NULL, call);
}

// Set of built-in names. Lookup is O(N) over a tiny static table; the
// alternative is to lower args eagerly and discover at the bottom that
// nothing matched, which double-emits IR for collection literal args.
static bool is_known_builtin(const char *name) {
    static const char *names[] = {
        "abs","alloc","args","assert","atan2","box","bytes",
        "ceil","chr","cos","env","exit","exp",
        "file_close","file_exists","file_open","file_read","file_read_all",
        "file_write","file_write_all","floor","free_mem","getcwd",
        "int_to_bin","int_to_hex","len","list_dir","list_to_f32","log",
        "max","memcpy","min","mkdir","ord","pack2","pack3","panic",
        "peek_byte","peek_f32","peek_ptr","peek_u32",
        "poke_f32","poke_ptr","poke_u32","pow","promote",
        "random_float","random_int","random_seed","round","shell",
        "shell_output","sin","sizeof","sqrt","str_count","str_reverse",
        "tan","time_now_ms","to_float","to_int","to_set","to_str",
        "to_string","type_of",
        "zone_create","zone_destroy","zone_reset","zone_alloc",
        NULL
    };
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0) return true;
    return false;
}

// Lower a built-in MIX call (`bytes`, `to_int`, `peek_u32`, ...) to a
// runtime helper. Returns true if handled, false otherwise.
static bool lower_builtin(LowerCtx *ctx, AstNode *call, LirOpnd *out) {
    const char *name = call->call.name;
    int n = call->call.arg_count;
    SrcLoc loc = call->loc;

    // Fast reject: skip eager arg lowering for non-builtin names so we
    // don't double-emit IR when lower_user_call lowers the same args.
    if (!is_known_builtin(name)) return false;

    // Lower args eagerly for the common case.
    LirOpnd args[8] = {0};
    if (n > 8) return false;
    for (int i = 0; i < n; i++) args[i] = lower_expr(ctx, call->call.args[i]);

    // Helper: emit a runtime call with given signature.
    #define BUILTIN(rt_name, ret_ty, ...) do { \
        LirType ps[] = { __VA_ARGS__ }; \
        int ns = (int)(sizeof(ps)/sizeof(ps[0])); \
        register_runtime(ctx->mod, loc, rt_name, ret_ty, ps, ns); \
        int r = lir_emit_call(ctx->fn, loc, rt_name, ret_ty, args, ns); \
        if (ret_ty == LIR_TY_VOID) *out = lir_opnd_none(); \
        else *out = lir_opnd_value(r, ret_ty); \
    } while (0)

    if (strcmp(name, "bytes") == 0 && n == 1)         { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_bytes", LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "alloc") == 0 && n == 1)         { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_alloc", LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "free_mem") == 0 && n == 1)      { BUILTIN("mix_free", LIR_TY_VOID, LIR_TY_PTR); return true; }
    if (strcmp(name, "memcpy") == 0 && n == 3)        { args[2] = to_i64(ctx,loc,args[2]); BUILTIN("mix_memcpy", LIR_TY_VOID, LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "exit") == 0 && n == 1)          { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_exit", LIR_TY_VOID, LIR_TY_I64); return true; }
    if (strcmp(name, "panic") == 0 && n == 1)         { BUILTIN("mix_panic", LIR_TY_VOID, LIR_TY_PTR); return true; }
    if (strcmp(name, "assert") == 0 && n == 2)        { args[0] = to_i32(ctx,loc,args[0]); BUILTIN("mix_assert", LIR_TY_VOID, LIR_TY_I32, LIR_TY_PTR); return true; }
    if (strcmp(name, "to_int") == 0 && n == 1) {
        // float→int or int→int; reuse cast logic.
        AstNode *a = call->call.args[0];
        if (args[0].type == LIR_TY_F64) {
            int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_FPTOSI, LIR_TY_F64, LIR_TY_I64, args[0]);
            *out = lir_opnd_value(r, LIR_TY_I64);
            return true;
        }
        if (args[0].type == LIR_TY_PTR) {
            // Strings → int via mix_parse_int (best-effort name).
            BUILTIN("mix_parse_int", LIR_TY_I64, LIR_TY_PTR);
            return true;
        }
        *out = widen_to_i64(ctx, loc, args[0], a->resolved_type);
        return true;
    }
    if (strcmp(name, "to_float") == 0 && n == 1) {
        if (args[0].type == LIR_TY_F64) { *out = args[0]; return true; }
        if (is_int_lir(args[0].type)) {
            LirOpnd w = widen_to_i64(ctx, loc, args[0], call->call.args[0]->resolved_type);
            int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_SITOFP, LIR_TY_I64, LIR_TY_F64, w);
            *out = lir_opnd_value(r, LIR_TY_F64);
            return true;
        }
        if (args[0].type == LIR_TY_PTR) {
            BUILTIN("mix_parse_float", LIR_TY_F64, LIR_TY_PTR);
            return true;
        }
        *out = args[0];
        return true;
    }
    if (strcmp(name, "to_string") == 0 && n == 1) {
        AstNode *a = call->call.args[0];
        MixType *at = a ? a->resolved_type : NULL;
        if (args[0].type == LIR_TY_F64) { BUILTIN("mix_to_string_float", LIR_TY_PTR, LIR_TY_F64); return true; }
        // bool widens to int — matches QBE which prints `to_string(true)` as "1".
        if (is_int_lir(args[0].type))   { args[0] = widen_to_i64(ctx, loc, args[0], at); BUILTIN("mix_to_string_int", LIR_TY_PTR, LIR_TY_I64); return true; }
        if (args[0].type == LIR_TY_PTR) { *out = args[0]; return true; }
    }
    if (strcmp(name, "to_str") == 0 && n == 1) {
        // Alias for to_string.
        AstNode *a = call->call.args[0];
        MixType *at = a ? a->resolved_type : NULL;
        if (args[0].type == LIR_TY_F64) { BUILTIN("mix_to_string_float", LIR_TY_PTR, LIR_TY_F64); return true; }
        // bool widens to int — matches QBE which prints `to_string(true)` as "1".
        if (is_int_lir(args[0].type))   { args[0] = widen_to_i64(ctx, loc, args[0], at); BUILTIN("mix_to_string_int", LIR_TY_PTR, LIR_TY_I64); return true; }
        if (args[0].type == LIR_TY_PTR) { *out = args[0]; return true; }
    }

    // Memory peek/poke
    if (strcmp(name, "peek_u32") == 0 && n == 2)      { args[1] = to_i64(ctx,loc,args[1]); BUILTIN("mix_peek_u32_at", LIR_TY_I64, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "peek_byte") == 0 && n == 2)     { args[1] = to_i64(ctx,loc,args[1]); BUILTIN("mix_peek_byte", LIR_TY_I64, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "peek_ptr") == 0 && n == 2)      { args[1] = to_i64(ctx,loc,args[1]); BUILTIN("mix_peek_ptr", LIR_TY_I64, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "peek_f32") == 0 && n == 2)      { args[1] = to_i64(ctx,loc,args[1]); BUILTIN("mix_peek_f32", LIR_TY_F64, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "poke_u32") == 0 && n == 3)      { args[1] = to_i64(ctx,loc,args[1]); args[2] = to_i64(ctx,loc,args[2]); BUILTIN("mix_poke_u32", LIR_TY_VOID, LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64); return true; }
    if (strcmp(name, "poke_ptr") == 0 && n == 3)      { args[1] = to_i64(ctx,loc,args[1]); args[2] = to_i64(ctx,loc,args[2]); BUILTIN("mix_poke_ptr", LIR_TY_VOID, LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64); return true; }
    if (strcmp(name, "poke_f32") == 0 && n == 3) {
        args[1] = to_i64(ctx,loc,args[1]);
        // Widen int→f64 if needed.
        if (is_int_lir(args[2].type)) {
            LirOpnd w = widen_to_i64(ctx, loc, args[2], call->call.args[2]->resolved_type);
            int r = lir_emit_conv(ctx->fn, loc, LIR_CONV_SITOFP, LIR_TY_I64, LIR_TY_F64, w);
            args[2] = lir_opnd_value(r, LIR_TY_F64);
        }
        BUILTIN("mix_poke_f32", LIR_TY_VOID, LIR_TY_PTR, LIR_TY_I64, LIR_TY_F64);
        return true;
    }
    if (strcmp(name, "pack2") == 0 && n == 3)         { args[2] = to_i64(ctx,loc,args[2]); BUILTIN("mix_pack2", LIR_TY_PTR, LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "pack3") == 0 && n == 4)         { args[3] = to_i64(ctx,loc,args[3]); BUILTIN("mix_pack3", LIR_TY_PTR, LIR_TY_PTR, LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64); return true; }

    // Strings
    if (strcmp(name, "chr") == 0 && n == 1)           { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_chr", LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "ord") == 0 && n == 1)           { BUILTIN("mix_ord", LIR_TY_I64, LIR_TY_PTR); return true; }
    if (strcmp(name, "int_to_bin") == 0 && n == 1)    { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_int_to_bin", LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "int_to_hex") == 0 && n == 1)    { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_int_to_hex", LIR_TY_PTR, LIR_TY_I64); return true; }

    // Zones
    if (strcmp(name, "zone_create") == 0 && n == 2)   { args[1] = to_i64(ctx,loc,args[1]); BUILTIN("zone_create", LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64); return true; }
    if (strcmp(name, "zone_destroy") == 0 && n == 1)  { BUILTIN("zone_destroy", LIR_TY_VOID, LIR_TY_PTR); return true; }
    if (strcmp(name, "zone_reset") == 0 && n == 1)    { BUILTIN("zone_reset", LIR_TY_VOID, LIR_TY_PTR); return true; }
    if (strcmp(name, "zone_alloc") == 0 && n == 2)    { args[1] = to_i64(ctx,loc,args[1]); BUILTIN("zone_alloc", LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64); return true; }

    // Box: box(zone, value) → mix_box_clone(zone, src_ptr, byte_size)
    if ((strcmp(name, "box") == 0 || strcmp(name, "promote") == 0) && n == 2) {
        AstNode *src = call->call.args[1];
        MixType *src_t = src->resolved_type;
        // Unwrap ref/box wrappers down to the underlying shape.
        MixType *boxed_t = src_t;
        while (boxed_t && (boxed_t->kind == TYPE_REF || boxed_t->kind == TYPE_BOX)) {
            boxed_t = boxed_t->kind == TYPE_REF ? boxed_t->ref.base : boxed_t->box.inner;
        }
        int sz = type_size(boxed_t);
        // src may already be a ptr (shape value); if it's a scalar, spill
        // it first. For boxes, fetch the payload pointer via mix_box_check.
        LirOpnd src_ptr = args[1];
        if (src_ptr.type != LIR_TY_PTR) {
            int slot = lir_emit_alloca(ctx->fn, loc, src_ptr.type);
            lir_emit_store(ctx->fn, loc, src_ptr.type, src_ptr,
                            lir_opnd_value(slot, LIR_TY_PTR));
            src_ptr = lir_opnd_value(slot, LIR_TY_PTR);
        }
        src_ptr = unwrap_box_runtime(ctx, loc, src_ptr, src_t);
        LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, loc, "mix_box_clone", LIR_TY_PTR, ps, 3);
        LirOpnd cargs[] = { args[0], src_ptr, lir_opnd_int_typed(sz, LIR_TY_I64) };
        int r = lir_emit_call(ctx->fn, loc, "mix_box_clone", LIR_TY_PTR, cargs, 3);
        *out = lir_opnd_value(r, LIR_TY_PTR);
        return true;
    }

    // Time / random
    if (strcmp(name, "time_now_ms") == 0 && n == 0)   { BUILTIN("mix_time_now_ms", LIR_TY_I64); return true; }
    if (strcmp(name, "random_int") == 0 && n == 0)    { BUILTIN("mix_random_int", LIR_TY_I64); return true; }
    if (strcmp(name, "random_float") == 0 && n == 0)  { BUILTIN("mix_random_float", LIR_TY_F64); return true; }
    if (strcmp(name, "random_seed") == 0 && n == 1)   { args[0] = to_i64(ctx,loc,args[0]); BUILTIN("mix_random_seed", LIR_TY_VOID, LIR_TY_I64); return true; }

    // System
    if (strcmp(name, "args") == 0 && n == 0)          { BUILTIN("mix_args", LIR_TY_PTR); return true; }
    if (strcmp(name, "env") == 0 && n == 1)           { BUILTIN("mix_env", LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "getcwd") == 0 && n == 0)        { BUILTIN("mix_getcwd", LIR_TY_PTR); return true; }
    if (strcmp(name, "shell") == 0 && n == 1)         { BUILTIN("mix_shell", LIR_TY_I64, LIR_TY_PTR); return true; }
    if (strcmp(name, "shell_output") == 0 && n == 1)  { BUILTIN("mix_shell_output", LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "list_dir") == 0 && n == 1)      { BUILTIN("mix_list_dir", LIR_TY_PTR, LIR_TY_PTR); return true; }

    // File I/O
    if (strcmp(name, "file_open") == 0 && n == 2)     { BUILTIN("mix_file_open", LIR_TY_PTR, LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "file_read") == 0 && n == 1)     { BUILTIN("mix_file_read", LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "file_read_all") == 0 && n == 1) { BUILTIN("mix_file_read_all", LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "file_write") == 0 && n == 2)    { BUILTIN("mix_file_write", LIR_TY_VOID, LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "file_write_all") == 0 && n == 2){ BUILTIN("mix_file_write_all", LIR_TY_I32, LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "file_close") == 0 && n == 1)    { BUILTIN("mix_file_close", LIR_TY_VOID, LIR_TY_PTR); return true; }
    if (strcmp(name, "file_exists") == 0 && n == 1)   { BUILTIN("mix_file_exists", LIR_TY_I32, LIR_TY_PTR); return true; }
    if (strcmp(name, "mkdir") == 0 && n == 1)         { BUILTIN("mix_mkdir", LIR_TY_I32, LIR_TY_PTR); return true; }

    // List builtins (function-call style)
    if (strcmp(name, "list_to_f32") == 0 && n == 1)   { BUILTIN("mix_list_to_f32", LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "to_set") == 0 && n == 1) {
        // Pick the int- or string-keyed set helper based on element type.
        MixType *at = call->call.args[0]->resolved_type;
        MixType *elem = (at && at->kind == TYPE_LIST) ? at->list.elem_type : NULL;
        const char *helper = (elem && elem->kind == TYPE_STR)
            ? "mix_set_from_list" : "mix_set_from_list_int";
        BUILTIN(helper, LIR_TY_PTR, LIR_TY_PTR);
        return true;
    }

    // String builtins (function-call style)
    if (strcmp(name, "str_count") == 0 && n == 2)     { BUILTIN("mix_str_count", LIR_TY_I64, LIR_TY_PTR, LIR_TY_PTR); return true; }
    if (strcmp(name, "str_reverse") == 0 && n == 1)   { BUILTIN("mix_str_reverse", LIR_TY_PTR, LIR_TY_PTR); return true; }

    // len(x): generic dispatch by arg type
    if (strcmp(name, "len") == 0 && n == 1) {
        AstNode *a = call->call.args[0];
        MixType *at = a ? a->resolved_type : NULL;
        if (at && at->kind == TYPE_LIST) { BUILTIN("mix_list_len", LIR_TY_I64, LIR_TY_PTR); return true; }
        if (at && at->kind == TYPE_MAP)  { BUILTIN("mix_map_len",  LIR_TY_I64, LIR_TY_PTR); return true; }
        if (at && at->kind == TYPE_SET)  { BUILTIN("mix_set_len",  LIR_TY_I64, LIR_TY_PTR); return true; }
        if (at && at->kind == TYPE_STR)  { BUILTIN("mix_str_len",  LIR_TY_I64, LIR_TY_PTR); return true; }
    }

    // sizeof(x): compile-time constant.
    if (strcmp(name, "sizeof") == 0 && n == 1) {
        AstNode *a = call->call.args[0];
        MixType *at = a ? a->resolved_type : NULL;
        int sz = type_size(at);
        *out = lir_opnd_int_typed(sz, LIR_TY_I64);
        return true;
    }
    // type_of(x): compile-time string of the type's name.
    if (strcmp(name, "type_of") == 0 && n == 1) {
        AstNode *a = call->call.args[0];
        MixType *at = a ? a->resolved_type : NULL;
        const char *tname = "unknown";
        if (at) {
            switch (at->kind) {
                case TYPE_INT: case TYPE_INT64:    tname = "int"; break;
                case TYPE_INT32: case TYPE_UINT32: tname = "int32"; break;
                case TYPE_INT8: case TYPE_UINT8:   tname = "int8"; break;
                case TYPE_INT16: case TYPE_UINT16: tname = "int16"; break;
                case TYPE_UINT64:                  tname = "uint64"; break;
                case TYPE_BYTE:                    tname = "byte"; break;
                case TYPE_BOOL:                    tname = "bool"; break;
                case TYPE_FLOAT: case TYPE_FLOAT64: tname = "float"; break;
                case TYPE_FLOAT32:                 tname = "float32"; break;
                case TYPE_STR:                     tname = "str"; break;
                case TYPE_PTR:                     tname = "ptr"; break;
                case TYPE_LIST:                    tname = "list"; break;
                case TYPE_MAP:                     tname = "map"; break;
                case TYPE_SET:                     tname = "set"; break;
                case TYPE_OPTIONAL:                tname = "optional"; break;
                case TYPE_SHAPE:                   tname = at->shape.name ? at->shape.name : "shape"; break;
                case TYPE_ZONE:                    tname = "Zone"; break;
                case TYPE_REF:                     tname = "ref"; break;
                case TYPE_BOX:                     tname = "Box"; break;
                case TYPE_FUNC:                    tname = "fn"; break;
                default:                           tname = "unknown"; break;
            }
        }
        int sid = lir_intern_string(ctx->mod, tname, (int)strlen(tname));
        *out = lir_opnd_string(sid);
        return true;
    }
    // promote(x): no-op for ints (returns x; QBE used it for ABI hinting).
    if (strcmp(name, "promote") == 0 && n == 1) {
        *out = args[0];
        return true;
    }
    // min/max — dispatch by operand type. Math helpers take doubles;
    // for ints, branch on a < b inline (cheaper than calling out).
    if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) && n == 2) {
        bool is_min = (name[1] == 'i');
        if (args[0].type == LIR_TY_F64 || args[1].type == LIR_TY_F64) {
            const char *fn = is_min ? "mix_math_min" : "mix_math_max";
            BUILTIN(fn, LIR_TY_F64, LIR_TY_F64, LIR_TY_F64);
            return true;
        }
        // Int min/max via select.
        LirOpnd a = widen_to_i64(ctx, loc, args[0], call->call.args[0]->resolved_type);
        LirOpnd b = widen_to_i64(ctx, loc, args[1], call->call.args[1]->resolved_type);
        int cmp_v = lir_emit_bin(ctx->fn, loc, is_min ? LIR_BIN_LT : LIR_BIN_GT,
                                   LIR_TY_I64, a, b);
        // Use branch+phi-via-alloca to select.
        int slot = lir_emit_alloca(ctx->fn, loc, LIR_TY_I64);
        int then_l = lir_func_new_label(ctx->fn);
        int else_l = lir_func_new_label(ctx->fn);
        int merge_l = lir_func_new_label(ctx->fn);
        emit_br_cond(ctx, loc, lir_opnd_value(cmp_v, LIR_TY_I1), then_l, else_l);
        emit_label(ctx, loc, then_l);
        lir_emit_store(ctx->fn, loc, LIR_TY_I64, a, lir_opnd_value(slot, LIR_TY_PTR));
        emit_br(ctx, loc, merge_l);
        emit_label(ctx, loc, else_l);
        lir_emit_store(ctx->fn, loc, LIR_TY_I64, b, lir_opnd_value(slot, LIR_TY_PTR));
        emit_br(ctx, loc, merge_l);
        emit_label(ctx, loc, merge_l);
        int v = lir_emit_load(ctx->fn, loc, LIR_TY_I64,
                                lir_opnd_value(slot, LIR_TY_PTR));
        *out = lir_opnd_value(v, LIR_TY_I64);
        return true;
    }
    if (strcmp(name, "abs") == 0 && n == 1) {
        if (args[0].type == LIR_TY_F64) { BUILTIN("mix_math_abs", LIR_TY_F64, LIR_TY_F64); return true; }
        // Int abs via branch.
        LirOpnd a = widen_to_i64(ctx, loc, args[0], call->call.args[0]->resolved_type);
        int neg_v = lir_emit_un(ctx->fn, loc, LIR_UN_NEG, LIR_TY_I64, a);
        int cmp_v = lir_emit_bin(ctx->fn, loc, LIR_BIN_LT, LIR_TY_I64,
                                   a, lir_opnd_int_typed(0, LIR_TY_I64));
        int slot = lir_emit_alloca(ctx->fn, loc, LIR_TY_I64);
        int then_l = lir_func_new_label(ctx->fn);
        int else_l = lir_func_new_label(ctx->fn);
        int merge_l = lir_func_new_label(ctx->fn);
        emit_br_cond(ctx, loc, lir_opnd_value(cmp_v, LIR_TY_I1), then_l, else_l);
        emit_label(ctx, loc, then_l);
        lir_emit_store(ctx->fn, loc, LIR_TY_I64, lir_opnd_value(neg_v, LIR_TY_I64),
                        lir_opnd_value(slot, LIR_TY_PTR));
        emit_br(ctx, loc, merge_l);
        emit_label(ctx, loc, else_l);
        lir_emit_store(ctx->fn, loc, LIR_TY_I64, a, lir_opnd_value(slot, LIR_TY_PTR));
        emit_br(ctx, loc, merge_l);
        emit_label(ctx, loc, merge_l);
        int v = lir_emit_load(ctx->fn, loc, LIR_TY_I64, lir_opnd_value(slot, LIR_TY_PTR));
        *out = lir_opnd_value(v, LIR_TY_I64);
        return true;
    }
    if (strcmp(name, "sqrt") == 0 && n == 1)  { BUILTIN("mix_math_sqrt", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "floor") == 0 && n == 1) { BUILTIN("mix_math_floor", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "ceil") == 0 && n == 1)  { BUILTIN("mix_math_ceil", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "round") == 0 && n == 1) { BUILTIN("mix_math_round", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "sin") == 0 && n == 1)   { BUILTIN("mix_math_sin", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "cos") == 0 && n == 1)   { BUILTIN("mix_math_cos", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "tan") == 0 && n == 1)   { BUILTIN("mix_math_tan", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "atan2") == 0 && n == 2) { BUILTIN("mix_math_atan2", LIR_TY_F64, LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "pow") == 0 && n == 2)   { BUILTIN("mix_math_pow", LIR_TY_F64, LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "log") == 0 && n == 1)   { BUILTIN("mix_math_log", LIR_TY_F64, LIR_TY_F64); return true; }
    if (strcmp(name, "exp") == 0 && n == 1)   { BUILTIN("mix_math_exp", LIR_TY_F64, LIR_TY_F64); return true; }

    #undef BUILTIN
    return false;
}

// Phase 5: indirect call through a function pointer held in a local
// variable (lambda var) or a parameter (`f` in `apply(f, x)`). Returns
// true if `name` resolves to such a callable; emits the call and writes
// the result into *out.
static bool lower_indirect_call_if_local(LowerCtx *ctx, AstNode *call,
                                           LirOpnd *out)
{
    const char *name = call->call.name;
    LirOpnd fp_i64 = lir_opnd_none();   // ptr-or-i64 source value
    LirType src_ty = LIR_TY_VOID;

    Local *l = scope_lookup(ctx, name);
    if (l) {
        // Locals are stored via alloca; load the slot. Skip shape-typed
        // locals — those are never callable.
        if (l->shape_type) return false;
        int v = lir_emit_load(ctx->fn, call->loc, l->scalar_type,
                                lir_opnd_value(l->value_id, LIR_TY_PTR));
        fp_i64 = lir_opnd_value(v, l->scalar_type);
        src_ty = l->scalar_type;
    } else {
        for (int i = 0; i < ctx->fn->param_count; i++) {
            if (strcmp(ctx->fn->param_names[i], name) == 0) {
                fp_i64 = lir_opnd_param(i, ctx->fn->param_types[i]);
                src_ty = ctx->fn->param_types[i];
                break;
            }
        }
    }
    if (fp_i64.kind == LIR_OPND_NONE) return false;

    // Coerce to ptr if needed.
    LirOpnd fp_ptr;
    if (src_ty == LIR_TY_PTR) {
        fp_ptr = fp_i64;
    } else if (is_int_lir(src_ty)) {
        // Widen narrow ints to i64 first if necessary, then inttoptr.
        LirOpnd w = fp_i64;
        if (src_ty != LIR_TY_I64) {
            int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_SEXT,
                                    src_ty, LIR_TY_I64, fp_i64);
            w = lir_opnd_value(r, LIR_TY_I64);
        }
        int p = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_INTTOPTR,
                                LIR_TY_I64, LIR_TY_PTR, w);
        fp_ptr = lir_opnd_value(p, LIR_TY_PTR);
    } else {
        return false;  // unsupported source type for callee ptr
    }

    // Lower args. Untyped lambdas use i64 for every param, so coerce each
    // arg to i64 (mirrors the lower_lambda signature).
    int n = call->call.arg_count;
    LirOpnd *args = NULL;
    LirType *param_types = NULL;
    if (n > 0) {
        args = arena_alloc(ctx->mod->arena, n * sizeof(LirOpnd));
        param_types = arena_alloc(ctx->mod->arena, n * sizeof(LirType));
    }
    for (int i = 0; i < n; i++) {
        LirOpnd a = lower_expr(ctx, call->call.args[i]);
        if (a.type == LIR_TY_F64) {
            int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_BITCAST,
                                    LIR_TY_F64, LIR_TY_I64, a);
            a = lir_opnd_value(r, LIR_TY_I64);
        } else if (a.type == LIR_TY_PTR) {
            int r = lir_emit_conv(ctx->fn, call->loc, LIR_CONV_PTRTOINT,
                                    LIR_TY_PTR, LIR_TY_I64, a);
            a = lir_opnd_value(r, LIR_TY_I64);
        } else if (is_int_lir(a.type) && a.type != LIR_TY_I64) {
            a = widen_to_i64(ctx, call->loc, a, call->call.args[i]->resolved_type);
        }
        args[i] = a;
        param_types[i] = LIR_TY_I64;
    }

    // Lambda return is i64 in our hoisted ABI. Leave the call value
    // i64-typed; downstream coercion (print dispatch, var-decl init)
    // handles narrowing/casting based on the AST resolved_type. This
    // avoids forcing a wrong cast when sema reports TYPE_INFER (which
    // would map to LIR_TY_PTR via mix_to_lir).
    int rid = lir_emit_call_indirect(ctx->fn, call->loc, fp_ptr,
                                       LIR_TY_I64, param_types, args, n);
    *out = (rid >= 0) ? lir_opnd_value(rid, LIR_TY_I64) : lir_opnd_none();
    return true;
}

static LirOpnd lower_call_expr(LowerCtx *ctx, AstNode *call) {
    if (!call->call.name) {
        unsupported(call, "call with no name");
        return lir_opnd_none();
    }
    if (strcmp(call->call.name, "print") == 0) {
        lower_print(ctx, call);
        return lir_opnd_none();
    }
    LirOpnd builtin_result;
    if (lower_builtin(ctx, call, &builtin_result)) return builtin_result;

    // Phase 5: indirect call through local var or param holding a fn ptr.
    LirOpnd indirect_result;
    if (lower_indirect_call_if_local(ctx, call, &indirect_result)) {
        return indirect_result;
    }

    return lower_user_call(ctx, call);
}

// ---- Field access ----------------------------------------------------------

// Unwrap ref/box wrappers down to the shape type they reference.
static MixType *unwrap_to_shape(MixType *t) {
    if (!t) return NULL;
    while (t) {
        if (t->kind == TYPE_SHAPE) return t;
        if (t->kind == TYPE_REF)   { t = t->ref.base;   continue; }
        if (t->kind == TYPE_BOX)   { t = t->box.inner;  continue; }
        if (t->kind == TYPE_OPTIONAL) { t = t->optional.inner; continue; }
        return NULL;
    }
    return NULL;
}

// Returns true if the type sits behind a box wrapper that needs a
// runtime mix_box_check() to fetch the underlying payload pointer
// (and to panic on stale-after-zone-reset access).
static bool needs_box_check(MixType *t) {
    while (t) {
        if (t->kind == TYPE_BOX) return true;
        if (t->kind == TYPE_REF) { t = t->ref.base; continue; }
        if (t->kind == TYPE_OPTIONAL) { t = t->optional.inner; continue; }
        return false;
    }
    return false;
}

// If `val` is a box handle, emit a mix_box_check call to extract the
// payload pointer. Caller passes the local's MIX type so we know whether
// to wrap. Returns the (possibly-checked) ptr.
static LirOpnd unwrap_box_runtime(LowerCtx *ctx, SrcLoc loc, LirOpnd val,
                                    MixType *t)
{
    if (!needs_box_check(t)) return val;
    LirType ps[] = { LIR_TY_PTR };
    register_runtime(ctx->mod, loc, "mix_box_check", LIR_TY_PTR, ps, 1);
    LirOpnd args[] = { val };
    int r = lir_emit_call(ctx->fn, loc, "mix_box_check", LIR_TY_PTR, args, 1);
    return lir_opnd_value(r, LIR_TY_PTR);
}

// Compute the address of `obj.field` as a ptr.
static LirOpnd field_address(LowerCtx *ctx, AstNode *fe, ShapeFieldInfo **fi_out) {
    AstNode *obj = fe->field_expr.object;
    MixType *obj_type = obj->resolved_type;
    MixType *shape_t = unwrap_to_shape(obj_type);
    if (!shape_t) {
        unsupported(fe, "field access on non-shape");
        return lir_opnd_none();
    }
    ShapeFieldInfo *f = find_field(shape_t, fe->field_expr.field_name);
    if (!f) {
        if (fi_out) *fi_out = NULL;
        return lir_opnd_none();
    }
    LirOpnd base = lower_expr(ctx, obj);
    if (base.type != LIR_TY_PTR) {
        unsupported(fe, "field access requires shape ptr");
        return lir_opnd_none();
    }
    // Unwrap box handles at runtime so the GEP operates on the payload.
    base = unwrap_box_runtime(ctx, fe->loc, base, obj_type);
    int addr = lir_emit_ptr_offset(ctx->fn, fe->loc, base, f->offset);
    if (fi_out) *fi_out = f;
    return lir_opnd_value(addr, LIR_TY_PTR);
}

static LirOpnd lower_field_expr(LowerCtx *ctx, AstNode *fe) {
    LirOpnd cv;
    if (lower_collection_field(ctx, fe, &cv)) return cv;

    ShapeFieldInfo *f = NULL;
    LirOpnd addr = field_address(ctx, fe, &f);
    if (f) {
        if (mix_is_shape(f->type)) return addr;
        LirType ft = mix_to_lir(f->type);
        int loaded = lir_emit_load(ctx->fn, fe->loc, ft, addr);
        LirOpnd v = lir_opnd_value(loaded, ft);
        // Promote f32 fields to f64 for downstream arithmetic — MIX
        // arithmetic uses double; f32 only exists at storage boundaries.
        if (ft == LIR_TY_F32) v = float_cast(ctx, fe->loc, v, LIR_TY_F64);
        return v;
    }
    // Computed field: name isn't a field but might be a 0-arg method.
    AstNode *obj = fe->field_expr.object;
    MixType *obj_type = obj ? obj->resolved_type : NULL;
    if (obj_type && obj_type->kind == TYPE_SHAPE) {
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s_%s",
                 obj_type->shape.name, fe->field_expr.field_name);
        Symbol *sym = symtab_lookup(ctx->symtab, mangled);
        if (sym && sym->type && sym->type->kind == TYPE_FUNC) {
            // Synthesize a NODE_METHOD_CALL with no args.
            AstNode *fake = arena_alloc(ctx->mod->arena, sizeof(AstNode));
            memset(fake, 0, sizeof(*fake));
            fake->kind = NODE_METHOD_CALL;
            fake->loc = fe->loc;
            fake->resolved_type = fe->resolved_type;
            fake->method_call.object = obj;
            fake->method_call.method_name = fe->field_expr.field_name;
            fake->method_call.args = NULL;
            fake->method_call.arg_count = 0;
            return lower_expr(ctx, fake);
        }
    }
    unsupported(fe, "field name not in shape");
    return lir_opnd_none();
}

// ---- Expressions -----------------------------------------------------------

static LirOpnd lower_expr(LowerCtx *ctx, AstNode *expr) {
    if (!expr) return lir_opnd_none();

    switch (expr->kind) {

    case NODE_INT_LIT: {
        LirType t = mix_to_lir(expr->resolved_type);
        if (t == LIR_TY_VOID) t = LIR_TY_I64;
        return lir_opnd_int_typed(expr->int_lit.value, t);
    }

    case NODE_FLOAT_LIT: return lir_opnd_f64(expr->float_lit.value);
    case NODE_BOOL_LIT:  return lir_opnd_bool(expr->bool_lit.value);

    case NODE_STRING_LIT: {
        int sid = lir_intern_string(ctx->mod, expr->string_lit.value,
                                     expr->string_lit.length);
        return lir_opnd_string(sid);
    }

    case NODE_IDENT: {
        const char *name = expr->ident.name;
        Local *l = scope_lookup(ctx, name);
        if (l) {
            if (l->shape_type) return lir_opnd_value(l->value_id, LIR_TY_PTR);
            int v = lir_emit_load(ctx->fn, expr->loc, l->scalar_type,
                                   lir_opnd_value(l->value_id, LIR_TY_PTR));
            return lir_opnd_value(v, l->scalar_type);
        }
        // Phase 5: bare ident in a method body that names a field of the
        // enclosing shape rewrites to `self.field`. Sema does the same
        // for NODE_ASSIGN; reads fall through to here.
        if (ctx->current_shape) {
            ShapeFieldInfo *f = find_field(ctx->current_shape, name);
            if (f) {
                Local *self = scope_lookup(ctx, "self");
                if (self && self->shape_type) {
                    LirOpnd self_p = lir_opnd_value(self->value_id, LIR_TY_PTR);
                    int faddr = lir_emit_ptr_offset(ctx->fn, expr->loc,
                                                      self_p, f->offset);
                    if (mix_is_shape(f->type)) {
                        return lir_opnd_value(faddr, LIR_TY_PTR);
                    }
                    LirType ft = mix_to_lir(f->type);
                    int v = lir_emit_load(ctx->fn, expr->loc, ft,
                                            lir_opnd_value(faddr, LIR_TY_PTR));
                    return lir_opnd_value(v, ft);
                }
            }
        }
        // Phase 5: top-level const lookup.
        for (int i = 0; i < g_const_count; i++) {
            if (strcmp(g_consts[i].name, name) == 0)
                return lower_expr(ctx, g_consts[i].value);
        }
        for (int i = 0; i < ctx->fn->param_count; i++) {
            if (strcmp(ctx->fn->param_names[i], name) == 0)
                return lir_opnd_param(i, ctx->fn->param_types[i]);
        }
        // Phase 5: top-level mutable global — load through the LLVM
        // global symbol.
        GlobalEntry *g = find_global(name);
        if (g) {
            int v = lir_emit_load(ctx->fn, expr->loc, g->lir_type,
                                   lir_opnd_fn_ref(g->name));
            return lir_opnd_value(v, g->lir_type);
        }
        // Phase 5: bare reference to a top-level function name (e.g.
        // `apply(dbl, 21)`) — produce the function symbol address.
        if (ctx->symtab) {
            Symbol *sym = symtab_lookup(ctx->symtab, name);
            if (sym && sym->type && sym->type->kind == TYPE_FUNC) {
                return lir_opnd_fn_ref(arena_strdup(ctx->mod->arena, name));
            }
            // Cross-module global: declare on demand.
            if (sym && sym->type && sym->type->kind != TYPE_FUNC &&
                sym->type->kind != TYPE_SHAPE && sym->type->kind != TYPE_NAMED) {
                LirType lt = mix_to_lir(sym->type);
                if (lt != LIR_TY_VOID) {
                    lir_module_add_global(ctx->mod, name, lt, false, 0, true);
                    if (g_global_count < 256) {
                        GlobalEntry *e2 = &g_globals[g_global_count++];
                        e2->name = arena_strdup(ctx->mod->arena, name);
                        e2->mix_type = sym->type;
                        e2->lir_type = lt;
                        e2->init_expr = NULL;
                        e2->is_pub = false;
                    }
                    int v = lir_emit_load(ctx->fn, expr->loc, lt,
                                           lir_opnd_fn_ref(name));
                    return lir_opnd_value(v, lt);
                }
            }
        }
        unsupported(expr, "unresolved identifier");
        return lir_opnd_none();
    }

    case NODE_BINARY_EXPR: {
        TokenKind tok = expr->binary.op;
        bool ok = false;
        LirBinOp op = tok_to_bin(tok, &ok);
        if (!ok) { unsupported(expr, "binary operator"); return lir_opnd_none(); }

        // Operator overloading on shapes: dispatch `a + b` (etc.) to
        // ShapeName_op_<name> when both operands are shapes (or LHS is
        // a shape with the corresponding op method defined). Translate
        // to a synthesized method call so the existing shape-method ABI
        // and shape-return sret transform handle the rest.
        MixType *lty = expr->binary.left ? expr->binary.left->resolved_type : NULL;
        if (lty && lty->kind == TYPE_SHAPE) {
            const char *opname = NULL;
            switch (tok) {
                case TOK_PLUS:    opname = "op_add"; break;
                case TOK_MINUS:   opname = "op_sub"; break;
                case TOK_STAR:    opname = "op_mul"; break;
                case TOK_SLASH:   opname = "op_div"; break;
                case TOK_PERCENT: opname = "op_mod"; break;
                case TOK_EQEQ:    opname = "op_eq"; break;
                case TOK_NEQ:     opname = "op_neq"; break;
                case TOK_LT:      opname = "op_lt"; break;
                case TOK_GT:      opname = "op_gt"; break;
                case TOK_LTE:     opname = "op_lte"; break;
                case TOK_GTE:     opname = "op_gte"; break;
                default: break;
            }
            if (opname && ctx->symtab) {
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s", lty->shape.name, opname);
                Symbol *sym = symtab_lookup(ctx->symtab, mangled);
                if (sym && sym->type && sym->type->kind == TYPE_FUNC) {
                    AstNode *fake = arena_alloc(ctx->mod->arena, sizeof(AstNode));
                    memset(fake, 0, sizeof(*fake));
                    fake->kind = NODE_CALL_EXPR;
                    fake->loc = expr->loc;
                    fake->resolved_type = expr->resolved_type;
                    fake->call.name = arena_strdup(ctx->mod->arena, mangled);
                    fake->call.arg_count = 2;
                    fake->call.args = arena_alloc(ctx->mod->arena, 2 * sizeof(AstNode *));
                    fake->call.args[0] = expr->binary.left;
                    fake->call.args[1] = expr->binary.right;
                    return lower_user_call(ctx, fake);
                }
            }
        }

        LirOpnd a = lower_expr(ctx, expr->binary.left);
        LirOpnd b = lower_expr(ctx, expr->binary.right);
        if (mix_error_count() > 0) return lir_opnd_none();

        // String concatenation: str + str → mix_str_concat.
        if (tok == TOK_PLUS && a.type == LIR_TY_PTR && b.type == LIR_TY_PTR) {
            MixType *lt = expr->binary.left ? expr->binary.left->resolved_type : NULL;
            MixType *rt = expr->binary.right ? expr->binary.right->resolved_type : NULL;
            if (lt && rt && lt->kind == TYPE_STR && rt->kind == TYPE_STR) {
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_concat", LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { a, b };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_concat",
                                        LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
        }
        // String comparison: == != < <= > >= → libc strcmp + integer cmp
        // (matches QBE backend; avoids needing a per-op runtime helper).
        if ((tok == TOK_EQEQ || tok == TOK_NEQ ||
             tok == TOK_LT   || tok == TOK_LTE ||
             tok == TOK_GT   || tok == TOK_GTE) &&
            a.type == LIR_TY_PTR && b.type == LIR_TY_PTR) {
            MixType *lt = expr->binary.left ? expr->binary.left->resolved_type : NULL;
            MixType *rt = expr->binary.right ? expr->binary.right->resolved_type : NULL;
            if (lt && rt && lt->kind == TYPE_STR && rt->kind == TYPE_STR) {
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "strcmp", LIR_TY_I32, ps, 2);
                LirOpnd args[] = { a, b };
                int r = lir_emit_call(ctx->fn, expr->loc, "strcmp",
                                        LIR_TY_I32, args, 2);
                LirOpnd cmp_v = lir_opnd_value(r, LIR_TY_I32);
                LirBinOp bin_op;
                switch (tok) {
                    case TOK_EQEQ: bin_op = LIR_BIN_EQ; break;
                    case TOK_NEQ:  bin_op = LIR_BIN_NE; break;
                    case TOK_LT:   bin_op = LIR_BIN_LT; break;
                    case TOK_LTE:  bin_op = LIR_BIN_LE; break;
                    case TOK_GT:   bin_op = LIR_BIN_GT; break;
                    case TOK_GTE:  bin_op = LIR_BIN_GE; break;
                    default:       bin_op = LIR_BIN_EQ; break;
                }
                int b_v = lir_emit_bin(ctx->fn, expr->loc, bin_op, LIR_TY_I32,
                                          cmp_v, lir_opnd_int_typed(0, LIR_TY_I32));
                return lir_opnd_value(b_v, LIR_TY_I1);
            }
        }

        // Pointer arithmetic: ptr + int (or int + ptr) → GEP i8.
        // Only for + and -. Subtraction by an int negates the offset
        // through a tiny synthesized neg.
        if ((tok == TOK_PLUS || tok == TOK_MINUS) &&
            (a.type == LIR_TY_PTR || b.type == LIR_TY_PTR) &&
            !(a.type == LIR_TY_PTR && b.type == LIR_TY_PTR))
        {
            LirOpnd ptr = (a.type == LIR_TY_PTR) ? a : b;
            LirOpnd off = (a.type == LIR_TY_PTR) ? b : a;
            // PTR_OFFSET takes an immediate offset. If `off` is an
            // immediate I64 we can use it directly; otherwise we need a
            // separate "GEP with value" op. For Phase 5 we cover the
            // immediate case (sufficient for `buf + 8`).
            if (off.kind == LIR_OPND_I64) {
                long long o = off.imm;
                if (tok == TOK_MINUS) o = -o;
                int r = lir_emit_ptr_offset(ctx->fn, expr->loc, ptr, (int)o);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // Fall through to the int-add path with the ptr cast to i64
            // as a fallback. Less typed but gets the right address.
            int pi = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_PTRTOINT,
                                     LIR_TY_PTR, LIR_TY_I64, ptr);
            LirOpnd pi_o = lir_opnd_value(pi, LIR_TY_I64);
            LirOpnd off_i64 = to_i64(ctx, expr->loc, off);
            LirBinOp add_op = (tok == TOK_MINUS) ? LIR_BIN_SUB : LIR_BIN_ADD;
            int sum = lir_emit_bin(ctx->fn, expr->loc, add_op, LIR_TY_I64,
                                     pi_o, off_i64);
            int back = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_INTTOPTR,
                                       LIR_TY_I64, LIR_TY_PTR,
                                       lir_opnd_value(sum, LIR_TY_I64));
            return lir_opnd_value(back, LIR_TY_PTR);
        }

        LirType operand_type = a.type;
        if (a.type != b.type) {
            if (is_int_lir(a.type) && is_int_lir(b.type)) {
                a = to_i64(ctx, expr->loc, a);
                b = to_i64(ctx, expr->loc, b);
                operand_type = LIR_TY_I64;
            }
        }
        int r = lir_emit_bin(ctx->fn, expr->loc, op, operand_type, a, b);
        LirType result_type = (op >= LIR_BIN_EQ && op <= LIR_BIN_GE)
            ? LIR_TY_I1 : operand_type;
        return lir_opnd_value(r, result_type);
    }

    case NODE_UNARY_EXPR: {
        TokenKind op = expr->unary.op;
        if (op == TOK_MINUS) {
            LirOpnd v = lower_expr(ctx, expr->unary.operand);
            int r = lir_emit_un(ctx->fn, expr->loc, LIR_UN_NEG, v.type, v);
            return lir_opnd_value(r, v.type);
        }
        if (op == TOK_NOT) {
            LirOpnd v = lower_expr(ctx, expr->unary.operand);
            int r = lir_emit_un(ctx->fn, expr->loc, LIR_UN_NOT, v.type, v);
            return lir_opnd_value(r, LIR_TY_I1);
        }
        if (op == TOK_AMPERSAND || op == TOK_REF || op == TOK_REF_MUT) {
            // Address-of. For shape-typed locals the slot already IS the
            // shape ptr. For scalar locals (int/float/bool) we want the
            // slot ptr (callee writes back through it). For ptr-typed
            // locals (list/map/set/box/ref/str) we want the LOADED value
            // (the actual heap ptr) — these refs are transparent.
            AstNode *o = expr->unary.operand;
            if (o->kind == NODE_IDENT) {
                Local *l = scope_lookup(ctx, o->ident.name);
                if (l) {
                    if (l->shape_type) {
                        return lir_opnd_value(l->value_id, LIR_TY_PTR);
                    }
                    if (l->scalar_type == LIR_TY_PTR) {
                        int v = lir_emit_load(ctx->fn, expr->loc, LIR_TY_PTR,
                                               lir_opnd_value(l->value_id, LIR_TY_PTR));
                        return lir_opnd_value(v, LIR_TY_PTR);
                    }
                    return lir_opnd_value(l->value_id, LIR_TY_PTR);
                }
            }
            // For other expressions (e.g., field access), the lowered
            // value of a shape/scalar field is the loaded value, not the
            // address. For scalars we'd need to spill — defer to a
            // case-by-case extension.
            if (o->kind == NODE_FIELD_EXPR) {
                ShapeFieldInfo *f = NULL;
                LirOpnd addr = field_address(ctx, o, &f);
                if (f) return addr;
            }
            // Fall back: lower the operand and hope it's already a ptr.
            LirOpnd v = lower_expr(ctx, o);
            return v;
        }
        if (op == TOK_STAR) {
            // Dereference: load through the pointer.
            LirOpnd p = lower_expr(ctx, expr->unary.operand);
            MixType *rt = expr->resolved_type;
            LirType lt = mix_to_lir(rt);
            if (lt == LIR_TY_VOID) lt = LIR_TY_I64;
            int r = lir_emit_load(ctx->fn, expr->loc, lt, p);
            return lir_opnd_value(r, lt);
        }
        unsupported(expr, "unary operator");
        return lir_opnd_none();
    }

    case NODE_CALL_EXPR: return lower_call_expr(ctx, expr);
    case NODE_FIELD_EXPR: return lower_field_expr(ctx, expr);
    case NODE_LIST_LIT:   return lower_list_lit(ctx, expr);
    case NODE_INDEX_EXPR: return lower_index_expr(ctx, expr);
    case NODE_LAMBDA:     return lower_lambda(ctx, expr);

    case NODE_SLICE_EXPR: {
        // list[start..end] / list[start..=end] → mix_list_slice runtime.
        LirOpnd lst = lower_expr(ctx, expr->slice_expr.object);
        LirOpnd start, end;
        if (expr->slice_expr.start) {
            start = to_i64(ctx, expr->loc, lower_expr(ctx, expr->slice_expr.start));
        } else {
            start = lir_opnd_int_typed(0, LIR_TY_I64);
        }
        if (expr->slice_expr.end) {
            end = to_i64(ctx, expr->loc, lower_expr(ctx, expr->slice_expr.end));
        } else {
            // Default end = list length.
            LirType lps[] = { LIR_TY_PTR };
            register_runtime(ctx->mod, expr->loc, "mix_list_len",
                             LIR_TY_I64, lps, 1);
            LirOpnd largs[] = { lst };
            int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_len",
                                    LIR_TY_I64, largs, 1);
            end = lir_opnd_value(r, LIR_TY_I64);
        }
        LirOpnd inclusive = lir_opnd_int_typed(expr->slice_expr.inclusive ? 1 : 0,
                                                  LIR_TY_I32);
        LirType ps[] = { LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64, LIR_TY_I32 };
        register_runtime(ctx->mod, expr->loc, "mix_list_slice",
                         LIR_TY_PTR, ps, 4);
        LirOpnd args[] = { lst, start, end, inclusive };
        int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_slice",
                                LIR_TY_PTR, args, 4);
        return lir_opnd_value(r, LIR_TY_PTR);
    }

    case NODE_SHARED_EXPR: {
        // `shared int(0)` → mix_shared_new(init_val) → opaque ptr.
        LirOpnd v = lower_expr(ctx, expr->shared_expr.init_expr);
        LirOpnd as_i64 = to_storage_i64(ctx, expr->loc, v,
                                          expr->shared_expr.init_expr
                                              ? expr->shared_expr.init_expr->resolved_type
                                              : NULL);
        LirType ps[] = { LIR_TY_I64 };
        register_runtime(ctx->mod, expr->loc, "mix_shared_new",
                         LIR_TY_PTR, ps, 1);
        LirOpnd args[] = { as_i64 };
        int r = lir_emit_call(ctx->fn, expr->loc, "mix_shared_new",
                                LIR_TY_PTR, args, 1);
        return lir_opnd_value(r, LIR_TY_PTR);
    }

    case NODE_GO_EXPR: {
        // `go fn(args)` → mix_task_spawn(@fn, packed_args, count). Args
        // are stored into a fresh i64 array allocated via mix_alloc.
        AstNode *call = expr->go_expr.call_expr;
        if (!call || call->kind != NODE_CALL_EXPR) {
            unsupported(expr, "go requires a function call");
            return lir_opnd_none();
        }
        int n = call->call.arg_count;
        // Allocate args buffer (n * 8 bytes), or use null when no args.
        LirOpnd args_buf;
        if (n > 0) {
            LirType ap[] = { LIR_TY_I64 };
            register_runtime(ctx->mod, expr->loc, "mix_alloc",
                             LIR_TY_PTR, ap, 1);
            LirOpnd a_args[] = { lir_opnd_int_typed(n * 8, LIR_TY_I64) };
            int b = lir_emit_call(ctx->fn, expr->loc, "mix_alloc",
                                    LIR_TY_PTR, a_args, 1);
            args_buf = lir_opnd_value(b, LIR_TY_PTR);
            for (int i = 0; i < n; i++) {
                LirOpnd v = lower_expr(ctx, call->call.args[i]);
                LirOpnd as_i64 = to_storage_i64(ctx, expr->loc, v,
                                                  call->call.args[i]->resolved_type);
                int slot_addr = lir_emit_ptr_offset(ctx->fn, expr->loc,
                                                      args_buf, i * 8);
                lir_emit_store(ctx->fn, expr->loc, LIR_TY_I64, as_i64,
                                lir_opnd_value(slot_addr, LIR_TY_PTR));
            }
        } else {
            args_buf = lir_opnd_int_typed(0, LIR_TY_PTR);
        }
        LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, expr->loc, "mix_task_spawn",
                         LIR_TY_PTR, ps, 3);
        LirOpnd cargs[] = {
            lir_opnd_fn_ref(arena_strdup(ctx->mod->arena, call->call.name)),
            args_buf,
            lir_opnd_int_typed(n, LIR_TY_I64),
        };
        int r = lir_emit_call(ctx->fn, expr->loc, "mix_task_spawn",
                                LIR_TY_PTR, cargs, 3);
        return lir_opnd_value(r, LIR_TY_PTR);
    }

    case NODE_WAIT_EXPR: {
        // `wait t` → mix_task_wait(t) → i64 result.
        LirOpnd handle = lower_expr(ctx, expr->wait_expr.handle_expr);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, expr->loc, "mix_task_wait",
                         LIR_TY_I64, ps, 1);
        LirOpnd args[] = { handle };
        int r = lir_emit_call(ctx->fn, expr->loc, "mix_task_wait",
                                LIR_TY_I64, args, 1);
        return lir_opnd_value(r, LIR_TY_I64);
    }

    case NODE_TRY_EXPR: {
        // `expr?` — unwrap a Result/Optional. If the value is ok/some,
        // produce the unwrapped scalar; otherwise return the (still-
        // error) result/none from the enclosing function.
        AstNode *inner = expr->try_expr.expr;
        MixType *inner_t = inner ? inner->resolved_type : NULL;
        bool is_result = inner_t && inner_t->kind == TYPE_RESULT;
        LirOpnd v = lower_expr(ctx, inner);

        const char *has_name = is_result ? "mix_result_is_ok" : "mix_optional_has";
        const char *get_name = is_result ? "mix_result_unwrap" : "mix_optional_get";
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, expr->loc, has_name, LIR_TY_I64, ps, 1);
        register_runtime(ctx->mod, expr->loc, get_name, LIR_TY_I64, ps, 1);
        LirOpnd args[] = { v };
        int has_v = lir_emit_call(ctx->fn, expr->loc, has_name, LIR_TY_I64, args, 1);
        int has_b = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_NE, LIR_TY_I64,
                                   lir_opnd_value(has_v, LIR_TY_I64),
                                   lir_opnd_int_typed(0, LIR_TY_I64));

        int ok_l = lir_func_new_label(ctx->fn);
        int err_l = lir_func_new_label(ctx->fn);
        emit_br_cond(ctx, expr->loc, lir_opnd_value(has_b, LIR_TY_I1), ok_l, err_l);

        // Error path: propagate. For Result, just return the same result.
        // For Optional, the enclosing fn must also return an Optional.
        emit_label(ctx, expr->loc, err_l);
        emit_ret_value(ctx, expr->loc, v);

        // OK path: extract the inner value and continue.
        emit_label(ctx, expr->loc, ok_l);
        int got = lir_emit_call(ctx->fn, expr->loc, get_name,
                                  LIR_TY_I64, args, 1);
        return from_storage_i64(ctx, expr->loc,
                                  lir_opnd_value(got, LIR_TY_I64),
                                  expr->resolved_type);
    }

    case NODE_METHOD_CALL: {
        AstNode *obj = expr->method_call.object;
        // `List[T].new(z)` / `Map[K,V].new(z)` / `Set[T].new(z)` —
        // generic-shape constructor in a zone. Object is an IDENT with
        // type args; method_name is "new".
        if (obj && obj->kind == NODE_IDENT &&
            obj->ident.type_arg_count > 0 &&
            expr->method_call.method_name &&
            strcmp(expr->method_call.method_name, "new") == 0 &&
            expr->method_call.arg_count >= 1) {
            MixType *ctor_t = expr->resolved_type
                ? expr->resolved_type
                : obj->resolved_type;
            LirOpnd zone = lower_expr(ctx, expr->method_call.args[0]);
            if (ctor_t && ctor_t->kind == TYPE_LIST) {
                MixType *elem = ctor_t->list.elem_type;
                if (elem && elem->kind == TYPE_SHAPE) {
                    LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                    register_runtime(ctx->mod, expr->loc, "mix_list_new_shape_in",
                                     LIR_TY_PTR, ps, 2);
                    LirOpnd args[] = { zone,
                        lir_opnd_int_typed(elem->shape.total_size, LIR_TY_I64) };
                    int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_new_shape_in",
                                            LIR_TY_PTR, args, 2);
                    return lir_opnd_value(r, LIR_TY_PTR);
                }
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_list_new_in",
                                 LIR_TY_PTR, ps, 1);
                LirOpnd args[] = { zone };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_new_in",
                                        LIR_TY_PTR, args, 1);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            if (ctor_t && ctor_t->kind == TYPE_MAP) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_map_new_in",
                                 LIR_TY_PTR, ps, 1);
                LirOpnd args[] = { zone };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_map_new_in",
                                        LIR_TY_PTR, args, 1);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            if (ctor_t && ctor_t->kind == TYPE_SET) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_set_new_in",
                                 LIR_TY_PTR, ps, 1);
                LirOpnd args[] = { zone };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_set_new_in",
                                        LIR_TY_PTR, args, 1);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
        }
        MixType *obj_t = obj ? obj->resolved_type : NULL;
        const char *mname = expr->method_call.method_name;

        // ---- Collection methods (lists/maps/sets) -----------------------
        if (obj_t && obj_t->kind == TYPE_LIST) {
            MixType *elem = obj_t->list.elem_type;
            LirOpnd list = lower_expr(ctx, obj);

            if (strcmp(mname, "push") == 0 || strcmp(mname, "append") == 0) {
                if (expr->method_call.arg_count != 1) {
                    unsupported(expr, "list.push/append with arity != 1");
                    return lir_opnd_none();
                }
                AstNode *a = expr->method_call.args[0];
                if (elem && elem->kind == TYPE_SHAPE) {
                    int tmp = lir_emit_alloca_bytes(ctx->fn, a->loc,
                                                      elem->shape.total_size,
                                                      elem->shape.alignment);
                    LirOpnd tmp_p = lir_opnd_value(tmp, LIR_TY_PTR);
                    lower_init_into(ctx, tmp_p, elem, a);
                    LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                    register_runtime(ctx->mod, expr->loc, "mix_list_push_bytes",
                                     LIR_TY_VOID, ps, 2);
                    LirOpnd args[] = { list, tmp_p };
                    lir_emit_call(ctx->fn, expr->loc, "mix_list_push_bytes",
                                    LIR_TY_VOID, args, 2);
                } else {
                    LirOpnd v = lower_expr(ctx, a);
                    LirOpnd as_i64 = to_storage_i64(ctx, a->loc, v, a->resolved_type);
                    LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                    register_runtime(ctx->mod, expr->loc, "mix_list_push",
                                     LIR_TY_VOID, ps, 2);
                    LirOpnd args[] = { list, as_i64 };
                    lir_emit_call(ctx->fn, expr->loc, "mix_list_push",
                                    LIR_TY_VOID, args, 2);
                }
                return lir_opnd_none();
            }
            if (strcmp(mname, "at") == 0 || strcmp(mname, "at_mut") == 0) {
                AstNode *a = expr->method_call.args[0];
                LirOpnd idx_i64 = to_i64(ctx, a->loc, lower_expr(ctx, a));
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_list_ptr",
                                 LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { list, idx_i64 };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_ptr",
                                        LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            if (strcmp(mname, "pop") == 0) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_list_pop",
                                 LIR_TY_I64, ps, 1);
                LirOpnd args[] = { list };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_pop",
                                        LIR_TY_I64, args, 1);
                LirOpnd raw = lir_opnd_value(r, LIR_TY_I64);
                return from_storage_i64(ctx, expr->loc, raw, elem);
            }
            // Unknown list method — fall through to unsupported.
        }

        // ---- Map methods ------------------------------------------------
        if (obj_t && obj_t->kind == TYPE_MAP) {
            LirOpnd m = lower_expr(ctx, obj);
            if (strcmp(mname, "has") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_map_has",
                                 LIR_TY_I64, ps, 2);
                LirOpnd args[] = { m, k };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_map_has",
                                        LIR_TY_I64, args, 2);
                int b = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_NE, LIR_TY_I64,
                                       lir_opnd_value(r, LIR_TY_I64),
                                       lir_opnd_int_typed(0, LIR_TY_I64));
                return lir_opnd_value(b, LIR_TY_I1);
            }
            if (strcmp(mname, "get") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_map_get",
                                 LIR_TY_I64, ps, 2);
                LirOpnd args[] = { m, k };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_map_get",
                                        LIR_TY_I64, args, 2);
                LirOpnd raw = lir_opnd_value(r, LIR_TY_I64);
                return from_storage_i64(ctx, expr->loc, raw, obj_t->map.val_type);
            }
            if (strcmp(mname, "set") == 0 && expr->method_call.arg_count == 2) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirOpnd v = lower_expr(ctx, expr->method_call.args[1]);
                LirOpnd vi = to_storage_i64(ctx, expr->loc, v,
                                               expr->method_call.args[1]->resolved_type);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_map_set",
                                 LIR_TY_VOID, ps, 3);
                LirOpnd args[] = { m, k, vi };
                lir_emit_call(ctx->fn, expr->loc, "mix_map_set",
                                LIR_TY_VOID, args, 3);
                return lir_opnd_none();
            }
            if (strcmp(mname, "remove") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_map_remove",
                                 LIR_TY_VOID, ps, 2);
                LirOpnd args[] = { m, k };
                lir_emit_call(ctx->fn, expr->loc, "mix_map_remove",
                                LIR_TY_VOID, args, 2);
                return lir_opnd_none();
            }
        }

        // ---- Shared methods ---------------------------------------------
        if (obj_t && obj_t->kind == TYPE_SHARED) {
            LirOpnd s = lower_expr(ctx, obj);
            if (strcmp(mname, "read") == 0 && expr->method_call.arg_count == 0) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_shared_read",
                                 LIR_TY_I64, ps, 1);
                LirOpnd args[] = { s };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_shared_read",
                                        LIR_TY_I64, args, 1);
                return lir_opnd_value(r, LIR_TY_I64);
            }
            if (strcmp(mname, "update") == 0 && expr->method_call.arg_count == 1) {
                // Arg is a lambda/fn-ptr. Lower it then ptrtoint→ptr.
                LirOpnd fn_v = lower_expr(ctx, expr->method_call.args[0]);
                if (fn_v.type != LIR_TY_PTR) {
                    LirOpnd w = fn_v;
                    if (w.type != LIR_TY_I64)
                        w = widen_to_i64(ctx, expr->loc, w,
                                          expr->method_call.args[0]->resolved_type);
                    int p = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_INTTOPTR,
                                            LIR_TY_I64, LIR_TY_PTR, w);
                    fn_v = lir_opnd_value(p, LIR_TY_PTR);
                }
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_shared_update",
                                 LIR_TY_VOID, ps, 2);
                LirOpnd args[] = { s, fn_v };
                lir_emit_call(ctx->fn, expr->loc, "mix_shared_update",
                                LIR_TY_VOID, args, 2);
                return lir_opnd_none();
            }
        }

        // ---- Set methods ------------------------------------------------
        if (obj_t && obj_t->kind == TYPE_SET) {
            LirOpnd s = lower_expr(ctx, obj);
            if ((strcmp(mname, "has") == 0 || strcmp(mname, "contains") == 0)
                && expr->method_call.arg_count == 1) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_set_has",
                                 LIR_TY_I64, ps, 2);
                LirOpnd args[] = { s, k };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_set_has",
                                        LIR_TY_I64, args, 2);
                int b = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_NE, LIR_TY_I64,
                                       lir_opnd_value(r, LIR_TY_I64),
                                       lir_opnd_int_typed(0, LIR_TY_I64));
                return lir_opnd_value(b, LIR_TY_I1);
            }
            if (strcmp(mname, "add") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_set_add",
                                 LIR_TY_VOID, ps, 2);
                LirOpnd args[] = { s, k };
                lir_emit_call(ctx->fn, expr->loc, "mix_set_add",
                                LIR_TY_VOID, args, 2);
                return lir_opnd_none();
            }
            if (strcmp(mname, "remove") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd k = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_set_remove",
                                 LIR_TY_VOID, ps, 2);
                LirOpnd args[] = { s, k };
                lir_emit_call(ctx->fn, expr->loc, "mix_set_remove",
                                LIR_TY_VOID, args, 2);
                return lir_opnd_none();
            }
            // Set algebra: union/intersect/diff. Each takes another set
            // and returns a fresh set ptr.
            const char *helper2 = NULL;
            if (strcmp(mname, "union") == 0)         helper2 = "mix_set_union";
            else if (strcmp(mname, "intersect") == 0) helper2 = "mix_set_intersect";
            else if (strcmp(mname, "diff") == 0)     helper2 = "mix_set_diff";
            if (helper2 && expr->method_call.arg_count == 1) {
                LirOpnd b = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, helper2, LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { s, b };
                int r = lir_emit_call(ctx->fn, expr->loc, helper2,
                                        LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
        }

        // ---- String methods ---------------------------------------------
        if (obj_t && obj_t->kind == TYPE_STR) {
            LirOpnd s = lower_expr(ctx, obj);
            // 1-arg helpers returning ptr (string)
            const char *helper1 = NULL;
            if (strcmp(mname, "upper") == 0)         helper1 = "mix_str_upper";
            else if (strcmp(mname, "lower") == 0)    helper1 = "mix_str_lower";
            else if (strcmp(mname, "trim") == 0)     helper1 = "mix_str_trim";
            else if (strcmp(mname, "reverse") == 0)  helper1 = "mix_str_reverse";
            if (helper1) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, helper1, LIR_TY_PTR, ps, 1);
                LirOpnd args[] = { s };
                int r = lir_emit_call(ctx->fn, expr->loc, helper1,
                                        LIR_TY_PTR, args, 1);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // 2-arg helpers (str, str)
            const char *helper2_str = NULL;
            LirType helper2_ret = LIR_TY_PTR;
            if (strcmp(mname, "split") == 0)         { helper2_str = "mix_str_split";       helper2_ret = LIR_TY_PTR; }
            else if (strcmp(mname, "concat") == 0)   { helper2_str = "mix_str_concat";      helper2_ret = LIR_TY_PTR; }
            else if (strcmp(mname, "contains") == 0) { helper2_str = "mix_str_contains";    helper2_ret = LIR_TY_I64; }
            else if (strcmp(mname, "starts_with") == 0) { helper2_str = "mix_str_starts_with"; helper2_ret = LIR_TY_I64; }
            else if (strcmp(mname, "ends_with") == 0) { helper2_str = "mix_str_ends_with";  helper2_ret = LIR_TY_I64; }
            if (helper2_str && expr->method_call.arg_count == 1) {
                LirOpnd a = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, helper2_str, helper2_ret, ps, 2);
                LirOpnd args[] = { s, a };
                int r = lir_emit_call(ctx->fn, expr->loc, helper2_str,
                                        helper2_ret, args, 2);
                if (helper2_ret == LIR_TY_I64) {
                    int b = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_NE, LIR_TY_I64,
                                           lir_opnd_value(r, LIR_TY_I64),
                                           lir_opnd_int_typed(0, LIR_TY_I64));
                    return lir_opnd_value(b, LIR_TY_I1);
                }
                return lir_opnd_value(r, helper2_ret);
            }
            // join: list.join(sep)
            if (strcmp(mname, "join") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd sep = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_join", LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { s, sep };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_join", LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // replace: str.replace(old, new)
            if (strcmp(mname, "replace") == 0 && expr->method_call.arg_count == 2) {
                LirOpnd a = lower_expr(ctx, expr->method_call.args[0]);
                LirOpnd b = lower_expr(ctx, expr->method_call.args[1]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_replace", LIR_TY_PTR, ps, 3);
                LirOpnd args[] = { s, a, b };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_replace", LIR_TY_PTR, args, 3);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // char_at(idx) — single-char string
            if (strcmp(mname, "char_at") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd idx = lower_expr(ctx, expr->method_call.args[0]);
                idx = to_i64(ctx, expr->loc, idx);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_str_char_at", LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { s, idx };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_char_at",
                                        LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // slice(start, end)
            if (strcmp(mname, "slice") == 0 && expr->method_call.arg_count == 2) {
                LirOpnd a = to_i64(ctx, expr->loc, lower_expr(ctx, expr->method_call.args[0]));
                LirOpnd b = to_i64(ctx, expr->loc, lower_expr(ctx, expr->method_call.args[1]));
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_str_slice", LIR_TY_PTR, ps, 3);
                LirOpnd args[] = { s, a, b };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_slice",
                                        LIR_TY_PTR, args, 3);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // sort() — char sort
            if (strcmp(mname, "sort") == 0 && expr->method_call.arg_count == 0) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_sort", LIR_TY_PTR, ps, 1);
                LirOpnd args[] = { s };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_sort",
                                        LIR_TY_PTR, args, 1);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // index_of(sub)
            if (strcmp(mname, "index_of") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd a = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_index_of", LIR_TY_I64, ps, 2);
                LirOpnd args[] = { s, a };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_index_of",
                                        LIR_TY_I64, args, 2);
                return lir_opnd_value(r, LIR_TY_I64);
            }
            // repeat(n)
            if (strcmp(mname, "repeat") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd n = to_i64(ctx, expr->loc, lower_expr(ctx, expr->method_call.args[0]));
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_str_repeat", LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { s, n };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_repeat",
                                        LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
            // count(sub)
            if (strcmp(mname, "count") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd a = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_count", LIR_TY_I64, ps, 2);
                LirOpnd args[] = { s, a };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_count",
                                        LIR_TY_I64, args, 2);
                return lir_opnd_value(r, LIR_TY_I64);
            }
            // code() — first-byte code point (alias for ord(s))
            if (strcmp(mname, "code") == 0 && expr->method_call.arg_count == 0) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_ord", LIR_TY_I64, ps, 1);
                LirOpnd args[] = { s };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_ord",
                                        LIR_TY_I64, args, 1);
                return lir_opnd_value(r, LIR_TY_I64);
            }
        }

        // ---- More list methods ------------------------------------------
        if (obj_t && obj_t->kind == TYPE_LIST) {
            MixType *elem = obj_t->list.elem_type;
            LirOpnd list = lower_expr(ctx, obj);

            if (strcmp(mname, "sort") == 0) {
                const char *fn_name = "mix_list_sort";
                if (elem && elem->kind == TYPE_STR) fn_name = "mix_list_sort_str";
                else if (elem && (elem->kind == TYPE_FLOAT || elem->kind == TYPE_FLOAT64))
                    fn_name = "mix_list_sort_float";
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, fn_name, LIR_TY_VOID, ps, 1);
                LirOpnd args[] = { list };
                lir_emit_call(ctx->fn, expr->loc, fn_name, LIR_TY_VOID, args, 1);
                return lir_opnd_none();
            }
            if (strcmp(mname, "reverse") == 0) {
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_list_reverse", LIR_TY_VOID, ps, 1);
                LirOpnd args[] = { list };
                lir_emit_call(ctx->fn, expr->loc, "mix_list_reverse", LIR_TY_VOID, args, 1);
                return lir_opnd_none();
            }
            if (strcmp(mname, "contains") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd v = lower_expr(ctx, expr->method_call.args[0]);
                LirOpnd vi = to_storage_i64(ctx, expr->loc, v,
                                               expr->method_call.args[0]->resolved_type);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_list_index_of", LIR_TY_I64, ps, 2);
                LirOpnd args[] = { list, vi };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_index_of", LIR_TY_I64, args, 2);
                int b = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_GE, LIR_TY_I64,
                                       lir_opnd_value(r, LIR_TY_I64),
                                       lir_opnd_int_typed(0, LIR_TY_I64));
                return lir_opnd_value(b, LIR_TY_I1);
            }
            if (strcmp(mname, "index_of") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd v = lower_expr(ctx, expr->method_call.args[0]);
                LirOpnd vi = to_storage_i64(ctx, expr->loc, v,
                                               expr->method_call.args[0]->resolved_type);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_list_index_of", LIR_TY_I64, ps, 2);
                LirOpnd args[] = { list, vi };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_list_index_of", LIR_TY_I64, args, 2);
                return lir_opnd_value(r, LIR_TY_I64);
            }
            if (strcmp(mname, "remove") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd v = lower_expr(ctx, expr->method_call.args[0]);
                LirOpnd vi = to_i64(ctx, expr->loc, v);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_list_remove", LIR_TY_VOID, ps, 2);
                LirOpnd args[] = { list, vi };
                lir_emit_call(ctx->fn, expr->loc, "mix_list_remove", LIR_TY_VOID, args, 2);
                return lir_opnd_none();
            }
            if (strcmp(mname, "insert") == 0 && expr->method_call.arg_count == 2) {
                LirOpnd idx = to_i64(ctx, expr->loc, lower_expr(ctx, expr->method_call.args[0]));
                LirOpnd v = lower_expr(ctx, expr->method_call.args[1]);
                LirOpnd vi = to_storage_i64(ctx, expr->loc, v,
                                               expr->method_call.args[1]->resolved_type);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64 };
                register_runtime(ctx->mod, expr->loc, "mix_list_insert", LIR_TY_VOID, ps, 3);
                LirOpnd args[] = { list, idx, vi };
                lir_emit_call(ctx->fn, expr->loc, "mix_list_insert", LIR_TY_VOID, args, 3);
                return lir_opnd_none();
            }
            if (strcmp(mname, "join") == 0 && expr->method_call.arg_count == 1) {
                LirOpnd sep = lower_expr(ctx, expr->method_call.args[0]);
                LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
                register_runtime(ctx->mod, expr->loc, "mix_str_join", LIR_TY_PTR, ps, 2);
                LirOpnd args[] = { list, sep };
                int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_join", LIR_TY_PTR, args, 2);
                return lir_opnd_value(r, LIR_TY_PTR);
            }
        }

        // Unwrap ref/box wrappers for shape method dispatch.
        MixType *unwrapped = unwrap_to_shape(obj_t);
        if (unwrapped) obj_t = unwrapped;

        if (!obj_t || obj_t->kind != TYPE_SHAPE) {
            unsupported(expr, "method call on non-shape");
            return lir_opnd_none();
        }

        // ---- Field-as-function dispatch ---------------------------------
        // `b.fn(args)` where `fn` is a field (no Shape_fn method exists).
        // Load the field value (fn-ptr stored as int / ptr) and emit an
        // indirect call.
        ShapeFieldInfo *field_as_fn = find_field(obj_t, expr->method_call.method_name);
        char method_mangled[256];
        snprintf(method_mangled, sizeof(method_mangled), "%s_%s",
                 obj_t->shape.name, expr->method_call.method_name);
        Symbol *method_sym = ctx->symtab
            ? symtab_lookup(ctx->symtab, method_mangled)
            : NULL;
        bool method_exists = method_sym && method_sym->type &&
                              method_sym->type->kind == TYPE_FUNC;
        if (field_as_fn && !method_exists) {
            AstNode fe_obj_node;
            memset(&fe_obj_node, 0, sizeof(fe_obj_node));
            fe_obj_node.kind = NODE_FIELD_EXPR;
            fe_obj_node.loc = expr->loc;
            fe_obj_node.field_expr.object = obj;
            fe_obj_node.field_expr.field_name = expr->method_call.method_name;
            LirOpnd field_val = lower_field_expr(ctx, &fe_obj_node);
            // Coerce field value to ptr (it's stored as i64).
            LirOpnd fp_ptr = field_val;
            if (is_int_lir(field_val.type)) {
                LirOpnd w = field_val;
                if (field_val.type != LIR_TY_I64) {
                    int r = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_SEXT,
                                            field_val.type, LIR_TY_I64, field_val);
                    w = lir_opnd_value(r, LIR_TY_I64);
                }
                int p = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_INTTOPTR,
                                        LIR_TY_I64, LIR_TY_PTR, w);
                fp_ptr = lir_opnd_value(p, LIR_TY_PTR);
            }
            int an = expr->method_call.arg_count;
            LirOpnd *cargs = an > 0 ? arena_alloc(ctx->mod->arena, an * sizeof(LirOpnd)) : NULL;
            LirType *cps   = an > 0 ? arena_alloc(ctx->mod->arena, an * sizeof(LirType)) : NULL;
            for (int i = 0; i < an; i++) {
                LirOpnd a = lower_expr(ctx, expr->method_call.args[i]);
                if (a.type == LIR_TY_F64) {
                    int r = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_BITCAST,
                                            LIR_TY_F64, LIR_TY_I64, a);
                    a = lir_opnd_value(r, LIR_TY_I64);
                } else if (a.type == LIR_TY_PTR) {
                    int r = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_PTRTOINT,
                                            LIR_TY_PTR, LIR_TY_I64, a);
                    a = lir_opnd_value(r, LIR_TY_I64);
                } else if (is_int_lir(a.type) && a.type != LIR_TY_I64) {
                    a = widen_to_i64(ctx, expr->loc, a,
                                       expr->method_call.args[i]->resolved_type);
                }
                cargs[i] = a;
                cps[i] = LIR_TY_I64;
            }
            int r = lir_emit_call_indirect(ctx->fn, expr->loc, fp_ptr,
                                              LIR_TY_I64, cps, cargs, an);
            return r >= 0 ? lir_opnd_value(r, LIR_TY_I64) : lir_opnd_none();
        }

        // ---- Shape method dispatch --------------------------------------
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s_%s",
                 obj_t->shape.name, expr->method_call.method_name);

        AstNode *fake = arena_alloc(ctx->mod->arena, sizeof(AstNode));
        memset(fake, 0, sizeof(*fake));
        fake->kind = NODE_CALL_EXPR;
        fake->loc = expr->loc;
        fake->resolved_type = expr->resolved_type;
        fake->call.name = arena_strdup(ctx->mod->arena, mangled);

        int n = expr->method_call.arg_count;
        fake->call.arg_count = n + 1;
        fake->call.args = arena_alloc(ctx->mod->arena,
                                        (n + 1) * sizeof(AstNode *));
        fake->call.args[0] = obj;
        for (int i = 0; i < n; i++) fake->call.args[i + 1] = expr->method_call.args[i];

        return lower_user_call(ctx, fake);
    }

    case NODE_NONE_LIT: {
        // `none` materializes a real {has=0, val=0} optional via the
        // runtime so call sites that hand it to else/match can call
        // mix_optional_has on it without dereffing NULL.
        register_runtime(ctx->mod, expr->loc, "mix_optional_none",
                         LIR_TY_PTR, NULL, 0);
        int r = lir_emit_call(ctx->fn, expr->loc, "mix_optional_none",
                                LIR_TY_PTR, NULL, 0);
        return lir_opnd_value(r, LIR_TY_PTR);
    }

    case NODE_ELSE_EXPR: {
        // `expr else fallback`. For optional types: if expr returns
        // non-null (some), use the unwrapped value; else use fallback.
        // For Phase 5 simplicity, we treat the optional payload by
        // calling mix_optional_has + mix_optional_get on the expr value
        // and using LLVM select.
        LirOpnd v = lower_expr(ctx, expr->else_expr.value);
        LirOpnd fb = lower_expr(ctx, expr->else_expr.fallback);

        MixType *src_t = expr->else_expr.value->resolved_type;
        bool is_optional_src = src_t && src_t->kind == TYPE_OPTIONAL;
        bool is_result_src = src_t && src_t->kind == TYPE_RESULT;
        // Bare `none else x` resolves the value to TYPE_VOID — treat it
        // like an optional source so the runtime branch is taken on the
        // empty optional pointer.
        bool is_none_src = expr->else_expr.value &&
                           expr->else_expr.value->kind == NODE_NONE_LIT;
        if (is_optional_src || is_none_src || is_result_src) {
            const char *has_name = is_result_src ? "mix_result_is_ok" : "mix_optional_has";
            const char *get_name = is_result_src ? "mix_result_unwrap" : "mix_optional_get";
            LirType ps[] = { LIR_TY_PTR };
            register_runtime(ctx->mod, expr->loc, has_name,
                             LIR_TY_I64, ps, 1);
            register_runtime(ctx->mod, expr->loc, get_name,
                             LIR_TY_I64, ps, 1);
            LirOpnd args[] = { v };
            int has_v = lir_emit_call(ctx->fn, expr->loc, has_name,
                                        LIR_TY_I64, args, 1);
            int has_b = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_NE, LIR_TY_I64,
                                       lir_opnd_value(has_v, LIR_TY_I64),
                                       lir_opnd_int_typed(0, LIR_TY_I64));

            int then_l = lir_func_new_label(ctx->fn);
            int else_l = lir_func_new_label(ctx->fn);
            int merge_l = lir_func_new_label(ctx->fn);

            // Result slot to merge both branches.
            LirType result_lt = mix_to_lir(expr->resolved_type);
            if (result_lt == LIR_TY_VOID) result_lt = LIR_TY_I64;
            int slot = lir_emit_alloca(ctx->fn, expr->loc, result_lt);
            LirOpnd slot_p = lir_opnd_value(slot, LIR_TY_PTR);

            emit_br_cond(ctx, expr->loc, lir_opnd_value(has_b, LIR_TY_I1),
                          then_l, else_l);

            emit_label(ctx, expr->loc, then_l);
            int got = lir_emit_call(ctx->fn, expr->loc, get_name,
                                      LIR_TY_I64, args, 1);
            LirOpnd got_v = from_storage_i64(ctx, expr->loc,
                                                lir_opnd_value(got, LIR_TY_I64),
                                                expr->resolved_type);
            // Convert to result_lt if needed (already done by from_storage_i64).
            lir_emit_store(ctx->fn, expr->loc, result_lt, got_v, slot_p);
            emit_br(ctx, expr->loc, merge_l);

            emit_label(ctx, expr->loc, else_l);
            // Coerce fallback to result_lt if needed.
            if (fb.type != result_lt && is_int_lir(fb.type) && is_int_lir(result_lt)) {
                int r = lir_emit_conv(ctx->fn, expr->loc,
                                       result_lt == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                       fb.type, result_lt, fb);
                fb = lir_opnd_value(r, result_lt);
            }
            lir_emit_store(ctx->fn, expr->loc, result_lt, fb, slot_p);
            emit_br(ctx, expr->loc, merge_l);

            emit_label(ctx, expr->loc, merge_l);
            int loaded = lir_emit_load(ctx->fn, expr->loc, result_lt, slot_p);
            return lir_opnd_value(loaded, result_lt);
        }
        // Non-optional: just use the value (legacy behavior is to use
        // fallback only on none/null, but for non-optional types the
        // value is always present).
        (void)fb;
        return v;
    }

    case NODE_CAST_EXPR: {
        LirOpnd v = lower_expr(ctx, expr->cast_expr.value);
        LirType dst = mix_to_lir(expr->resolved_type);
        if (dst == LIR_TY_VOID) return v;
        if (v.type == dst) return v;
        // int → float
        if (is_int_lir(v.type) && dst == LIR_TY_F64) {
            int r = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_SITOFP,
                                    v.type, dst, v);
            return lir_opnd_value(r, dst);
        }
        // float → int
        if (v.type == LIR_TY_F64 && is_int_lir(dst)) {
            int r = lir_emit_conv(ctx->fn, expr->loc, LIR_CONV_FPTOSI,
                                    v.type, dst, v);
            return lir_opnd_value(r, dst);
        }
        // int widening / narrowing
        if (is_int_lir(v.type) && is_int_lir(dst)) {
            LirConvKind k;
            // Pick zext for unsigned source widening; sext otherwise; trunc for narrowing.
            // (Same logic as widen_to_i64 but to arbitrary dst.)
            // Treat boolean source as zext.
            int src_w = (v.type==LIR_TY_I1)?1:(v.type==LIR_TY_I8)?8:(v.type==LIR_TY_I32)?32:64;
            int dst_w = (dst==LIR_TY_I1)?1:(dst==LIR_TY_I8)?8:(dst==LIR_TY_I32)?32:64;
            if (dst_w < src_w) k = LIR_CONV_TRUNC;
            else k = mix_is_unsigned(expr->cast_expr.value->resolved_type) ? LIR_CONV_ZEXT : LIR_CONV_SEXT;
            int r = lir_emit_conv(ctx->fn, expr->loc, k, v.type, dst, v);
            return lir_opnd_value(r, dst);
        }
        return v;
    }

    case NODE_LIST_COMP: {
        // [expr for x in iter if cond]
        // Lower as: alloca list (new), for each x in iter, if cond push expr.
        MixType *list_type = expr->resolved_type;
        MixType *elem = (list_type && list_type->kind == TYPE_LIST)
            ? list_type->list.elem_type : NULL;

        int list_v;
        if (elem && elem->kind == TYPE_SHAPE) {
            LirType ps[] = { LIR_TY_I64 };
            register_runtime(ctx->mod, expr->loc, "mix_list_new_shape",
                             LIR_TY_PTR, ps, 1);
            LirOpnd args[] = { lir_opnd_int_typed(elem->shape.total_size, LIR_TY_I64) };
            list_v = lir_emit_call(ctx->fn, expr->loc, "mix_list_new_shape",
                                     LIR_TY_PTR, args, 1);
        } else {
            register_runtime(ctx->mod, expr->loc, "mix_list_new",
                             LIR_TY_PTR, NULL, 0);
            list_v = lir_emit_call(ctx->fn, expr->loc, "mix_list_new",
                                     LIR_TY_PTR, NULL, 0);
        }
        LirOpnd list_p = lir_opnd_value(list_v, LIR_TY_PTR);

        // Synthesize a `for var in iter` loop.
        AstNode iter_loop = {0};
        iter_loop.kind = NODE_FOR_STMT;
        iter_loop.loc = expr->loc;
        iter_loop.for_stmt.var_name = expr->list_comp.var_name;
        iter_loop.for_stmt.iterable = expr->list_comp.iterable;

        // Body: optionally if cond, then push expr. We can't easily build
        // an AST for that — instead inline the loop here.
        AstNode *iter = expr->list_comp.iterable;
        MixType *iter_type = iter ? iter->resolved_type : NULL;
        if (!iter_type || iter_type->kind != TYPE_LIST) {
            unsupported(expr, "list comp over non-list iterable");
            return list_p;
        }
        MixType *iter_elem = iter_type->list.elem_type;

        LirOpnd src_list = lower_expr(ctx, iter);

        int counter_slot = lir_emit_alloca(ctx->fn, expr->loc, LIR_TY_I64);
        LirOpnd counter_p = lir_opnd_value(counter_slot, LIR_TY_PTR);
        lir_emit_store(ctx->fn, expr->loc, LIR_TY_I64,
                        lir_opnd_int_typed(0, LIR_TY_I64), counter_p);
        LirType len_p[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, expr->loc, "mix_list_len", LIR_TY_I64, len_p, 1);
        LirOpnd len_args[] = { src_list };
        int len_v = lir_emit_call(ctx->fn, expr->loc, "mix_list_len",
                                     LIR_TY_I64, len_args, 1);
        LirOpnd len = lir_opnd_value(len_v, LIR_TY_I64);

        int cond_l = lir_func_new_label(ctx->fn);
        int body_l = lir_func_new_label(ctx->fn);
        int after_l = lir_func_new_label(ctx->fn);

        emit_br(ctx, expr->loc, cond_l);
        emit_label(ctx, expr->loc, cond_l);
        int cur_v = lir_emit_load(ctx->fn, expr->loc, LIR_TY_I64, counter_p);
        int cmp_v = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_LT, LIR_TY_I64,
                                   lir_opnd_value(cur_v, LIR_TY_I64), len);
        emit_br_cond(ctx, expr->loc, lir_opnd_value(cmp_v, LIR_TY_I1),
                      body_l, after_l);

        emit_label(ctx, expr->loc, body_l);
        // Bind iter elem.
        int saved = ctx->local_count;
        if (iter_elem && iter_elem->kind == TYPE_SHAPE) {
            LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
            register_runtime(ctx->mod, expr->loc, "mix_list_ptr",
                             LIR_TY_PTR, ps, 2);
            LirOpnd args[] = { src_list, lir_opnd_value(cur_v, LIR_TY_I64) };
            int p = lir_emit_call(ctx->fn, expr->loc, "mix_list_ptr",
                                    LIR_TY_PTR, args, 2);
            Local *l = scope_add(ctx, expr->list_comp.var_name);
            l->value_id = p;
            l->shape_type = iter_elem;
        } else {
            LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
            register_runtime(ctx->mod, expr->loc, "mix_list_get",
                             LIR_TY_I64, ps, 2);
            LirOpnd args[] = { src_list, lir_opnd_value(cur_v, LIR_TY_I64) };
            int raw = lir_emit_call(ctx->fn, expr->loc, "mix_list_get",
                                      LIR_TY_I64, args, 2);
            LirOpnd val = from_storage_i64(ctx, expr->loc,
                                              lir_opnd_value(raw, LIR_TY_I64),
                                              iter_elem);
            LirType lt = mix_to_lir(iter_elem);
            if (lt == LIR_TY_VOID) lt = LIR_TY_I64;
            int slot = lir_emit_alloca(ctx->fn, expr->loc, lt);
            lir_emit_store(ctx->fn, expr->loc, lt, val,
                            lir_opnd_value(slot, LIR_TY_PTR));
            Local *l = scope_add(ctx, expr->list_comp.var_name);
            l->value_id = slot;
            l->scalar_type = lt;
        }

        // Optional condition: if false, skip push.
        int skip_l = -1;
        int push_l = -1;
        if (expr->list_comp.condition) {
            LirOpnd c = lower_expr(ctx, expr->list_comp.condition);
            push_l = lir_func_new_label(ctx->fn);
            skip_l = lir_func_new_label(ctx->fn);
            emit_br_cond(ctx, expr->loc, c, push_l, skip_l);
            emit_label(ctx, expr->loc, push_l);
        }

        // Lower expr and push.
        if (elem && elem->kind == TYPE_SHAPE) {
            int tmp = lir_emit_alloca_bytes(ctx->fn, expr->loc,
                                              elem->shape.total_size,
                                              elem->shape.alignment);
            LirOpnd tmp_p = lir_opnd_value(tmp, LIR_TY_PTR);
            lower_init_into(ctx, tmp_p, elem, expr->list_comp.expr);
            LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
            register_runtime(ctx->mod, expr->loc, "mix_list_push_bytes",
                             LIR_TY_VOID, ps, 2);
            LirOpnd args[] = { list_p, tmp_p };
            lir_emit_call(ctx->fn, expr->loc, "mix_list_push_bytes",
                            LIR_TY_VOID, args, 2);
        } else {
            LirOpnd v = lower_expr(ctx, expr->list_comp.expr);
            LirOpnd as_i64 = to_storage_i64(ctx, expr->loc, v,
                                               expr->list_comp.expr->resolved_type);
            LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
            register_runtime(ctx->mod, expr->loc, "mix_list_push",
                             LIR_TY_VOID, ps, 2);
            LirOpnd args[] = { list_p, as_i64 };
            lir_emit_call(ctx->fn, expr->loc, "mix_list_push",
                            LIR_TY_VOID, args, 2);
        }

        if (skip_l >= 0) {
            emit_br(ctx, expr->loc, skip_l);
            emit_label(ctx, expr->loc, skip_l);
        }

        ctx->local_count = saved;

        // Increment.
        int next_v = lir_emit_load(ctx->fn, expr->loc, LIR_TY_I64, counter_p);
        int inc_v = lir_emit_bin(ctx->fn, expr->loc, LIR_BIN_ADD, LIR_TY_I64,
                                   lir_opnd_value(next_v, LIR_TY_I64),
                                   lir_opnd_int_typed(1, LIR_TY_I64));
        lir_emit_store(ctx->fn, expr->loc, LIR_TY_I64,
                        lir_opnd_value(inc_v, LIR_TY_I64), counter_p);
        emit_br(ctx, expr->loc, cond_l);

        emit_label(ctx, expr->loc, after_l);
        return list_p;
    }

    case NODE_MAP_LIT: {
        // {"key": val, ...}
        register_runtime(ctx->mod, expr->loc, "mix_map_new",
                         LIR_TY_PTR, NULL, 0);
        int m = lir_emit_call(ctx->fn, expr->loc, "mix_map_new",
                                LIR_TY_PTR, NULL, 0);
        LirOpnd map_p = lir_opnd_value(m, LIR_TY_PTR);
        for (int i = 0; i < expr->map_lit.entry_count; i++) {
            LirOpnd k = lower_expr(ctx, expr->map_lit.keys[i]);
            LirOpnd v = lower_expr(ctx, expr->map_lit.values[i]);
            LirOpnd vi = to_storage_i64(ctx, expr->loc, v,
                                           expr->map_lit.values[i]->resolved_type);
            LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64 };
            register_runtime(ctx->mod, expr->loc, "mix_map_set",
                             LIR_TY_VOID, ps, 3);
            LirOpnd args[] = { map_p, k, vi };
            lir_emit_call(ctx->fn, expr->loc, "mix_map_set",
                            LIR_TY_VOID, args, 3);
        }
        return map_p;
    }

    case NODE_SET_LIT: {
        register_runtime(ctx->mod, expr->loc, "mix_set_new",
                         LIR_TY_PTR, NULL, 0);
        int s = lir_emit_call(ctx->fn, expr->loc, "mix_set_new",
                                LIR_TY_PTR, NULL, 0);
        LirOpnd set_p = lir_opnd_value(s, LIR_TY_PTR);
        for (int i = 0; i < expr->set_lit.element_count; i++) {
            AstNode *e = expr->set_lit.elements[i];
            LirOpnd v = lower_expr(ctx, e);
            // mix_set_add takes (set, const char *) — only string sets
            // are well-supported. For int sets there's no public push
            // helper at the level we have. Try the string path; non-str
            // sets will surface a runtime error.
            LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR };
            register_runtime(ctx->mod, expr->loc, "mix_set_add",
                             LIR_TY_VOID, ps, 2);
            LirOpnd args[] = { set_p, v };
            lir_emit_call(ctx->fn, expr->loc, "mix_set_add",
                            LIR_TY_VOID, args, 2);
        }
        return set_p;
    }

    case NODE_STRING_INTERP: {
        // Build a string by chaining mix_str_concat calls. Each part
        // (literal text) and each interpolated expression is converted
        // to a string and concatenated onto an accumulator.
        LirOpnd acc = lir_opnd_none();
        bool have_acc = false;

        // Helper-call signatures registered lazily.
        LirType cat_p[] = { LIR_TY_PTR, LIR_TY_PTR };
        LirType i2s_p[] = { LIR_TY_I64 };
        LirType f2s_p[] = { LIR_TY_F64 };
        LirType b2s_p[] = { LIR_TY_I32 };

        for (int si = 0; si <= expr->string_interp.expr_count; si++) {
            // Literal segment.
            int plen = expr->string_interp.part_lengths[si];
            if (plen > 0) {
                int sid = lir_intern_string(ctx->mod,
                                              expr->string_interp.parts[si],
                                              plen);
                LirOpnd seg = lir_opnd_string(sid);
                if (!have_acc) {
                    acc = seg;
                    have_acc = true;
                } else {
                    lir_register_callee(ctx->mod, expr->loc, "mix_str_concat",
                                         LIR_TY_PTR, cat_p, 2);
                    LirOpnd args[] = { acc, seg };
                    int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_concat",
                                            LIR_TY_PTR, args, 2);
                    acc = lir_opnd_value(r, LIR_TY_PTR);
                }
            }
            // Interpolated expression.
            if (si < expr->string_interp.expr_count) {
                AstNode *iexpr = expr->string_interp.exprs[si];
                LirOpnd v = lower_expr(ctx, iexpr);
                MixType *etype = iexpr->resolved_type;
                LirOpnd as_str;
                if (v.type == LIR_TY_PTR &&
                    etype && etype->kind == TYPE_LIST) {
                    MixType *elem = etype->list.elem_type;
                    const char *helper = "mix_to_string_list_int";
                    if (elem) {
                        if (elem->kind == TYPE_STR)               helper = "mix_to_string_list_str";
                        else if (elem->kind == TYPE_BOOL)         helper = "mix_to_string_list_bool";
                        else if (elem->kind == TYPE_FLOAT ||
                                 elem->kind == TYPE_FLOAT64 ||
                                 elem->kind == TYPE_FLOAT32)      helper = "mix_to_string_list_float";
                    }
                    LirType ps[] = { LIR_TY_PTR };
                    lir_register_callee(ctx->mod, iexpr->loc, helper,
                                         LIR_TY_PTR, ps, 1);
                    LirOpnd args[] = { v };
                    int r = lir_emit_call(ctx->fn, iexpr->loc, helper,
                                            LIR_TY_PTR, args, 1);
                    as_str = lir_opnd_value(r, LIR_TY_PTR);
                } else if (v.type == LIR_TY_PTR &&
                           etype && etype->kind == TYPE_MAP) {
                    MixType *vt = etype->map.val_type;
                    const char *helper = (vt && vt->kind == TYPE_STR)
                        ? "mix_to_string_map_str" : "mix_to_string_map";
                    LirType ps[] = { LIR_TY_PTR };
                    lir_register_callee(ctx->mod, iexpr->loc, helper,
                                         LIR_TY_PTR, ps, 1);
                    LirOpnd args[] = { v };
                    int r = lir_emit_call(ctx->fn, iexpr->loc, helper,
                                            LIR_TY_PTR, args, 1);
                    as_str = lir_opnd_value(r, LIR_TY_PTR);
                } else if (v.type == LIR_TY_PTR &&
                           etype && etype->kind == TYPE_SET) {
                    MixType *se = etype->set.elem_type;
                    const char *helper = (se && type_is_integer(se))
                        ? "mix_to_string_set_int" : "mix_to_string_set";
                    LirType ps[] = { LIR_TY_PTR };
                    lir_register_callee(ctx->mod, iexpr->loc, helper,
                                         LIR_TY_PTR, ps, 1);
                    LirOpnd args[] = { v };
                    int r = lir_emit_call(ctx->fn, iexpr->loc, helper,
                                            LIR_TY_PTR, args, 1);
                    as_str = lir_opnd_value(r, LIR_TY_PTR);
                } else if (v.type == LIR_TY_PTR) {
                    as_str = v;
                } else if (v.type == LIR_TY_F64) {
                    lir_register_callee(ctx->mod, iexpr->loc,
                                         "mix_to_string_float",
                                         LIR_TY_PTR, f2s_p, 1);
                    LirOpnd args[] = { v };
                    int r = lir_emit_call(ctx->fn, iexpr->loc,
                                            "mix_to_string_float",
                                            LIR_TY_PTR, args, 1);
                    as_str = lir_opnd_value(r, LIR_TY_PTR);
                } else if (v.type == LIR_TY_I1) {
                    lir_register_callee(ctx->mod, iexpr->loc,
                                         "mix_to_string_bool",
                                         LIR_TY_PTR, b2s_p, 1);
                    LirOpnd args[] = { to_i32(ctx, iexpr->loc, v) };
                    int r = lir_emit_call(ctx->fn, iexpr->loc,
                                            "mix_to_string_bool",
                                            LIR_TY_PTR, args, 1);
                    as_str = lir_opnd_value(r, LIR_TY_PTR);
                } else if (is_int_lir(v.type)) {
                    lir_register_callee(ctx->mod, iexpr->loc,
                                         "mix_to_string_int",
                                         LIR_TY_PTR, i2s_p, 1);
                    LirOpnd args[] = { widen_to_i64(ctx, iexpr->loc, v,
                                                       iexpr->resolved_type) };
                    int r = lir_emit_call(ctx->fn, iexpr->loc,
                                            "mix_to_string_int",
                                            LIR_TY_PTR, args, 1);
                    as_str = lir_opnd_value(r, LIR_TY_PTR);
                } else {
                    unsupported(iexpr, "string interpolation of unsupported type");
                    return lir_opnd_none();
                }

                if (!have_acc) {
                    acc = as_str;
                    have_acc = true;
                } else {
                    lir_register_callee(ctx->mod, expr->loc, "mix_str_concat",
                                         LIR_TY_PTR, cat_p, 2);
                    LirOpnd args[] = { acc, as_str };
                    int r = lir_emit_call(ctx->fn, expr->loc, "mix_str_concat",
                                            LIR_TY_PTR, args, 2);
                    acc = lir_opnd_value(r, LIR_TY_PTR);
                }
            }
        }
        if (!have_acc) {
            int sid = lir_intern_string(ctx->mod, "", 0);
            return lir_opnd_string(sid);
        }
        return acc;
    }

    case NODE_SHAPE_LIT: {
        // Standalone shape literal: alloca a temp, fill, return the ptr.
        MixType *st = expr->resolved_type;
        if (!mix_is_shape(st)) {
            unsupported(expr, "shape literal without resolved shape type");
            return lir_opnd_none();
        }
        int tmp = lir_emit_alloca_bytes(ctx->fn, expr->loc,
                                          st->shape.total_size,
                                          st->shape.alignment);
        LirOpnd tmp_p = lir_opnd_value(tmp, LIR_TY_PTR);
        lower_shape_lit_into(ctx, tmp_p, st, expr);
        return tmp_p;
    }

    default:
        unsupported(expr, "expression kind");
        return lir_opnd_none();
    }
}

// ---- Statements ------------------------------------------------------------

static int ensure_scalar_slot(LowerCtx *ctx, SrcLoc loc, const char *name,
                                LirType type)
{
    Local *existing = scope_lookup(ctx, name);
    if (existing && !existing->shape_type) return existing->value_id;
    int a = lir_emit_alloca(ctx->fn, loc, type);
    Local *l = scope_add(ctx, name);
    l->value_id = a;
    l->scalar_type = type;
    return a;
}

static void lower_var_decl(LowerCtx *ctx, AstNode *vd) {
    MixType *t = vd->resolved_type;
    if (mix_is_shape(t)) {
        // Allocate the slot directly; lower the initializer into it.
        int slot = lir_emit_alloca_bytes(ctx->fn, vd->loc,
                                           t->shape.total_size,
                                           t->shape.alignment);
        Local *l = scope_add(ctx, vd->var_decl.name);
        l->value_id = slot;
        l->shape_type = t;
        if (vd->var_decl.init_expr) {
            lower_init_into(ctx, lir_opnd_value(slot, LIR_TY_PTR), t,
                              vd->var_decl.init_expr);
        }
        return;
    }

    LirType lt = mix_to_lir(t);
    if (lt == LIR_TY_VOID) {
        unsupported(vd, "var decl with unsupported type");
        return;
    }
    // Always allocate a fresh slot for a var decl. MIX allows shadowing
    // (including type-changing shadow like `y = 7; y! = "seven"`) — the
    // new binding must not stomp the old slot.
    int slot = lir_emit_alloca(ctx->fn, vd->loc, lt);
    Local *l = scope_add(ctx, vd->var_decl.name);
    l->value_id = slot;
    l->scalar_type = lt;

    if (vd->var_decl.init_expr) {
        LirOpnd val = lower_expr(ctx, vd->var_decl.init_expr);
        // Best-effort coercion to slot type when sema's type and the
        // emitted value disagree.
        if (val.type != lt) {
            if (is_int_lir(val.type) && is_int_lir(lt)) {
                int r = lir_emit_conv(ctx->fn, vd->loc,
                                       lt == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                       val.type, lt, val);
                val = lir_opnd_value(r, lt);
            } else if (val.type == LIR_TY_PTR && is_int_lir(lt)) {
                int r = lir_emit_conv(ctx->fn, vd->loc, LIR_CONV_PTRTOINT,
                                       LIR_TY_PTR, lt, val);
                val = lir_opnd_value(r, lt);
            } else if (is_int_lir(val.type) && lt == LIR_TY_PTR) {
                int r = lir_emit_conv(ctx->fn, vd->loc, LIR_CONV_INTTOPTR,
                                       val.type, LIR_TY_PTR, val);
                val = lir_opnd_value(r, LIR_TY_PTR);
            } else if (is_float_lir(val.type) && is_int_lir(lt)) {
                LirOpnd as_f64 = float_cast(ctx, vd->loc, val, LIR_TY_F64);
                int r = lir_emit_conv(ctx->fn, vd->loc, LIR_CONV_FPTOSI,
                                       LIR_TY_F64, lt, as_f64);
                val = lir_opnd_value(r, lt);
            } else if (is_int_lir(val.type) && is_float_lir(lt)) {
                LirOpnd w = widen_to_i64(ctx, vd->loc, val,
                                            vd->var_decl.init_expr->resolved_type);
                int r = lir_emit_conv(ctx->fn, vd->loc, LIR_CONV_SITOFP,
                                       LIR_TY_I64, LIR_TY_F64, w);
                val = float_cast(ctx, vd->loc,
                                   lir_opnd_value(r, LIR_TY_F64), lt);
            } else if (is_float_lir(val.type) && is_float_lir(lt)) {
                val = float_cast(ctx, vd->loc, val, lt);
            }
        }
        lir_emit_store(ctx->fn, vd->loc, lt, val,
                        lir_opnd_value(slot, LIR_TY_PTR));
    }
}

static void lower_assign(LowerCtx *ctx, AstNode *as) {
    Local *l = scope_lookup(ctx, as->assign.name);
    if (!l) {
        // Maybe a module-level global.
        GlobalEntry *g = find_global(as->assign.name);
        if (g) {
            LirOpnd addr = lir_opnd_fn_ref(g->name);
            LirOpnd rhs = lower_expr(ctx, as->assign.value);
            LirOpnd to_store = rhs;
            if (as->assign.op != TOK_EQ) {
                int curv = lir_emit_load(ctx->fn, as->loc, g->lir_type, addr);
                LirOpnd cur = lir_opnd_value(curv, g->lir_type);
                LirBinOp bop;
                switch (as->assign.op) {
                    case TOK_PLUS_EQ:  bop = LIR_BIN_ADD; break;
                    case TOK_MINUS_EQ: bop = LIR_BIN_SUB; break;
                    case TOK_STAR_EQ:  bop = LIR_BIN_MUL; break;
                    case TOK_SLASH_EQ: bop = LIR_BIN_DIV; break;
                    default: unsupported(as, "compound assignment operator"); return;
                }
                int r = lir_emit_bin(ctx->fn, as->loc, bop, g->lir_type, cur, rhs);
                to_store = lir_opnd_value(r, g->lir_type);
            }
            // Coerce store value to the global's type.
            if (to_store.type != g->lir_type && is_int_lir(to_store.type) &&
                is_int_lir(g->lir_type)) {
                int r = lir_emit_conv(ctx->fn, as->loc,
                                       g->lir_type == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                       to_store.type, g->lir_type, to_store);
                to_store = lir_opnd_value(r, g->lir_type);
            }
            lir_emit_store(ctx->fn, as->loc, g->lir_type, to_store, addr);
            return;
        }
        unsupported(as, "assignment to unknown local"); return;
    }

    if (l->shape_type) {
        // Whole-shape reassignment.
        if (as->assign.op != TOK_EQ) {
            unsupported(as, "compound assignment on shape");
            return;
        }
        lower_init_into(ctx, lir_opnd_value(l->value_id, LIR_TY_PTR),
                          l->shape_type, as->assign.value);
        return;
    }

    LirOpnd rhs = lower_expr(ctx, as->assign.value);
    LirOpnd to_store = rhs;
    if (as->assign.op != TOK_EQ) {
        LirOpnd cur = lir_opnd_value(
            lir_emit_load(ctx->fn, as->loc, l->scalar_type,
                           lir_opnd_value(l->value_id, LIR_TY_PTR)),
            l->scalar_type);
        LirBinOp bop;
        switch (as->assign.op) {
            case TOK_PLUS_EQ:  bop = LIR_BIN_ADD; break;
            case TOK_MINUS_EQ: bop = LIR_BIN_SUB; break;
            case TOK_STAR_EQ:  bop = LIR_BIN_MUL; break;
            case TOK_SLASH_EQ: bop = LIR_BIN_DIV; break;
            default: unsupported(as, "compound assignment operator"); return;
        }
        int r = lir_emit_bin(ctx->fn, as->loc, bop, l->scalar_type, cur, rhs);
        to_store = lir_opnd_value(r, l->scalar_type);
    }
    lir_emit_store(ctx->fn, as->loc, l->scalar_type, to_store,
                    lir_opnd_value(l->value_id, LIR_TY_PTR));
}

static void lower_field_assign(LowerCtx *ctx, AstNode *fa) {
    AstNode *obj = fa->field_assign.object;
    MixType *obj_type = obj->resolved_type;
    MixType *shape_t = unwrap_to_shape(obj_type);
    if (!shape_t) {
        unsupported(fa, "field assign on non-shape");
        return;
    }
    ShapeFieldInfo *f = find_field(shape_t, fa->field_assign.field_name);
    if (!f) { unsupported(fa, "field name not in shape"); return; }

    LirOpnd base = lower_expr(ctx, obj);
    if (base.type != LIR_TY_PTR) {
        unsupported(fa, "field assign requires shape ptr");
        return;
    }
    base = unwrap_box_runtime(ctx, fa->loc, base, obj_type);
    LirOpnd field_addr = lir_opnd_value(
        lir_emit_ptr_offset(ctx->fn, fa->loc, base, f->offset),
        LIR_TY_PTR);

    if (mix_is_shape(f->type)) {
        if (fa->field_assign.op != TOK_EQ) {
            unsupported(fa, "compound assignment on shape field");
            return;
        }
        lower_init_into(ctx, field_addr, f->type, fa->field_assign.value);
        return;
    }

    LirType ft = mix_to_lir(f->type);
    LirOpnd rhs = lower_expr(ctx, fa->field_assign.value);
    LirOpnd to_store = rhs;
    if (fa->field_assign.op != TOK_EQ) {
        LirOpnd cur = lir_opnd_value(
            lir_emit_load(ctx->fn, fa->loc, ft, field_addr), ft);
        LirBinOp bop;
        switch (fa->field_assign.op) {
            case TOK_PLUS_EQ:  bop = LIR_BIN_ADD; break;
            case TOK_MINUS_EQ: bop = LIR_BIN_SUB; break;
            case TOK_STAR_EQ:  bop = LIR_BIN_MUL; break;
            case TOK_SLASH_EQ: bop = LIR_BIN_DIV; break;
            default: unsupported(fa, "compound assignment operator"); return;
        }
        int r = lir_emit_bin(ctx->fn, fa->loc, bop, ft, cur, rhs);
        to_store = lir_opnd_value(r, ft);
    }
    if (to_store.type != ft) {
        if (is_int_lir(to_store.type) && is_int_lir(ft)) {
            int r = lir_emit_conv(ctx->fn, fa->loc,
                                   ft == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                   to_store.type, ft, to_store);
            to_store = lir_opnd_value(r, ft);
        } else if (to_store.type == LIR_TY_PTR && is_int_lir(ft)) {
            int r = lir_emit_conv(ctx->fn, fa->loc, LIR_CONV_PTRTOINT,
                                   LIR_TY_PTR, LIR_TY_I64, to_store);
            LirOpnd as_i64 = lir_opnd_value(r, LIR_TY_I64);
            if (ft != LIR_TY_I64) {
                int r2 = lir_emit_conv(ctx->fn, fa->loc, LIR_CONV_TRUNC,
                                         LIR_TY_I64, ft, as_i64);
                to_store = lir_opnd_value(r2, ft);
            } else {
                to_store = as_i64;
            }
        } else if (is_int_lir(to_store.type) && ft == LIR_TY_PTR) {
            int r = lir_emit_conv(ctx->fn, fa->loc, LIR_CONV_INTTOPTR,
                                   to_store.type, LIR_TY_PTR, to_store);
            to_store = lir_opnd_value(r, LIR_TY_PTR);
        } else if (is_float_lir(to_store.type) && is_float_lir(ft)) {
            to_store = float_cast(ctx, fa->loc, to_store, ft);
        } else if (is_int_lir(to_store.type) && is_float_lir(ft)) {
            LirOpnd w = widen_to_i64(ctx, fa->loc, to_store,
                                        fa->field_assign.value->resolved_type);
            int r = lir_emit_conv(ctx->fn, fa->loc, LIR_CONV_SITOFP,
                                   LIR_TY_I64, LIR_TY_F64, w);
            to_store = float_cast(ctx, fa->loc,
                                    lir_opnd_value(r, LIR_TY_F64), ft);
        }
    }
    lir_emit_store(ctx->fn, fa->loc, ft, to_store, field_addr);
}

static void lower_block_stmts(LowerCtx *ctx, AstNode *block) {
    if (!block) return;
    if (block->kind == NODE_BLOCK) {
        for (int i = 0; i < block->block.stmt_count; i++) {
            lower_stmt(ctx, block->block.stmts[i]);
            if (mix_error_count() > 0) return;
        }
    } else {
        lower_stmt(ctx, block);
    }
}

static void lower_if(LowerCtx *ctx, AstNode *if_stmt) {
    int then_l = lir_func_new_label(ctx->fn);
    int merge_l = lir_func_new_label(ctx->fn);
    int else_l = if_stmt->if_stmt.else_block ? lir_func_new_label(ctx->fn) : merge_l;

    LirOpnd cond = lower_expr(ctx, if_stmt->if_stmt.condition);
    if (cond.type != LIR_TY_I1) {
        LirOpnd zero = lir_opnd_int_typed(0, cond.type);
        int r = lir_emit_bin(ctx->fn, if_stmt->loc, LIR_BIN_NE, cond.type, cond, zero);
        cond = lir_opnd_value(r, LIR_TY_I1);
    }
    emit_br_cond(ctx, if_stmt->loc, cond, then_l, else_l);

    emit_label(ctx, if_stmt->loc, then_l);
    {
        // New lexical scope: vars declared inside the then-block must not
        // outlive it. Save/restore local count so an inner shadow of an
        // outer name is dropped at block end.
        int saved = ctx->local_count;
        lower_block_stmts(ctx, if_stmt->if_stmt.then_block);
        ctx->local_count = saved;
    }
    emit_br(ctx, if_stmt->loc, merge_l);

    if (if_stmt->if_stmt.else_block) {
        emit_label(ctx, if_stmt->loc, else_l);
        int saved = ctx->local_count;
        lower_block_stmts(ctx, if_stmt->if_stmt.else_block);
        ctx->local_count = saved;
        emit_br(ctx, if_stmt->loc, merge_l);
    }
    emit_label(ctx, if_stmt->loc, merge_l);
}

static void lower_while(LowerCtx *ctx, AstNode *w) {
    int cond_l  = lir_func_new_label(ctx->fn);
    int body_l  = lir_func_new_label(ctx->fn);
    int after_l = lir_func_new_label(ctx->fn);

    emit_br(ctx, w->loc, cond_l);
    emit_label(ctx, w->loc, cond_l);

    LirOpnd cond = lower_expr(ctx, w->while_stmt.condition);
    if (cond.type != LIR_TY_I1) {
        LirOpnd zero = lir_opnd_int_typed(0, cond.type);
        int r = lir_emit_bin(ctx->fn, w->loc, LIR_BIN_NE, cond.type, cond, zero);
        cond = lir_opnd_value(r, LIR_TY_I1);
    }
    emit_br_cond(ctx, w->loc, cond, body_l, after_l);

    emit_label(ctx, w->loc, body_l);
    if (ctx->loop_depth < 64) {
        ctx->loop_break[ctx->loop_depth] = after_l;
        ctx->loop_continue[ctx->loop_depth] = cond_l;
        ctx->loop_depth++;
    }
    int saved_locals = ctx->local_count;
    lower_block_stmts(ctx, w->while_stmt.body);
    ctx->local_count = saved_locals;
    if (ctx->loop_depth > 0) ctx->loop_depth--;
    emit_br(ctx, w->loc, cond_l);

    emit_label(ctx, w->loc, after_l);
}

// for x in list / for i, x in list / for x! in list (mutable writeback).
static void lower_for_list(LowerCtx *ctx, AstNode *f, MixType *list_type) {
    MixType *elem = list_type->list.elem_type;
    AstNode *iter = f->for_stmt.iterable;

    LirOpnd list = lower_expr(ctx, iter);

    // Counter local (the index, or hidden if no `i, x` form).
    int counter_slot = lir_emit_alloca(ctx->fn, f->loc, LIR_TY_I64);
    LirOpnd counter_p = lir_opnd_value(counter_slot, LIR_TY_PTR);
    lir_emit_store(ctx->fn, f->loc, LIR_TY_I64,
                    lir_opnd_int_typed(0, LIR_TY_I64), counter_p);

    if (f->for_stmt.index_name) {
        Local *li = scope_add(ctx, f->for_stmt.index_name);
        li->value_id = counter_slot;
        li->scalar_type = LIR_TY_I64;
    }

    // Cache list length once at loop entry (mix_list_len is cheap; this
    // matches QBE behavior).
    LirType len_p[] = { LIR_TY_PTR };
    register_runtime(ctx->mod, f->loc, "mix_list_len", LIR_TY_I64, len_p, 1);
    LirOpnd len_args[] = { list };
    int len_v = lir_emit_call(ctx->fn, f->loc, "mix_list_len", LIR_TY_I64,
                                 len_args, 1);
    LirOpnd len = lir_opnd_value(len_v, LIR_TY_I64);

    int cond_l  = lir_func_new_label(ctx->fn);
    int body_l  = lir_func_new_label(ctx->fn);
    int inc_l   = lir_func_new_label(ctx->fn);
    int after_l = lir_func_new_label(ctx->fn);

    emit_br(ctx, f->loc, cond_l);
    emit_label(ctx, f->loc, cond_l);

    int cur_v = lir_emit_load(ctx->fn, f->loc, LIR_TY_I64, counter_p);
    LirOpnd cur = lir_opnd_value(cur_v, LIR_TY_I64);
    int cmp_v = lir_emit_bin(ctx->fn, f->loc, LIR_BIN_LT, LIR_TY_I64, cur, len);
    emit_br_cond(ctx, f->loc, lir_opnd_value(cmp_v, LIR_TY_I1), body_l, after_l);

    emit_label(ctx, f->loc, body_l);

    // Push break/continue labels. continue jumps to the increment block
    // so the writeback (mutable foreach) and counter increment still run
    // before falling back to cond_l.
    if (ctx->loop_depth < 64) {
        ctx->loop_break[ctx->loop_depth] = after_l;
        ctx->loop_continue[ctx->loop_depth] = inc_l;
        ctx->loop_depth++;
    }

    // Bind the per-iteration value. Shape elements: bind the ptr (no
    // copy). Scalar elements: load via mix_list_get and bind a local.
    int saved_local_count = ctx->local_count;
    if (elem && elem->kind == TYPE_SHAPE) {
        LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, f->loc, "mix_list_ptr", LIR_TY_PTR, ps, 2);
        LirOpnd args[] = { list, lir_opnd_value(cur_v, LIR_TY_I64) };
        int p = lir_emit_call(ctx->fn, f->loc, "mix_list_ptr",
                                LIR_TY_PTR, args, 2);
        Local *l = scope_add(ctx, f->for_stmt.var_name);
        l->value_id = p;
        l->shape_type = elem;
    } else {
        LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, f->loc, "mix_list_get", LIR_TY_I64, ps, 2);
        LirOpnd args[] = { list, lir_opnd_value(cur_v, LIR_TY_I64) };
        int raw = lir_emit_call(ctx->fn, f->loc, "mix_list_get",
                                  LIR_TY_I64, args, 2);
        LirOpnd val = from_storage_i64(ctx, f->loc,
                                          lir_opnd_value(raw, LIR_TY_I64), elem);
        // Store into a fresh local slot so the body can read like any
        // other local.
        LirType lt = mix_to_lir(elem);
        if (lt == LIR_TY_VOID) lt = LIR_TY_I64;
        int slot = lir_emit_alloca(ctx->fn, f->loc, lt);
        lir_emit_store(ctx->fn, f->loc, lt, val,
                        lir_opnd_value(slot, LIR_TY_PTR));
        Local *l = scope_add(ctx, f->for_stmt.var_name);
        l->value_id = slot;
        l->scalar_type = lt;
    }

    lower_block_stmts(ctx, f->for_stmt.body);

    // Body falls through to inc_l unconditionally; both natural exit and
    // explicit `continue` route through it so the writeback runs.
    if (!ctx->block_terminated) {
        emit_br(ctx, f->loc, inc_l);
    }

    emit_label(ctx, f->loc, inc_l);
    // Mutable foreach writeback: if `var_is_mutable` and the elem is
    // scalar (we stored a copy in the slot), write the slot's value
    // back to the list before incrementing.
    if (f->for_stmt.var_is_mutable && elem && elem->kind != TYPE_SHAPE) {
        Local *l = scope_lookup(ctx, f->for_stmt.var_name);
        if (l) {
            int v = lir_emit_load(ctx->fn, f->loc, l->scalar_type,
                                   lir_opnd_value(l->value_id, LIR_TY_PTR));
            LirOpnd back = to_storage_i64(ctx, f->loc,
                                            lir_opnd_value(v, l->scalar_type),
                                            elem);
            LirType ps[] = { LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64 };
            register_runtime(ctx->mod, f->loc, "mix_list_set",
                             LIR_TY_VOID, ps, 3);
            LirOpnd args[] = { list, lir_opnd_value(cur_v, LIR_TY_I64), back };
            lir_emit_call(ctx->fn, f->loc, "mix_list_set", LIR_TY_VOID,
                            args, 3);
        }
    }
    // Increment counter.
    int next_v = lir_emit_load(ctx->fn, f->loc, LIR_TY_I64, counter_p);
    int inc_v = lir_emit_bin(ctx->fn, f->loc, LIR_BIN_ADD, LIR_TY_I64,
                              lir_opnd_value(next_v, LIR_TY_I64),
                              lir_opnd_int_typed(1, LIR_TY_I64));
    lir_emit_store(ctx->fn, f->loc, LIR_TY_I64,
                    lir_opnd_value(inc_v, LIR_TY_I64), counter_p);
    emit_br(ctx, f->loc, cond_l);

    // Drop loop-local bindings.
    ctx->local_count = saved_local_count;
    if (ctx->loop_depth > 0) ctx->loop_depth--;

    emit_label(ctx, f->loc, after_l);
}

// For-set iteration: lower as `for x in mix_set_values(set)` (a list of strs).
static void lower_for_set(LowerCtx *ctx, AstNode *f, MixType *set_type) {
    LirOpnd s = lower_expr(ctx, f->for_stmt.iterable);
    LirType ps[] = { LIR_TY_PTR };
    register_runtime(ctx->mod, f->loc, "mix_set_values", LIR_TY_PTR, ps, 1);
    LirOpnd args[] = { s };
    int list_v = lir_emit_call(ctx->fn, f->loc, "mix_set_values",
                                 LIR_TY_PTR, args, 1);
    // Synthesize an iter type of [str] for the inner loop.
    MixType *inner = arena_alloc(ctx->mod->arena, sizeof(MixType));
    memset(inner, 0, sizeof(*inner));
    inner->kind = TYPE_LIST;
    inner->list.elem_type = set_type->set.elem_type;
    AstNode *fake_iter = arena_alloc(ctx->mod->arena, sizeof(AstNode));
    memset(fake_iter, 0, sizeof(*fake_iter));
    fake_iter->kind = NODE_INT_LIT;
    fake_iter->loc = f->loc;
    fake_iter->resolved_type = inner;
    AstNode synth_for = *f;
    synth_for.for_stmt.iterable = fake_iter;
    // Inject the materialized list ptr by lowering the iterable directly
    // through a small one-shot wrapper: easiest is to reuse lower_for_list
    // but it expects to lower the iterable expression itself. Instead,
    // do an inline minimal version.
    (void)synth_for;
    LirOpnd list = lir_opnd_value(list_v, LIR_TY_PTR);

    int counter_slot = lir_emit_alloca(ctx->fn, f->loc, LIR_TY_I64);
    LirOpnd counter_p = lir_opnd_value(counter_slot, LIR_TY_PTR);
    lir_emit_store(ctx->fn, f->loc, LIR_TY_I64,
                    lir_opnd_int_typed(0, LIR_TY_I64), counter_p);
    LirType lp[] = { LIR_TY_PTR };
    register_runtime(ctx->mod, f->loc, "mix_list_len", LIR_TY_I64, lp, 1);
    LirOpnd la[] = { list };
    int len_v = lir_emit_call(ctx->fn, f->loc, "mix_list_len", LIR_TY_I64, la, 1);
    LirOpnd len = lir_opnd_value(len_v, LIR_TY_I64);

    int cond_l  = lir_func_new_label(ctx->fn);
    int body_l  = lir_func_new_label(ctx->fn);
    int after_l = lir_func_new_label(ctx->fn);

    emit_br(ctx, f->loc, cond_l);
    emit_label(ctx, f->loc, cond_l);

    int cur_v = lir_emit_load(ctx->fn, f->loc, LIR_TY_I64, counter_p);
    int cmp_v = lir_emit_bin(ctx->fn, f->loc, LIR_BIN_LT, LIR_TY_I64,
                               lir_opnd_value(cur_v, LIR_TY_I64), len);
    emit_br_cond(ctx, f->loc, lir_opnd_value(cmp_v, LIR_TY_I1), body_l, after_l);

    emit_label(ctx, f->loc, body_l);
    if (ctx->loop_depth < 64) {
        ctx->loop_break[ctx->loop_depth] = after_l;
        ctx->loop_continue[ctx->loop_depth] = cond_l;
        ctx->loop_depth++;
    }
    int saved = ctx->local_count;

    LirType gp[] = { LIR_TY_PTR, LIR_TY_I64 };
    register_runtime(ctx->mod, f->loc, "mix_list_get", LIR_TY_I64, gp, 2);
    LirOpnd ga[] = { list, lir_opnd_value(cur_v, LIR_TY_I64) };
    int raw = lir_emit_call(ctx->fn, f->loc, "mix_list_get", LIR_TY_I64, ga, 2);
    // Set elements are strings (pointers).
    int slot = lir_emit_alloca(ctx->fn, f->loc, LIR_TY_PTR);
    int as_ptr = lir_emit_conv(ctx->fn, f->loc, LIR_CONV_INTTOPTR,
                                 LIR_TY_I64, LIR_TY_PTR,
                                 lir_opnd_value(raw, LIR_TY_I64));
    lir_emit_store(ctx->fn, f->loc, LIR_TY_PTR,
                    lir_opnd_value(as_ptr, LIR_TY_PTR),
                    lir_opnd_value(slot, LIR_TY_PTR));
    Local *l = scope_add(ctx, f->for_stmt.var_name);
    l->value_id = slot;
    l->scalar_type = LIR_TY_PTR;

    lower_block_stmts(ctx, f->for_stmt.body);
    ctx->local_count = saved;
    if (ctx->loop_depth > 0) ctx->loop_depth--;
    if (!ctx->block_terminated) {
        int next_v = lir_emit_load(ctx->fn, f->loc, LIR_TY_I64, counter_p);
        int inc_v = lir_emit_bin(ctx->fn, f->loc, LIR_BIN_ADD, LIR_TY_I64,
                                   lir_opnd_value(next_v, LIR_TY_I64),
                                   lir_opnd_int_typed(1, LIR_TY_I64));
        lir_emit_store(ctx->fn, f->loc, LIR_TY_I64,
                        lir_opnd_value(inc_v, LIR_TY_I64), counter_p);
        emit_br(ctx, f->loc, cond_l);
    }
    emit_label(ctx, f->loc, after_l);
}

static void lower_for_range(LowerCtx *ctx, AstNode *f) {
    AstNode *iter = f->for_stmt.iterable;
    MixType *iter_type = iter ? iter->resolved_type : NULL;

    if (iter_type && iter_type->kind == TYPE_LIST) {
        lower_for_list(ctx, f, iter_type);
        return;
    }
    if (iter_type && iter_type->kind == TYPE_SET) {
        lower_for_set(ctx, f, iter_type);
        return;
    }

    if (iter->kind != NODE_BINARY_EXPR ||
        (iter->binary.op != TOK_DOTDOT && iter->binary.op != TOK_DOTDOT_EQ)) {
        unsupported(f, "for over non-range iterable");
        return;
    }
    bool inclusive = (iter->binary.op == TOK_DOTDOT_EQ);

    LirType counter_ty = LIR_TY_I64;
    int slot = ensure_scalar_slot(ctx, f->loc, f->for_stmt.var_name, counter_ty);

    LirOpnd start = lower_expr(ctx, iter->binary.left);
    LirOpnd end   = lower_expr(ctx, iter->binary.right);
    if (start.type != counter_ty) start = to_i64(ctx, f->loc, start);
    if (end.type   != counter_ty) end   = to_i64(ctx, f->loc, end);

    lir_emit_store(ctx->fn, f->loc, counter_ty, start,
                    lir_opnd_value(slot, LIR_TY_PTR));

    int cond_l  = lir_func_new_label(ctx->fn);
    int body_l  = lir_func_new_label(ctx->fn);
    int inc_l   = lir_func_new_label(ctx->fn);   // continue lands here
    int after_l = lir_func_new_label(ctx->fn);

    emit_br(ctx, f->loc, cond_l);
    emit_label(ctx, f->loc, cond_l);

    int cur_v = lir_emit_load(ctx->fn, f->loc, counter_ty,
                                lir_opnd_value(slot, LIR_TY_PTR));
    LirOpnd cur = lir_opnd_value(cur_v, counter_ty);
    LirBinOp cmp = inclusive ? LIR_BIN_LE : LIR_BIN_LT;
    int cmp_v = lir_emit_bin(ctx->fn, f->loc, cmp, counter_ty, cur, end);
    emit_br_cond(ctx, f->loc, lir_opnd_value(cmp_v, LIR_TY_I1), body_l, after_l);

    emit_label(ctx, f->loc, body_l);
    if (ctx->loop_depth < 64) {
        ctx->loop_break[ctx->loop_depth] = after_l;
        ctx->loop_continue[ctx->loop_depth] = inc_l;
        ctx->loop_depth++;
    }
    lower_block_stmts(ctx, f->for_stmt.body);
    if (ctx->loop_depth > 0) ctx->loop_depth--;
    if (!ctx->block_terminated) emit_br(ctx, f->loc, inc_l);

    emit_label(ctx, f->loc, inc_l);
    {
        int next_v = lir_emit_load(ctx->fn, f->loc, counter_ty,
                                    lir_opnd_value(slot, LIR_TY_PTR));
        int inc_v = lir_emit_bin(ctx->fn, f->loc, LIR_BIN_ADD, counter_ty,
                                  lir_opnd_value(next_v, counter_ty),
                                  lir_opnd_int_typed(1, counter_ty));
        lir_emit_store(ctx->fn, f->loc, counter_ty,
                        lir_opnd_value(inc_v, counter_ty),
                        lir_opnd_value(slot, LIR_TY_PTR));
        emit_br(ctx, f->loc, cond_l);
    }

    emit_label(ctx, f->loc, after_l);
}

static void lower_done(LowerCtx *ctx, AstNode *d) {
    if (!d->done_stmt.value) {
        // Bare `done` — emit a typed return so void/non-void/sret all work.
        if (ctx->sret_value_id >= 0) { emit_ret_void(ctx, d->loc); return; }
        LirType rt = ctx->fn->return_type;
        if (rt == LIR_TY_VOID) emit_ret_void(ctx, d->loc);
        else if (rt == LIR_TY_F64) emit_ret_value(ctx, d->loc, lir_opnd_f64(0.0));
        else if (rt == LIR_TY_PTR) emit_ret_value(ctx, d->loc, lir_opnd_int_typed(0, LIR_TY_PTR));
        else emit_ret_value(ctx, d->loc, lir_opnd_int_typed(0, rt));
        return;
    }

    // Shape return path: write into sret slot, ret void.
    if (ctx->sret_value_id >= 0) {
        LirOpnd dst = lir_opnd_value(ctx->sret_value_id, LIR_TY_PTR);
        lower_init_into(ctx, dst, ctx->sret_shape_type, d->done_stmt.value);
        emit_ret_void(ctx, d->loc);
        return;
    }

    LirOpnd v = lower_expr(ctx, d->done_stmt.value);

    // Optional-returning function with `done x` where x is the unwrapped
    // value: wrap with mix_optional_some(x) so the runtime tag {has, val}
    // is materialized and a ptr returned.
    MixType *fnrt = ctx->fn_return_mix_type;
    bool wraps_optional = fnrt && fnrt->kind == TYPE_OPTIONAL &&
                          v.type != LIR_TY_PTR;
    bool wraps_result = fnrt && fnrt->kind == TYPE_RESULT;
    if (wraps_optional) {
        LirOpnd as_i64 = to_storage_i64(ctx, d->loc, v,
                                         d->done_stmt.value->resolved_type);
        LirType ps[] = { LIR_TY_I64 };
        register_runtime(ctx->mod, d->loc, "mix_optional_some",
                         LIR_TY_PTR, ps, 1);
        LirOpnd args[] = { as_i64 };
        int r = lir_emit_call(ctx->fn, d->loc, "mix_optional_some",
                                LIR_TY_PTR, args, 1);
        v = lir_opnd_value(r, LIR_TY_PTR);
    } else if (wraps_result) {
        // `done x` in a Result-returning fn → mix_result_ok(x as i64).
        LirOpnd as_i64 = to_storage_i64(ctx, d->loc, v,
                                         d->done_stmt.value->resolved_type);
        LirType ps[] = { LIR_TY_I64 };
        register_runtime(ctx->mod, d->loc, "mix_result_ok",
                         LIR_TY_PTR, ps, 1);
        LirOpnd args[] = { as_i64 };
        int r = lir_emit_call(ctx->fn, d->loc, "mix_result_ok",
                                LIR_TY_PTR, args, 1);
        v = lir_opnd_value(r, LIR_TY_PTR);
    } else if (v.type != ctx->fn->return_type &&
        is_int_lir(v.type) && is_int_lir(ctx->fn->return_type)) {
        int r = lir_emit_conv(ctx->fn, d->loc,
                               ctx->fn->return_type == LIR_TY_I64
                                   ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                               v.type, ctx->fn->return_type, v);
        v = lir_opnd_value(r, ctx->fn->return_type);
    }
    emit_ret_value(ctx, d->loc, v);
}

static void lower_index_assign(LowerCtx *ctx, AstNode *ia) {
    AstNode *obj = ia->index_assign.object;
    MixType *ot  = obj->resolved_type;
    if (ot && ot->kind == TYPE_MAP) {
        LirOpnd m = lower_expr(ctx, obj);
        LirOpnd k = lower_expr(ctx, ia->index_assign.index);
        LirOpnd v = lower_expr(ctx, ia->index_assign.value);
        LirOpnd vi = to_storage_i64(ctx, ia->loc, v,
                                       ia->index_assign.value->resolved_type);
        LirType ps[] = { LIR_TY_PTR, LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, ia->loc, "mix_map_set", LIR_TY_VOID, ps, 3);
        LirOpnd args[] = { m, k, vi };
        lir_emit_call(ctx->fn, ia->loc, "mix_map_set", LIR_TY_VOID, args, 3);
        return;
    }
    if (!ot || ot->kind != TYPE_LIST) {
        unsupported(ia, "index assign on non-list/map");
        return;
    }
    MixType *elem = ot->list.elem_type;
    LirOpnd list = lower_expr(ctx, obj);
    LirOpnd idx  = lower_expr(ctx, ia->index_assign.index);
    LirOpnd idx_i64 = to_i64(ctx, ia->loc, idx);

    if (elem && elem->kind == TYPE_SHAPE) {
        // For shape elements, get a pointer and memcpy/init into it.
        LirType ps[] = { LIR_TY_PTR, LIR_TY_I64 };
        register_runtime(ctx->mod, ia->loc, "mix_list_ptr",
                         LIR_TY_PTR, ps, 2);
        LirOpnd args[] = { list, idx_i64 };
        int p = lir_emit_call(ctx->fn, ia->loc, "mix_list_ptr",
                                LIR_TY_PTR, args, 2);
        lower_init_into(ctx, lir_opnd_value(p, LIR_TY_PTR), elem,
                          ia->index_assign.value);
        return;
    }

    LirOpnd v = lower_expr(ctx, ia->index_assign.value);
    LirOpnd as_i64 = to_storage_i64(ctx, ia->loc, v,
                                       ia->index_assign.value->resolved_type);
    LirType ps[] = { LIR_TY_PTR, LIR_TY_I64, LIR_TY_I64 };
    register_runtime(ctx->mod, ia->loc, "mix_list_set", LIR_TY_VOID, ps, 3);
    LirOpnd args[] = { list, idx_i64, as_i64 };
    lir_emit_call(ctx->fn, ia->loc, "mix_list_set", LIR_TY_VOID, args, 3);
}

static void lower_break(LowerCtx *ctx, AstNode *s) {
    if (ctx->loop_depth == 0) {
        unsupported(s, "break outside loop");
        return;
    }
    emit_br(ctx, s->loc, ctx->loop_break[ctx->loop_depth - 1]);
}

static void lower_continue(LowerCtx *ctx, AstNode *s) {
    if (ctx->loop_depth == 0) {
        unsupported(s, "continue outside loop");
        return;
    }
    emit_br(ctx, s->loc, ctx->loop_continue[ctx->loop_depth - 1]);
}

static void lower_fail(LowerCtx *ctx, AstNode *s) {
    // `fail "msg"` in a Result-returning function → mix_result_err + ret.
    // Otherwise fall through to mix_panic.
    AstNode *v = s->fail_stmt.value;
    bool is_result_fn = ctx->fn_return_mix_type &&
                        ctx->fn_return_mix_type->kind == TYPE_RESULT;
    if (is_result_fn) {
        LirOpnd val;
        if (v && v->kind == NODE_STRING_LIT) {
            int sid = lir_intern_string(ctx->mod, v->string_lit.value, v->string_lit.length);
            val = lir_opnd_string(sid);
        } else if (v) {
            val = lower_expr(ctx, v);
        } else {
            int sid = lir_intern_string(ctx->mod, "fail", 4);
            val = lir_opnd_string(sid);
        }
        LirOpnd as_i64 = (val.type == LIR_TY_PTR)
            ? lir_opnd_value(lir_emit_conv(ctx->fn, s->loc, LIR_CONV_PTRTOINT,
                                              LIR_TY_PTR, LIR_TY_I64, val), LIR_TY_I64)
            : to_storage_i64(ctx, s->loc, val,
                                v ? v->resolved_type : NULL);
        LirType ps[] = { LIR_TY_I64 };
        register_runtime(ctx->mod, s->loc, "mix_result_err",
                         LIR_TY_PTR, ps, 1);
        LirOpnd args[] = { as_i64 };
        int r = lir_emit_call(ctx->fn, s->loc, "mix_result_err",
                                LIR_TY_PTR, args, 1);
        emit_ret_value(ctx, s->loc, lir_opnd_value(r, LIR_TY_PTR));
        return;
    }
    if (v && v->kind == NODE_STRING_LIT) {
        int sid = lir_intern_string(ctx->mod, v->string_lit.value, v->string_lit.length);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, s->loc, "mix_panic", LIR_TY_VOID, ps, 1);
        LirOpnd args[] = { lir_opnd_string(sid) };
        lir_emit_call(ctx->fn, s->loc, "mix_panic", LIR_TY_VOID, args, 1);
        // mix_panic doesn't return; emit a typed ret of the function's
        // return type so LLVM keeps the IR well-formed.
        LirType rt = ctx->fn->return_type;
        if (rt == LIR_TY_VOID) emit_ret_void(ctx, s->loc);
        else if (rt == LIR_TY_F64) emit_ret_value(ctx, s->loc, lir_opnd_f64(0.0));
        else if (rt == LIR_TY_PTR) emit_ret_value(ctx, s->loc, lir_opnd_int_typed(0, LIR_TY_PTR));
        else emit_ret_value(ctx, s->loc, lir_opnd_int_typed(0, rt));
    } else if (v) {
        // Best-effort: lower the value and pass it; if it's a ptr/string, panic with it.
        LirOpnd val = lower_expr(ctx, v);
        if (val.type == LIR_TY_PTR) {
            LirType ps[] = { LIR_TY_PTR };
            register_runtime(ctx->mod, s->loc, "mix_panic", LIR_TY_VOID, ps, 1);
            LirOpnd args[] = { val };
            lir_emit_call(ctx->fn, s->loc, "mix_panic", LIR_TY_VOID, args, 1);
            LirType rt = ctx->fn->return_type;
            if (rt == LIR_TY_VOID) emit_ret_void(ctx, s->loc);
            else if (rt == LIR_TY_F64) emit_ret_value(ctx, s->loc, lir_opnd_f64(0.0));
            else if (rt == LIR_TY_PTR) emit_ret_value(ctx, s->loc, lir_opnd_int_typed(0, LIR_TY_PTR));
            else emit_ret_value(ctx, s->loc, lir_opnd_int_typed(0, rt));
        } else {
            unsupported(s, "fail with non-string value");
        }
    } else {
        int sid = lir_intern_string(ctx->mod, "fail", 4);
        LirType ps[] = { LIR_TY_PTR };
        register_runtime(ctx->mod, s->loc, "mix_panic", LIR_TY_VOID, ps, 1);
        LirOpnd args[] = { lir_opnd_string(sid) };
        lir_emit_call(ctx->fn, s->loc, "mix_panic", LIR_TY_VOID, args, 1);
        emit_ret_void(ctx, s->loc);
    }
}

static void lower_defer(LowerCtx *ctx, AstNode *s) {
    if (ctx->defer_count >= 64) {
        unsupported(s, "too many defers in one function");
        return;
    }
    ctx->deferred[ctx->defer_count++] = s->defer_stmt.stmt;
}

// Lower a match statement. If `result_slot` >= 0, each non-statement arm
// body has its expression value stored to that slot (used for
// match-as-implicit-return).
static void lower_match(LowerCtx *ctx, AstNode *m, int result_slot,
                          LirType result_type)
{
    LirOpnd subj = lower_expr(ctx, m->match_stmt.subject);
    int merge_l = lir_func_new_label(ctx->fn);

    MixType *subj_mix = m->match_stmt.subject ? m->match_stmt.subject->resolved_type : NULL;
    bool subj_is_optional = subj_mix && subj_mix->kind == TYPE_OPTIONAL;
    bool subj_is_result   = subj_mix && subj_mix->kind == TYPE_RESULT;
    bool subj_is_tagged   = subj_mix && subj_mix->kind == TYPE_SHAPE &&
                            subj_mix->shape.is_tagged_union;

    // Pre-load the tag once when matching on a tagged union; arms below
    // compare against this tag value.
    int tag_value = -1;
    if (subj_is_tagged && subj.type == LIR_TY_PTR) {
        tag_value = lir_emit_load(ctx->fn, m->loc, LIR_TY_I64, subj);
    }

    for (int i = 0; i < m->match_stmt.arm_count; i++) {
        struct MatchArm *arm = &m->match_stmt.arms[i];
        int next_l = lir_func_new_label(ctx->fn);
        int body_l = lir_func_new_label(ctx->fn);

        // Tagged-union arm: `Circle(r) =>`, `Rect(w, h) =>`. Pattern is a
        // NODE_CALL_EXPR with name = variant; args bind variant fields.
        // We allocate a fresh local for each binding (popped after the
        // arm body so subsequent arms don't see the binding).
        int saved_local_count = ctx->local_count;
        bool handled_tagged = false;
        if (!arm->is_wildcard && subj_is_tagged && arm->pattern &&
            arm->pattern->kind == NODE_CALL_EXPR && arm->pattern->call.name) {
            ShapeVariant *sv = type_find_variant(subj_mix, arm->pattern->call.name);
            if (sv) {
                int cmp_v = lir_emit_bin(ctx->fn, m->loc, LIR_BIN_EQ, LIR_TY_I64,
                                           lir_opnd_value(tag_value, LIR_TY_I64),
                                           lir_opnd_int_typed(sv->tag, LIR_TY_I64));
                emit_br_cond(ctx, m->loc, lir_opnd_value(cmp_v, LIR_TY_I1),
                              body_l, next_l);
                emit_label(ctx, m->loc, body_l);
                // Bind each variant field arg to a fresh local.
                int n = arm->pattern->call.arg_count;
                for (int k = 0; k < n && k < sv->field_count; k++) {
                    AstNode *binding = arm->pattern->call.args[k];
                    if (!binding || binding->kind != NODE_IDENT) continue;
                    LirType ft = mix_to_lir(sv->fields[k].type);
                    if (ft == LIR_TY_VOID) ft = LIR_TY_I64;
                    int faddr = lir_emit_ptr_offset(ctx->fn, m->loc, subj,
                                                      8 + sv->fields[k].offset);
                    int fval = lir_emit_load(ctx->fn, m->loc, ft,
                                              lir_opnd_value(faddr, LIR_TY_PTR));
                    int slot = lir_emit_alloca(ctx->fn, m->loc, ft);
                    lir_emit_store(ctx->fn, m->loc, ft,
                                    lir_opnd_value(fval, ft),
                                    lir_opnd_value(slot, LIR_TY_PTR));
                    Local *bl = scope_add(ctx, binding->ident.name);
                    bl->value_id = slot;
                    bl->scalar_type = ft;
                }
                handled_tagged = true;
            }
        }

        // Variant-binding patterns over Optional/Result: `some(v)`,
        // `none`, `ok(v)`, `err(e)`. The pattern may be NODE_CALL_EXPR
        // (with a name like "some"/"ok") or NODE_NONE_LIT / NODE_IDENT
        // ("none"/"err" with no payload).
        Local *bound_local = NULL;
        bool handled_variant = handled_tagged;
        if (!handled_variant && !arm->is_wildcard && (subj_is_optional || subj_is_result)) {
            const char *vname = NULL;
            const char *bind  = NULL;
            AstNode *pat = arm->pattern;
            if (pat) {
                if (pat->kind == NODE_CALL_EXPR && pat->call.name &&
                    pat->call.arg_count == 1 &&
                    pat->call.args[0] &&
                    pat->call.args[0]->kind == NODE_IDENT) {
                    vname = pat->call.name;
                    bind  = pat->call.args[0]->ident.name;
                } else if (pat->kind == NODE_IDENT) {
                    vname = pat->ident.name;
                } else if (pat->kind == NODE_NONE_LIT) {
                    vname = "none";
                }
            }
            if (vname) {
                const char *has_name = subj_is_result ? "mix_result_is_ok" : "mix_optional_has";
                LirType ps[] = { LIR_TY_PTR };
                register_runtime(ctx->mod, m->loc, has_name, LIR_TY_I64, ps, 1);
                LirOpnd has_args[] = { subj };
                int hv = lir_emit_call(ctx->fn, m->loc, has_name, LIR_TY_I64, has_args, 1);
                int hb = lir_emit_bin(ctx->fn, m->loc, LIR_BIN_NE, LIR_TY_I64,
                                       lir_opnd_value(hv, LIR_TY_I64),
                                       lir_opnd_int_typed(0, LIR_TY_I64));
                bool want_ok = (strcmp(vname, "some") == 0 || strcmp(vname, "ok") == 0);
                int cond_v = hb;
                if (!want_ok) {
                    int n = lir_emit_un(ctx->fn, m->loc, LIR_UN_NOT, LIR_TY_I1,
                                         lir_opnd_value(hb, LIR_TY_I1));
                    cond_v = n;
                }
                emit_br_cond(ctx, m->loc, lir_opnd_value(cond_v, LIR_TY_I1),
                              body_l, next_l);
                emit_label(ctx, m->loc, body_l);
                if (bind) {
                    const char *get_name = subj_is_result ? "mix_result_unwrap" : "mix_optional_get";
                    if (!want_ok && subj_is_result) get_name = "mix_result_unwrap_err";
                    register_runtime(ctx->mod, m->loc, get_name, LIR_TY_I64, ps, 1);
                    int got = lir_emit_call(ctx->fn, m->loc, get_name,
                                              LIR_TY_I64, has_args, 1);
                    int slot = lir_emit_alloca(ctx->fn, m->loc, LIR_TY_I64);
                    lir_emit_store(ctx->fn, m->loc, LIR_TY_I64,
                                    lir_opnd_value(got, LIR_TY_I64),
                                    lir_opnd_value(slot, LIR_TY_PTR));
                    bound_local = scope_add(ctx, bind);
                    bound_local->value_id = slot;
                    bound_local->scalar_type = LIR_TY_I64;
                }
                handled_variant = true;
            }
        }

        if (!handled_variant && !arm->is_wildcard) {
            LirOpnd pat = lower_expr(ctx, arm->pattern);
            // Coerce types to match for the comparison.
            LirType ot = subj.type;
            LirOpnd a = subj, b = pat;
            if (ot != b.type && is_int_lir(ot) && is_int_lir(b.type)) {
                a = to_i64(ctx, m->loc, a);
                b = to_i64(ctx, m->loc, b);
                ot = LIR_TY_I64;
            }
            int cmp_v = lir_emit_bin(ctx->fn, m->loc, LIR_BIN_EQ, ot, a, b);
            emit_br_cond(ctx, m->loc, lir_opnd_value(cmp_v, LIR_TY_I1),
                          body_l, next_l);
            emit_label(ctx, m->loc, body_l);
        }

        // Lower arm body. If it's a value-producing expression and we have
        // a result slot, store the value; else lower as a statement.
        AstNode *body = arm->body;
        bool is_value_expr = body && (body->kind == NODE_INT_LIT ||
            body->kind == NODE_FLOAT_LIT || body->kind == NODE_BOOL_LIT ||
            body->kind == NODE_STRING_LIT || body->kind == NODE_STRING_INTERP ||
            body->kind == NODE_IDENT || body->kind == NODE_BINARY_EXPR ||
            body->kind == NODE_UNARY_EXPR || body->kind == NODE_CALL_EXPR ||
            body->kind == NODE_FIELD_EXPR || body->kind == NODE_LIST_LIT ||
            body->kind == NODE_INDEX_EXPR || body->kind == NODE_SHAPE_LIT);

        if (result_slot >= 0 && is_value_expr) {
            LirOpnd v = lower_expr(ctx, body);
            if (v.type != result_type && is_int_lir(v.type) && is_int_lir(result_type)) {
                int r = lir_emit_conv(ctx->fn, body->loc,
                                       result_type == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                       v.type, result_type, v);
                v = lir_opnd_value(r, result_type);
            }
            lir_emit_store(ctx->fn, body->loc, result_type, v,
                            lir_opnd_value(result_slot, LIR_TY_PTR));
        } else if (body) {
            lower_stmt(ctx, body);
        }
        // Pop any per-arm bound locals so the next arm doesn't see them.
        ctx->local_count = saved_local_count;
        (void)bound_local; // covered by saved_local_count restore
        emit_br(ctx, m->loc, merge_l);

        if (!arm->is_wildcard) {
            emit_label(ctx, m->loc, next_l);
            // If this is the last arm, the next_l block is empty and would
            // immediately precede merge_l. Emit a fall-through branch so
            // LLVM has a valid terminator on the empty block.
            if (i == m->match_stmt.arm_count - 1) {
                emit_br(ctx, m->loc, merge_l);
            }
        }
    }
    emit_label(ctx, m->loc, merge_l);
}

static void lower_stmt(LowerCtx *ctx, AstNode *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case NODE_VAR_DECL:       lower_var_decl    (ctx, stmt); break;
        case NODE_ASSIGN:         lower_assign      (ctx, stmt); break;
        case NODE_FIELD_ASSIGN:   lower_field_assign(ctx, stmt); break;
        case NODE_INDEX_ASSIGN:   lower_index_assign(ctx, stmt); break;
        case NODE_DEREF_ASSIGN: {
            // *ptr = val
            LirOpnd p = lower_expr(ctx, stmt->deref_assign.ptr_expr);
            LirOpnd v = lower_expr(ctx, stmt->deref_assign.value);
            LirType t = v.type;
            if (t == LIR_TY_VOID) t = LIR_TY_I64;
            lir_emit_store(ctx->fn, stmt->loc, t, v, p);
            break;
        }
        case NODE_IF_STMT:        lower_if          (ctx, stmt); break;
        case NODE_WHILE_STMT:     lower_while       (ctx, stmt); break;
        case NODE_FOR_STMT:       lower_for_range   (ctx, stmt); break;
        case NODE_DONE_STMT:      lower_done        (ctx, stmt); break;
        case NODE_BREAK_STMT:     lower_break       (ctx, stmt); break;
        case NODE_CONTINUE_STMT:  lower_continue    (ctx, stmt); break;
        case NODE_FAIL_STMT:      lower_fail        (ctx, stmt); break;
        case NODE_DEFER_STMT:     lower_defer       (ctx, stmt); break;
        case NODE_UNSAFE_BLOCK:   lower_block_stmts (ctx, stmt->unsafe_block.body); break;
        case NODE_ZONE_STMT:      lower_block_stmts (ctx, stmt->zone_stmt.body); break;
        case NODE_MATCH_STMT:     lower_match       (ctx, stmt, -1, LIR_TY_VOID); break;
        case NODE_BLOCK:          lower_block_stmts (ctx, stmt); break;
        case NODE_EXPR_STMT:      (void)lower_expr  (ctx, stmt->expr_stmt.expr); break;
        case NODE_CALL_EXPR:      (void)lower_expr  (ctx, stmt); break;
        case NODE_SHAPE_DECL:     /* layout in sema */ break;
        default:
            unsupported(stmt, "statement kind");
            break;
    }
}

// ---- Lambda lowering -------------------------------------------------------
// A lambda `x => x * 2` is hoisted to a fresh top-level function named
// `mix_lambda_<n>` and the expression evaluates to the function's address
// (returned as LIR_OPND_FN_REF). All lambda params are i64; callers
// coerce as needed at the call site (sema infers TYPE_INFER for params,
// so we have nothing more specific to commit to). The body is lowered in
// a fresh LowerCtx so it doesn't see the enclosing function's locals —
// we don't capture variables yet.
static LirOpnd lower_lambda(LowerCtx *ctx, AstNode *lam) {
    if (!lam || lam->kind != NODE_LAMBDA) return lir_opnd_none();

    char namebuf[64];
    int n = g_lambda_counter++;
    snprintf(namebuf, sizeof(namebuf), "mix_lambda_%d", n);
    const char *fn_name = arena_strdup(ctx->mod->arena, namebuf);
    lam->lambda.generated_name = (char *)fn_name;

    // Lambdas use a uniform i64 ABI (params and return) so call sites and
    // indirect-call coercion don't have to track per-lambda signatures.
    // The lambda's untyped sema return (TYPE_INFER → PTR) would otherwise
    // produce mismatched IR.
    LirType ret_ty = LIR_TY_I64;

    LirFunc *lfn = lir_module_add_func(ctx->mod, fn_name, false, ret_ty);
    for (int i = 0; i < lam->lambda.param_count; i++) {
        lir_func_add_param(lfn, lam->lambda.param_names[i], LIR_TY_I64, false);
    }

    // Lower the body inside a fresh LowerCtx so the lambda doesn't see
    // the enclosing function's locals/loops/defers (we don't support
    // closures yet).
    LowerCtx lctx = {0};
    lctx.mod = ctx->mod;
    lctx.fn = lfn;
    lctx.symtab = ctx->symtab;
    lctx.sret_value_id = -1;

    // Spill each i64 param into an alloca slot so reads use the standard
    // load/store pattern.
    for (int i = 0; i < lam->lambda.param_count; i++) {
        int slot = lir_emit_alloca(lfn, lam->loc, LIR_TY_I64);
        lir_emit_store(lfn, lam->loc, LIR_TY_I64,
                        lir_opnd_param(i, LIR_TY_I64),
                        lir_opnd_value(slot, LIR_TY_PTR));
        Local *l = scope_add(&lctx, lam->lambda.param_names[i]);
        l->value_id = slot;
        l->scalar_type = LIR_TY_I64;
    }

    // Lower the body and return its value, coerced to the declared ret_ty.
    LirOpnd v = lower_expr(&lctx, lam->lambda.body);
    if (v.type != ret_ty) {
        if (is_int_lir(v.type) && is_int_lir(ret_ty)) {
            int sw = (v.type==LIR_TY_I1)?1:(v.type==LIR_TY_I8)?8:(v.type==LIR_TY_I32)?32:64;
            int dw = (ret_ty==LIR_TY_I1)?1:(ret_ty==LIR_TY_I8)?8:(ret_ty==LIR_TY_I32)?32:64;
            LirConvKind k = (dw < sw) ? LIR_CONV_TRUNC : LIR_CONV_SEXT;
            int r = lir_emit_conv(lfn, lam->loc, k, v.type, ret_ty, v);
            v = lir_opnd_value(r, ret_ty);
        } else if (v.type == LIR_TY_PTR && is_int_lir(ret_ty)) {
            int r = lir_emit_conv(lfn, lam->loc, LIR_CONV_PTRTOINT,
                                    LIR_TY_PTR, ret_ty, v);
            v = lir_opnd_value(r, ret_ty);
        } else if (is_int_lir(v.type) && ret_ty == LIR_TY_PTR) {
            int r = lir_emit_conv(lfn, lam->loc, LIR_CONV_INTTOPTR,
                                    v.type, LIR_TY_PTR, v);
            v = lir_opnd_value(r, LIR_TY_PTR);
        }
    }
    if (!lctx.block_terminated) {
        lir_emit_ret_value(lfn, lam->loc, v);
        lctx.block_terminated = true;
    }

    return lir_opnd_fn_ref(fn_name);
}

// ---- Function lowering -----------------------------------------------------

// When lowering a method (`shape Foo { method bar() }`), this struct tells
// lower_function_inner about the synthesized `self` first param. NULL for
// regular top-level functions.
typedef struct {
    const char *mangled_name;   // ShapeName_methodName
    MixType    *self_shape;
    bool        self_is_mutable;
} MethodCtx;

static void lower_function_inner(LirModule *mod, SymTab *symtab,
                                  AstNode *fn_decl, MethodCtx *mctx);

static void lower_function(LirModule *mod, SymTab *symtab, AstNode *fn_decl) {
    lower_function_inner(mod, symtab, fn_decl, NULL);
}

static void lower_method(LirModule *mod, SymTab *symtab,
                          AstNode *shape_decl, AstNode *method)
{
    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s_%s",
             shape_decl->shape_decl.name, method->fn_decl.name);
    Symbol *sym = symtab_lookup(symtab, shape_decl->shape_decl.name);
    if (!sym || !sym->type || sym->type->kind != TYPE_SHAPE) return;
    MethodCtx mc = {0};
    mc.mangled_name = arena_strdup(mod->arena, mangled);
    mc.self_shape = sym->type;
    mc.self_is_mutable = method->fn_decl.has_mutation;
    lower_function_inner(mod, symtab, method, &mc);
}

static void lower_function_inner(LirModule *mod, SymTab *symtab,
                                  AstNode *fn_decl, MethodCtx *mctx)
{
    bool is_main = (!mctx) && (fn_decl->fn_decl.name &&
                    strcmp(fn_decl->fn_decl.name, "main") == 0);

    // Determine return type. Sema wraps the return in TYPE_RESULT when a
    // `~` function with `fail` exists in the body — use the symtab type
    // when present so the function signature matches the call sites.
    MixType *ret_mix = NULL;
    if (fn_decl->fn_decl.return_type)
        ret_mix = fn_decl->fn_decl.return_type->resolved_type;
    if (!mctx && fn_decl->fn_decl.name) {
        Symbol *sym = symtab_lookup(symtab, fn_decl->fn_decl.name);
        if (sym && sym->type && sym->type->kind == TYPE_FUNC) {
            MixType *symret = sym->type->func.return_type;
            if (symret && symret->kind == TYPE_RESULT) ret_mix = symret;
        }
    }

    bool returns_shape = mix_is_shape(ret_mix);

    LirType ret_ty;
    if (is_main)             ret_ty = LIR_TY_I32;
    else if (returns_shape)  ret_ty = LIR_TY_VOID;     // sret transformation
    else                     ret_ty = mix_to_lir(ret_mix);

    const char *fn_name = mctx ? mctx->mangled_name : fn_decl->fn_decl.name;
    LirFunc *fn = lir_module_add_func(mod, fn_name, is_main, ret_ty);

    // Hidden sret param is added FIRST when the function returns a shape.
    if (returns_shape) {
        lir_func_add_param(fn, "_sret", LIR_TY_PTR, false);
    }

    // Methods get a synthesized `self` param (always passed as ptr — same
    // ABI as a shape param).
    if (mctx) {
        lir_func_add_param(fn, "self", LIR_TY_PTR, mctx->self_is_mutable);
    }

    for (int i = 0; i < fn_decl->fn_decl.param_count; i++) {
        Param *p = &fn_decl->fn_decl.params[i];
        MixType *pt = (p->type && p->type->resolved_type) ? p->type->resolved_type : NULL;
        LirType lt = mix_to_lir(pt);
        if (lt == LIR_TY_VOID && pt) {
            unsupported(fn_decl, "parameter with unsupported type");
            return;
        }
        // Mutable params are passed as pointers to the caller's storage so
        // mutations propagate. Shape params are already ptr-passed in this
        // ABI; only scalar params change LIR type.
        bool pass_as_ptr = p->is_mutable && !mix_is_shape(pt);
        lir_func_add_param(fn, p->name, pass_as_ptr ? LIR_TY_PTR : lt, p->is_mutable);
    }

    if (is_main) register_set_args(mod);

    LowerCtx ctx = {0};
    ctx.mod = mod;
    ctx.fn = fn;
    ctx.symtab = symtab;
    ctx.sret_value_id = -1;
    ctx.fn_return_mix_type = ret_mix;
    if (mctx) ctx.current_shape = mctx->self_shape;

    int param_idx = 0;
    if (returns_shape) {
        // Bind the sret pointer for `done`/implicit-return paths.
        ctx.sret_value_id = lir_func_new_value(fn);
        // Use the param ptr directly. We synthesize a no-op alloca-less
        // local that resolves to the param.
        // We'll just remember the param index and synthesize an opnd in
        // lower_done; simpler is to alloca a slot for the ptr and store.
        // For Phase 4A the simplest move: keep sret as param 0, address it
        // via lir_opnd_param(0, LIR_TY_PTR).
        // But ctx.sret_value_id assumes an SSA value id. Let's spill to a
        // local SSA value with a no-op load: actually simpler — we can
        // skip the value_id machinery and just remember "use param 0".
        // To keep lower_done simple we'll spill the sret param into a
        // local alloca, store the param ptr, and load it on demand. That
        // costs one extra pair at -O0; mem2reg removes it.
        int slot = lir_emit_alloca(fn, fn_decl->loc, LIR_TY_PTR);
        lir_emit_store(fn, fn_decl->loc, LIR_TY_PTR,
                        lir_opnd_param(param_idx, LIR_TY_PTR),
                        lir_opnd_value(slot, LIR_TY_PTR));
        ctx.sret_value_id = lir_emit_load(fn, fn_decl->loc, LIR_TY_PTR,
                                            lir_opnd_value(slot, LIR_TY_PTR));
        ctx.sret_shape_type = ret_mix;
        param_idx++;
    }

    // Method: spill `self` (a ptr to the shape) and bind it as a shape Local.
    if (mctx) {
        int slot = lir_emit_alloca(fn, fn_decl->loc, LIR_TY_PTR);
        lir_emit_store(fn, fn_decl->loc, LIR_TY_PTR,
                        lir_opnd_param(param_idx, LIR_TY_PTR),
                        lir_opnd_value(slot, LIR_TY_PTR));
        int v = lir_emit_load(fn, fn_decl->loc, LIR_TY_PTR,
                                lir_opnd_value(slot, LIR_TY_PTR));
        Local *l = scope_add(&ctx, "self");
        l->value_id = v;
        l->shape_type = mctx->self_shape;
        param_idx++;
    }

    // Bind each MIX param to a Local. Scalar params spill into an alloca
    // for the load/store-from-ptr pattern. Shape params and mutable
    // scalar params bind to the caller's pointer directly (no spill of
    // the value, just a one-time alloca+store+load to get a stable SSA
    // id for the ptr — mem2reg removes it at -O1+).
    for (int i = 0; i < fn_decl->fn_decl.param_count; i++) {
        Param *p = &fn_decl->fn_decl.params[i];
        MixType *pt = (p->type && p->type->resolved_type) ? p->type->resolved_type : NULL;
        LirType param_lir_ty = fn->param_types[param_idx];

        bool is_shape_param = mix_is_shape(pt);
        bool is_mut_scalar  = p->is_mutable && !is_shape_param;

        if (is_shape_param || is_mut_scalar) {
            // Spill the param's PTR value to a stable SSA id.
            int slot = lir_emit_alloca(fn, fn_decl->loc, LIR_TY_PTR);
            lir_emit_store(fn, fn_decl->loc, LIR_TY_PTR,
                            lir_opnd_param(param_idx, LIR_TY_PTR),
                            lir_opnd_value(slot, LIR_TY_PTR));
            int v = lir_emit_load(fn, fn_decl->loc, LIR_TY_PTR,
                                    lir_opnd_value(slot, LIR_TY_PTR));
            Local *l = scope_add(&ctx, p->name);
            l->value_id = v;
            if (is_shape_param) {
                l->shape_type = pt;
            } else {
                // Mut scalar: scope sees this Local as a regular scalar
                // local backed by the caller's storage. Reads = LOAD,
                // writes = STORE.
                l->scalar_type = mix_to_lir(pt);
            }
        } else {
            // Immutable scalar: spill value into alloca; treat as local.
            int slot = lir_emit_alloca(fn, fn_decl->loc, param_lir_ty);
            lir_emit_store(fn, fn_decl->loc, param_lir_ty,
                            lir_opnd_param(param_idx, param_lir_ty),
                            lir_opnd_value(slot, LIR_TY_PTR));
            Local *l = scope_add(&ctx, p->name);
            l->value_id = slot;
            l->scalar_type = param_lir_ty;
        }
        param_idx++;
    }

    AstNode *body = fn_decl->fn_decl.body;

    // Implicit final-expression return for non-main / non-void functions.
    int last_idx = -1;
    bool need_implicit_return = !is_main &&
        (ret_ty != LIR_TY_VOID || returns_shape);
    if (need_implicit_return && body && body->kind == NODE_BLOCK) {
        for (int i = body->block.stmt_count - 1; i >= 0; i--) {
            AstNode *s = body->block.stmts[i];
            if (s && (s->kind == NODE_EXPR_STMT || s->kind == NODE_CALL_EXPR ||
                      s->kind == NODE_BINARY_EXPR || s->kind == NODE_INT_LIT ||
                      s->kind == NODE_FLOAT_LIT || s->kind == NODE_BOOL_LIT ||
                      s->kind == NODE_IDENT || s->kind == NODE_FIELD_EXPR ||
                      s->kind == NODE_SHAPE_LIT || s->kind == NODE_MATCH_STMT ||
                      s->kind == NODE_INDEX_EXPR || s->kind == NODE_LIST_LIT ||
                      s->kind == NODE_MAP_LIT || s->kind == NODE_SET_LIT ||
                      s->kind == NODE_NONE_LIT || s->kind == NODE_ELSE_EXPR ||
                      s->kind == NODE_STRING_LIT || s->kind == NODE_STRING_INTERP ||
                      s->kind == NODE_UNARY_EXPR || s->kind == NODE_CAST_EXPR ||
                      s->kind == NODE_LIST_COMP || s->kind == NODE_METHOD_CALL)) {
                last_idx = i;
                break;
            }
        }
    }

    if (body && body->kind == NODE_BLOCK) {
        for (int i = 0; i < body->block.stmt_count; i++) {
            if (i == last_idx) {
                AstNode *s = body->block.stmts[i];
                AstNode *expr = (s->kind == NODE_EXPR_STMT) ? s->expr_stmt.expr : s;
                if (returns_shape) {
                    LirOpnd dst = lir_opnd_value(ctx.sret_value_id, LIR_TY_PTR);
                    lower_init_into(&ctx, dst, ret_mix, expr);
                    emit_ret_void(&ctx, s->loc);
                } else if (expr && expr->kind == NODE_MATCH_STMT) {
                    // Match-as-implicit-return: allocate a result slot,
                    // each arm body stores its value, then return slot.
                    int slot = lir_emit_alloca(fn, s->loc, ret_ty);
                    lower_match(&ctx, expr, slot, ret_ty);
                    int v = lir_emit_load(fn, s->loc, ret_ty,
                                            lir_opnd_value(slot, LIR_TY_PTR));
                    emit_ret_value(&ctx, s->loc, lir_opnd_value(v, ret_ty));
                } else {
                    LirOpnd v = lower_expr(&ctx, expr);
                    // Implicit return value in an optional/result fn:
                    // wrap with mix_optional_some / mix_result_ok unless
                    // the value is already a wrapped ptr (e.g. another
                    // matching call or `none`).
                    bool wrap_opt = ret_mix && ret_mix->kind == TYPE_OPTIONAL &&
                                     v.type != LIR_TY_PTR;
                    bool wrap_res = ret_mix && ret_mix->kind == TYPE_RESULT;
                    if (wrap_opt) {
                        LirOpnd as_i64 = to_storage_i64(&ctx, s->loc, v,
                                                          expr ? expr->resolved_type : NULL);
                        LirType ps[] = { LIR_TY_I64 };
                        register_runtime(mod, s->loc, "mix_optional_some",
                                          LIR_TY_PTR, ps, 1);
                        LirOpnd args[] = { as_i64 };
                        int r = lir_emit_call(fn, s->loc, "mix_optional_some",
                                                LIR_TY_PTR, args, 1);
                        v = lir_opnd_value(r, LIR_TY_PTR);
                    } else if (wrap_res && v.type != LIR_TY_PTR) {
                        LirOpnd as_i64 = to_storage_i64(&ctx, s->loc, v,
                                                          expr ? expr->resolved_type : NULL);
                        LirType ps[] = { LIR_TY_I64 };
                        register_runtime(mod, s->loc, "mix_result_ok",
                                          LIR_TY_PTR, ps, 1);
                        LirOpnd args[] = { as_i64 };
                        int r = lir_emit_call(fn, s->loc, "mix_result_ok",
                                                LIR_TY_PTR, args, 1);
                        v = lir_opnd_value(r, LIR_TY_PTR);
                    } else if (v.type != ret_ty) {
                        if (is_int_lir(v.type) && is_int_lir(ret_ty)) {
                            int r = lir_emit_conv(fn, s->loc,
                                                   ret_ty == LIR_TY_I64 ? LIR_CONV_SEXT : LIR_CONV_TRUNC,
                                                   v.type, ret_ty, v);
                            v = lir_opnd_value(r, ret_ty);
                        } else if (v.type == LIR_TY_PTR && is_int_lir(ret_ty)) {
                            int r = lir_emit_conv(fn, s->loc, LIR_CONV_PTRTOINT,
                                                    LIR_TY_PTR, ret_ty, v);
                            v = lir_opnd_value(r, ret_ty);
                        } else if (is_int_lir(v.type) && ret_ty == LIR_TY_PTR) {
                            int r = lir_emit_conv(fn, s->loc, LIR_CONV_INTTOPTR,
                                                    v.type, LIR_TY_PTR, v);
                            v = lir_opnd_value(r, LIR_TY_PTR);
                        }
                    }
                    emit_ret_value(&ctx, s->loc, v);
                }
            } else {
                lower_stmt(&ctx, body->block.stmts[i]);
            }
            if (mix_error_count() > 0) return;
        }
    } else if (body) {
        lower_stmt(&ctx, body);
    }

    // Synthesize a trailing return for any path that didn't terminate.
    if (!ctx.block_terminated) {
        if (ret_ty == LIR_TY_VOID) {
            emit_ret_void(&ctx, fn_decl->loc);
        } else if (ret_ty == LIR_TY_F64) {
            emit_ret_value(&ctx, fn_decl->loc, lir_opnd_f64(0.0));
        } else {
            emit_ret_value(&ctx, fn_decl->loc, lir_opnd_int_typed(0, ret_ty));
        }
    }
}

LirModule *lower_program(AstNode *program, Arena *arena, SymTab *symtab) {
    if (!program || program->kind != NODE_PROGRAM) return NULL;
    LirModule *mod = lir_module_new(arena);

    // Phase 5: collect all top-level @const decls before lowering any
    // function bodies, so identifier references can resolve to const
    // values regardless of decl order.
    g_const_count = 0;
    g_lambda_counter = 0;
    g_global_count = 0;
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_CONST_DECL) {
            g_consts_push(decl->const_decl.name, decl->const_decl.value);
        }
        // Cond-compiled consts: walk active branch.
        if (decl->kind == NODE_COND_DECL && decl->cond_decl.active) {
            for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                AstNode *cd = decl->cond_decl.decls[j];
                if (cd->kind == NODE_CONST_DECL) {
                    g_consts_push(cd->const_decl.name, cd->const_decl.value);
                }
            }
        }
        if (decl->kind == NODE_VAR_DECL && g_global_count < 256) {
            // Top-level mutable global (`pub running! = true`).
            MixType *t = decl->resolved_type;
            LirType lt = mix_to_lir(t);
            if (lt == LIR_TY_VOID) lt = LIR_TY_I64;
            GlobalEntry *e = &g_globals[g_global_count++];
            e->name = decl->var_decl.name;
            e->mix_type = t;
            e->lir_type = lt;
            e->init_expr = decl->var_decl.init_expr;
            e->is_pub = decl->var_decl.is_pub;

            // Constant-fold the initializer if possible so the global gets
            // a proper static init value (avoids needing a constructor).
            bool has_const = false;
            long long const_init = 0;
            AstNode *iv = decl->var_decl.init_expr;
            if (iv) {
                if (iv->kind == NODE_INT_LIT) {
                    has_const = true;
                    const_init = iv->int_lit.value;
                } else if (iv->kind == NODE_BOOL_LIT) {
                    has_const = true;
                    const_init = iv->bool_lit.value ? 1 : 0;
                }
            }
            lir_module_add_global(mod, e->name, lt, has_const, const_init, false);
        }
    }
    // Cross-module imports: scan for symbols declared in OTHER modules
    // that resolve to globals. Sema flagged those by inserting them into
    // the symbol table; we hoist a `declare external global` for any
    // pub global the program references that isn't defined locally. A
    // simple-but-loose approximation: declare any non-fn TYPE_INT /
    // TYPE_BOOL / TYPE_PTR symbol that the program uses but isn't local.
    // For now, detect cross-module use by walking every NODE_USE_DECL's
    // imported module and asking sema for its pub globals — sema doesn't
    // expose that, so as a conservative fix we declare every reference
    // we encounter on demand (handled in the IDENT lookup below).

    // Phase 5: pre-register every top-level fn decl as a callee using the
    // sema-resolved signature. This nails down the param types before any
    // call site lowers, so when call-site arg types disagree (e.g.
    // `apply(dbl, 21)` passes a fn-ptr where the param is declared `int`)
    // the call-site coercion path triggers instead of registering a
    // wrong signature.
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind != NODE_FN_DECL) continue;
        const char *name = decl->fn_decl.name;
        if (!name) continue;
        Symbol *sym = symtab_lookup(symtab, name);
        if (!sym || !sym->type || sym->type->kind != TYPE_FUNC) continue;
        MixType *ft = sym->type;
        int n = ft->func.param_count;
        LirType *params = NULL;
        if (n > 0) params = arena_alloc(arena, n * sizeof(LirType));
        for (int k = 0; k < n; k++) {
            MixType *pt = ft->func.param_types[k];
            bool pmut = ft->func.param_mutable && ft->func.param_mutable[k];
            // Mutable scalar params are passed as ptr-to-storage; shape
            // params are already ptr; everything else maps directly.
            if (pmut && pt && pt->kind != TYPE_SHAPE) params[k] = LIR_TY_PTR;
            else params[k] = mix_to_lir(pt);
        }
        MixType *rt = ft->func.return_type;
        bool returns_shape = rt && rt->kind == TYPE_SHAPE;
        LirType ret = returns_shape ? LIR_TY_VOID : mix_to_lir(rt);
        if (returns_shape) {
            // Insert hidden sret param at index 0.
            LirType *p2 = arena_alloc(arena, (n + 1) * sizeof(LirType));
            p2[0] = LIR_TY_PTR;
            for (int k = 0; k < n; k++) p2[k+1] = params[k];
            params = p2;
            n++;
        }
        lir_register_callee(mod, decl->loc, name, ret, params, n);
    }

    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        switch (decl->kind) {
            case NODE_FN_DECL:
                lower_function(mod, symtab, decl);
                break;
            case NODE_SHAPE_DECL:
                // Layout already computed in sema; emit each method as a
                // top-level function `ShapeName_methodName(self, ...)`.
                for (int j = 0; j < decl->shape_decl.method_count; j++) {
                    lower_method(mod, symtab, decl, decl->shape_decl.methods[j]);
                    if (mix_error_count() > 0) return mod;
                }
                break;
            case NODE_CONST_DECL:
                // Already collected above; no LIR emission needed.
                break;
            case NODE_EXTERN_BLOCK: {
                for (int j = 0; j < decl->extern_block.decl_count; j++) {
                    AstNode *e = decl->extern_block.decls[j];
                    if (e->kind != NODE_EXTERN_FN_DECL) continue;
                    Symbol *sym = symtab_lookup(symtab, e->extern_fn_decl.name);
                    if (!sym || !sym->type || sym->type->kind != TYPE_FUNC) {
                        unsupported(e, "extern fn signature not in symbol table");
                        continue;
                    }
                    MixType *ft = sym->type;
                    int n = ft->func.param_count;
                    LirType *params = NULL;
                    if (n > 0) {
                        params = arena_alloc(arena, n * sizeof(LirType));
                        for (int k = 0; k < n; k++) {
                            // Small int-only structs pass in an integer
                            // register per the C ABI, not as a pointer.
                            LirType ival = shape_int_value_lir(ft->func.param_types[k]);
                            if (ival != LIR_TY_VOID) params[k] = ival;
                            else params[k] = mix_to_lir(ft->func.param_types[k]);
                        }
                    }
                    LirType ret = mix_to_lir(ft->func.return_type);
                    const char *symbol = e->extern_fn_decl.c_name
                        ? e->extern_fn_decl.c_name
                        : e->extern_fn_decl.name;
                    lir_register_callee(mod, e->loc, symbol, ret, params, n);
                }
                break;
            }
            case NODE_USE_DECL:
            case NODE_USE_C_DECL:
                // Modules and `use c` headers are handled at the
                // compile_module() level in main.c — sub-modules are
                // already compiled into separate .o files and their pub
                // symbols are in the symbol table. Lowering treats them
                // as no-ops here.
                break;
            case NODE_TYPE_ALIAS:
                // Type aliases are pure sema constructs.
                break;
            case NODE_VAR_DECL:
                // Top-level globals are emitted up-front (g_globals);
                // here they're a no-op. Initializer code for non-const
                // values is run from main() — TODO if we hit a test that
                // needs it.
                break;
            case NODE_COND_DECL:
                // Conditional compilation: emit the active branch's decls.
                if (decl->cond_decl.active) {
                    for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                        AstNode *cd = decl->cond_decl.decls[j];
                        if (cd->kind == NODE_FN_DECL) {
                            lower_function(mod, symtab, cd);
                        } else if (cd->kind == NODE_SHAPE_DECL) {
                            for (int k = 0; k < cd->shape_decl.method_count; k++)
                                lower_method(mod, symtab, cd, cd->shape_decl.methods[k]);
                        } else if (cd->kind == NODE_CONST_DECL) {
                            g_consts_push(cd->const_decl.name, cd->const_decl.value);
                        }
                    }
                }
                break;
            default:
                unsupported(decl, "top-level node kind");
                break;
        }
        if (mix_error_count() > 0) return mod;
    }

    return mod;
}
