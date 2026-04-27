#include "qbe_emit.h"
#include "errors.h"
#include "arena.h"
#include "lexer.h"
#include <inttypes.h>

QbeEmitter qbe_emitter_create(FILE *out, Arena *arena, SymTab *symtab, bool debug) {
    QbeEmitter emit = {0};
    emit.out = out;
    emit.arena = arena;
    emit.symtab = symtab;
    emit.emit_debug_info = debug;
    emit.last_match_temp = -1;
    return emit;
}

static int next_temp(QbeEmitter *emit) {
    return emit->temp_counter++;
}

static int next_label(QbeEmitter *emit) {
    return emit->label_counter++;
}

static const char *qbe_name_tagged(QbeEmitter *emit, const char *base,
                                   const void *tag, const char *kind) {
    unsigned long long id = (unsigned long long)(uintptr_t)tag;
    size_t len = strlen(base) + strlen(kind) + 40;
    char *out = arena_alloc(emit->arena, len);
    snprintf(out, len, "%s__%s_%llx", base, kind, id);
    return out;
}

static const char *qbe_var_decl_name(QbeEmitter *emit, AstNode *decl) {
    return qbe_name_tagged(emit, decl->var_decl.name, decl, "v");
}

static const char *qbe_for_binding_name(QbeEmitter *emit, AstNode *for_stmt,
                                        const char *name, const char *role) {
    return qbe_name_tagged(emit, name, for_stmt, role);
}

static const char *qbe_list_comp_name(QbeEmitter *emit, AstNode *expr,
                                      const char *name) {
    return qbe_name_tagged(emit, name, expr, "lc");
}

static const char *qbe_match_binding_name(QbeEmitter *emit, AstNode *binding,
                                          const char *name, const char *kind) {
    return qbe_name_tagged(emit, name, binding, kind);
}

static void qbe_local_names_reset(QbeEmitter *emit) {
    emit->local_binding_count = 0;
    emit->local_scope_depth = 0;
}

static void qbe_bind_local_name(QbeEmitter *emit, const char *source_name,
                                const char *emitted_name) {
    if (emit->local_binding_count >= 2048) return;
    emit->local_bindings[emit->local_binding_count].source_name =
        arena_strdup(emit->arena, source_name);
    emit->local_bindings[emit->local_binding_count].emitted_name =
        arena_strdup(emit->arena, emitted_name);
    emit->local_binding_count++;
}

static bool qbe_has_local_binding(QbeEmitter *emit, const char *source_name) {
    for (int i = emit->local_binding_count - 1; i >= 0; i--) {
        if (strcmp(emit->local_bindings[i].source_name, source_name) == 0) {
            return true;
        }
    }
    return false;
}

static const char *qbe_lookup_local_name(QbeEmitter *emit,
                                         const char *source_name) {
    for (int i = emit->local_binding_count - 1; i >= 0; i--) {
        if (strcmp(emit->local_bindings[i].source_name, source_name) == 0) {
            return emit->local_bindings[i].emitted_name;
        }
    }
    return source_name;
}

static void qbe_push_local_scope(QbeEmitter *emit) {
    if (emit->local_scope_depth >= 256) return;
    emit->local_scope_marks[emit->local_scope_depth++] =
        emit->local_binding_count;
}

static void qbe_pop_local_scope(QbeEmitter *emit) {
    if (emit->local_scope_depth <= 0) return;
    emit->local_binding_count =
        emit->local_scope_marks[--emit->local_scope_depth];
}

// Emit QBE dbgloc directive for source-level debugging (DWARF line info)
static void emit_dbgloc(QbeEmitter *emit, SrcLoc loc) {
    if (!emit->emit_debug_info) return;
    if (loc.line <= 0) return;
    if (loc.line == emit->dbg_line) return;
    fprintf(emit->out, "\tdbgloc %d\n", loc.line);
    emit->dbg_line = loc.line;
}

// Get QBE type suffix for a resolved type
static const char *qbe_type(MixType *type) {
    if (!type) return "l";
    return type_to_qbe(type);
}

// Widen a narrow-integer temp (`w`) to `l` so it matches an int parameter
// or runtime function argument. Bool / byte / int8..int32 / uint8..uint32
// all live in `w` registers but MIX's `int` is `l`, so passing one to a
// builtin that takes `int` produces "invalid type for first operand"
// without this. Floats and already-`l` values pass through unchanged.
static int widen_int_to_long(QbeEmitter *emit, int val, MixType *atype) {
    if (!atype) return val;
    const char *q = type_to_qbe(atype);
    if (strcmp(q, "w") != 0) return val;
    int ext = next_temp(emit);
    // Use unsigned extension for unsigned source kinds (so a Uint32
    // bitmask like 0xFFFFFFFF doesn't sign-extend into a negative long);
    // signed extension for everything else.
    bool unsigned_src = atype->kind == TYPE_UINT8 || atype->kind == TYPE_UINT16
                     || atype->kind == TYPE_UINT32 || atype->kind == TYPE_BYTE
                     || atype->kind == TYPE_BOOL;
    fprintf(emit->out, "\t%%t%d =l %s %%t%d\n",
            ext, unsigned_src ? "extuw" : "extsw", val);
    return ext;
}

// Coerce a value temp to the QBE register type required by a field's type.
// For example, an integer value (=l) being stored into a float32 field needs
// conversion to =s via sltof+truncd, and into a float64 field needs =d sltof.
// Returns the temp holding the correctly-typed value (may be the same as val).
static int coerce_to_field_type(QbeEmitter *emit, int val, MixType *val_type,
                                MixType *field_type) {
    if (!field_type) return val;
    const char *field_qbe = type_to_qbe(field_type);
    const char *val_qbe = val_type ? type_to_qbe(val_type) : "l";

    // int -> float32: sltof to double then truncd to single
    if (strcmp(field_qbe, "s") == 0 && strcmp(val_qbe, "l") != 0 &&
        strcmp(val_qbe, "w") != 0 && strcmp(val_qbe, "s") == 0) {
        return val; // already single float
    }
    if (strcmp(field_qbe, "s") == 0 && (strcmp(val_qbe, "l") == 0 || strcmp(val_qbe, "w") == 0)) {
        // int -> float32: sltof to double, then truncd to single
        int dbl = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", dbl, val);
        int sng = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =s truncd %%t%d\n", sng, dbl);
        return sng;
    }
    if (strcmp(field_qbe, "s") == 0 && strcmp(val_qbe, "d") == 0) {
        // float64 -> float32: truncd
        int sng = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =s truncd %%t%d\n", sng, val);
        return sng;
    }
    if (strcmp(field_qbe, "d") == 0 && (strcmp(val_qbe, "l") == 0 || strcmp(val_qbe, "w") == 0)) {
        // int -> float64: sltof
        int dbl = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", dbl, val);
        return dbl;
    }
    if (strcmp(field_qbe, "d") == 0 && strcmp(val_qbe, "s") == 0) {
        // float32 -> float64: exts
        int dbl = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =d exts %%t%d\n", dbl, val);
        return dbl;
    }
    // Narrow int (`w`) -> long (`l`): zero-extend if the source was an
    // unsigned narrow type so a Uint32 mask doesn't sign-extend into a
    // negative long; sign-extend otherwise. Without this, passing a
    // bool/uint32/etc. into a function expecting `int` produces
    // "invalid type for first operand" from QBE.
    if (strcmp(field_qbe, "l") == 0 && strcmp(val_qbe, "w") == 0) {
        return widen_int_to_long(emit, val, val_type);
    }
    return val;
}

// Get QBE type for expression and parameter values. Plain shapes travel through
// codegen as pointers to value storage, so their register type stays `l`.
static const char *qbe_sig_type(MixType *type, Arena *arena) {
    (void)arena;
    if (!type) return "l";
    if (type->kind == TYPE_SHAPE) return "l";
    return type_to_qbe(type);
}

static const char *qbe_param_sig_type(MixType *type, bool is_mutable,
                                      Arena *arena) {
    if (is_mutable) return "l";
    return qbe_sig_type(type, arena);
}

static int qbe_stack_slot_size(MixType *type) {
    if (!type) return 8;
    if (type->kind == TYPE_SHAPE) return type->shape.total_size;
    switch (type->kind) {
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_FLOAT32:
        case TYPE_BOOL:
            return 4;
        case TYPE_INT16:
        case TYPE_UINT16:
            return 2;
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_BYTE:
            return 1;
        default:
            return 8;
    }
}

static int emit_alloca_temp(QbeEmitter *emit, int size) {
    int t = next_temp(emit);
    fprintf(emit->out, "\t%%t%d =l alloc8 %d\n", t, size > 0 ? size : 1);
    return t;
}

static void emit_memcpy_call(QbeEmitter *emit, int dst, int src, int size) {
    fprintf(emit->out, "\tcall $memcpy(l %%t%d, l %%t%d, l %d)\n",
            dst, src, size);
}

static void emit_memset_zero(QbeEmitter *emit, int dst, int size) {
    fprintf(emit->out, "\tcall $memset(l %%t%d, w 0, l %d)\n", dst, size);
}

static int emit_shape_temp(QbeEmitter *emit, MixType *type, bool zero_init) {
    int size = (type && type->kind == TYPE_SHAPE) ? type->shape.total_size : 8;
    int t = emit_alloca_temp(emit, size);
    if (zero_init) {
        emit_memset_zero(emit, t, size);
    }
    return t;
}

static void emit_shape_copy(QbeEmitter *emit, int dst, int src, MixType *type) {
    int size = (type && type->kind == TYPE_SHAPE) ? type->shape.total_size : 8;
    emit_memcpy_call(emit, dst, src, size);
}

static int emit_value_spill_ptr(QbeEmitter *emit, MixType *type, int value_temp) {
    int size = type_size(type);
    int slot = emit_alloca_temp(emit, size);
    if (type && type->kind == TYPE_SHAPE) {
        emit_shape_copy(emit, slot, value_temp, type);
        return slot;
    }
    const char *mem_ty = type_to_qbe_mem(type);
    fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, value_temp, slot);
    return slot;
}

static void qbe_track_mutable_param(QbeEmitter *emit, const char *name,
                                    MixType *type) {
    if (emit->mutable_param_count >= 256) return;
    emit->mutable_param_names[emit->mutable_param_count] =
        arena_strdup(emit->arena, name);
    emit->mutable_param_types[emit->mutable_param_count] = type;
    emit->mutable_param_count++;
}

static void emit_stmt(QbeEmitter *emit, AstNode *stmt);

static void emit_mutable_param_writebacks(QbeEmitter *emit) {
    for (int i = 0; i < emit->mutable_param_count; i++) {
        const char *name = emit->mutable_param_names[i];
        MixType *type = emit->mutable_param_types[i];
        const char *mem_ty = type_to_qbe_mem(type);
        const char *load_ty = type_to_qbe_load(type);
        const char *reg_ty = qbe_type(type);
        int val = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =%s load%s %%v.%s\n",
                val, reg_ty, load_ty, name);
        fprintf(emit->out, "\tstore%s %%t%d, %%p.%s\n",
                mem_ty, val, name);
    }
}

static void emit_function_epilogue(QbeEmitter *emit) {
    for (int i = emit->defer_count - 1; i >= 0; i--) {
        emit_stmt(emit, emit->deferred[i]);
    }
    emit_mutable_param_writebacks(emit);
}

static void emit_return_temp(QbeEmitter *emit, int temp) {
    emit_function_epilogue(emit);
    if (emit->current_return_type &&
        emit->current_return_type->kind == TYPE_SHAPE) {
        fprintf(emit->out, "\tcall $memcpy(l %%p._ret, l %%t%d, l %d)\n",
                temp, emit->current_return_type->shape.total_size);
        fprintf(emit->out, "\tret\n");
    } else {
        fprintf(emit->out, "\tret %%t%d\n", temp);
    }
}

static void emit_return_void(QbeEmitter *emit) {
    emit_function_epilogue(emit);
    fprintf(emit->out, "\tret\n");
}

// Register a string literal and return its QBE data name
static const char *emit_string_data(QbeEmitter *emit, const char *value, int length) {
    // Check if we already have this string
    for (int i = 0; i < emit->string_count; i++) {
        if (emit->strings[i].length == length &&
            memcmp(emit->strings[i].value, value, length) == 0) {
            return emit->strings[i].qbe_name;
        }
    }

    char name[32];
    snprintf(name, sizeof(name), "$str%d", emit->data_counter++);

    int idx = emit->string_count++;
    emit->strings[idx].value = value;
    emit->strings[idx].length = length;
    emit->strings[idx].qbe_name = arena_strdup(emit->arena, name);

    return emit->strings[idx].qbe_name;
}

// --- Expression emission ---
// Returns the temp number holding the result

static int emit_expr(QbeEmitter *emit, AstNode *expr);

static MixType *ref_base_type(MixType *type) {
    while (type &&
           (type->kind == TYPE_REF || type->kind == TYPE_BOX)) {
        if (type->kind == TYPE_REF) type = type->ref.base;
        else type = type->box.inner;
    }
    return type;
}

static bool ref_uses_direct_value(MixType *base) {
    if (!base) return false;
    switch (base->kind) {
        case TYPE_SHAPE:
        case TYPE_LIST:
        case TYPE_MAP:
        case TYPE_SET:
        case TYPE_STR:
        case TYPE_OPTIONAL:
        case TYPE_RESULT:
        case TYPE_SHARED:
        case TYPE_ZONE:
        case TYPE_TASK:
        case TYPE_PTR:
        case TYPE_REF:
        case TYPE_BOX:
        case TYPE_FUNC:
            return true;
        default:
            return false;
    }
}

static int spill_ref_value(QbeEmitter *emit, int val, MixType *type) {
    int slot = next_temp(emit);
    int size = type_size(type);
    if (size < 8) size = 8;
    fprintf(emit->out, "\t%%t%d =l alloc8 %d\n", slot, size);
    fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n",
            type_to_qbe_mem(type), val, slot);
    return slot;
}

static int emit_borrow_expr(QbeEmitter *emit, AstNode *operand) {
    if (!operand) return -1;
    MixType *base = operand->resolved_type;
    if (base && base->kind == TYPE_REF) {
        return emit_expr(emit, operand);
    }
    if (ref_uses_direct_value(base)) {
        return emit_expr(emit, operand);
    }
    if (operand->kind == NODE_UNARY_EXPR && operand->unary.op == TOK_STAR) {
        return emit_expr(emit, operand->unary.operand);
    }
    if (operand->kind == NODE_IDENT) {
        Symbol *sym = symtab_lookup(emit->symtab, operand->ident.name);
        bool has_local = qbe_has_local_binding(emit, operand->ident.name);
        if (!has_local && emit->current_shape) {
            ShapeFieldInfo *fi = type_find_field(emit->current_shape,
                                                 operand->ident.name);
            if (fi) {
                int t = next_temp(emit);
                if (fi->offset == 0) {
                    fprintf(emit->out, "\t%%t%d =l copy %%v.self\n", t);
                } else {
                    fprintf(emit->out, "\t%%t%d =l add %%v.self, %d\n",
                            t, fi->offset);
                }
                return t;
            }
        }
        if (!has_local && sym && sym->is_global) {
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy $g_%s\n", t, operand->ident.name);
            return t;
        }
        if (has_local && (operand->ident.is_stack_slot || operand->ident.is_mutable)) {
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy %%v.%s\n",
                    t, qbe_lookup_local_name(emit, operand->ident.name));
            return t;
        }
        int val = emit_expr(emit, operand);
        return spill_ref_value(emit, val, base);
    }
    if (operand->kind == NODE_FIELD_EXPR) {
        MixType *obj_type = ref_base_type(
            operand->field_expr.object ? operand->field_expr.object->resolved_type : NULL);
        if (obj_type && obj_type->kind == TYPE_SHAPE) {
            int obj = emit_expr(emit, operand->field_expr.object);
            ShapeFieldInfo *fi = type_find_field(obj_type,
                                                 operand->field_expr.field_name);
            if (fi) {
                int t = next_temp(emit);
                if (fi->offset == 0) {
                    fprintf(emit->out, "\t%%t%d =l copy %%t%d\n", t, obj);
                } else {
                    fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n",
                            t, obj, fi->offset);
                }
                return t;
            }
        }
    }
    if (operand->kind == NODE_INDEX_EXPR) {
        MixType *obj_type = ref_base_type(
            operand->index_expr.object ? operand->index_expr.object->resolved_type : NULL);
        if (obj_type && obj_type->kind == TYPE_LIST) {
            int obj = emit_expr(emit, operand->index_expr.object);
            int idx = emit_expr(emit, operand->index_expr.index);
            idx = widen_int_to_long(emit, idx,
                                    operand->index_expr.index->resolved_type);
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_list_ptr(l %%t%d, l %%t%d)\n",
                    t, obj, idx);
            return t;
        }
    }
    int val = emit_expr(emit, operand);
    return spill_ref_value(emit, val, base);
}

static int emit_mutable_arg_address(QbeEmitter *emit, AstNode *arg,
                                    MixType *expected_type) {
    if (!arg) return -1;

    if (arg->resolved_type && arg->resolved_type->kind == TYPE_REF) {
        return emit_expr(emit, arg);
    }

    if (arg->kind == NODE_UNARY_EXPR && arg->unary.op == TOK_STAR) {
        return emit_expr(emit, arg->unary.operand);
    }

    if (arg->kind == NODE_IDENT) {
        Symbol *sym = symtab_lookup(emit->symtab, arg->ident.name);
        bool has_local = qbe_has_local_binding(emit, arg->ident.name);
        if (!has_local && emit->current_shape) {
            ShapeFieldInfo *fi = type_find_field(emit->current_shape,
                                                 arg->ident.name);
            if (fi) {
                if (expected_type && expected_type->kind == TYPE_SHAPE) {
                    mix_error(arg->loc,
                              "cannot pass inline shape field '%s' as a mutable argument",
                              arg->ident.name);
                    return -1;
                }
                int t = next_temp(emit);
                if (fi->offset == 0) {
                    fprintf(emit->out, "\t%%t%d =l copy %%v.self\n", t);
                } else {
                    fprintf(emit->out, "\t%%t%d =l add %%v.self, %d\n",
                            t, fi->offset);
                }
                return t;
            }
        }

        const char *slot_name = has_local
            ? qbe_lookup_local_name(emit, arg->ident.name)
            : arg->ident.name;
        const char *slot_prefix = (!has_local && sym && sym->is_global)
            ? "$g_" : "%v.";
        int t = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =l copy %s%s\n",
                t, slot_prefix, slot_name);
        return t;
    }

    if (arg->kind == NODE_FIELD_EXPR) {
        if (expected_type && expected_type->kind == TYPE_SHAPE) {
            mix_error(arg->loc,
                      "cannot pass an inline shape field as a mutable argument");
            return -1;
        }
        int obj = emit_expr(emit, arg->field_expr.object);
        MixType *obj_type = arg->field_expr.object->resolved_type;
        if (obj_type && obj_type->kind == TYPE_SHAPE) {
            ShapeFieldInfo *fi = type_find_field(obj_type,
                                                 arg->field_expr.field_name);
            if (fi) {
                int t = next_temp(emit);
                if (fi->offset == 0) {
                    fprintf(emit->out, "\t%%t%d =l copy %%t%d\n", t, obj);
                } else {
                    fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n",
                            t, obj, fi->offset);
                }
                return t;
            }
        }
    }

    if (arg->kind == NODE_METHOD_CALL &&
        arg->method_call.is_mutable_call &&
        strcmp(arg->method_call.method_name, "at_mut") == 0 &&
        arg->method_call.object &&
        arg->method_call.object->resolved_type &&
        arg->method_call.object->resolved_type->kind == TYPE_LIST &&
        arg->method_call.arg_count == 1) {
        int obj = emit_expr(emit, arg->method_call.object);
        int idx = emit_expr(emit, arg->method_call.args[0]);
        idx = widen_int_to_long(emit, idx,
                                arg->method_call.args[0]->resolved_type);
        int t = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =l call $mix_list_ptr(l %%t%d, l %%t%d)\n",
                t, obj, idx);
        return t;
    }

    mix_error(arg->loc, "mutable argument is not addressable");
    return -1;
}

static int emit_slot_value(QbeEmitter *emit, const char *slot_name,
                           MixType *type) {
    int t = next_temp(emit);
    if (type && type->kind == TYPE_SHAPE) {
        fprintf(emit->out, "\t%%t%d =l copy %%v.%s\n", t, slot_name);
        return t;
    }
    const char *ty = qbe_type(type);
    const char *load_ty = type_to_qbe_load(type);
    fprintf(emit->out, "\t%%t%d =%s load%s %%v.%s\n", t, ty, load_ty, slot_name);
    return t;
}

static int emit_list_store_value_from_slot(QbeEmitter *emit,
                                           const char *slot_name,
                                           MixType *type) {
    int val = emit_slot_value(emit, slot_name, type);
    if (type && type_is_float(type)) {
        int bits = next_temp(emit);
        fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", bits, val);
        return bits;
    }
    return widen_int_to_long(emit, val, type);
}

static int emit_expr(QbeEmitter *emit, AstNode *expr) {
    if (!expr) return -1;

    switch (expr->kind) {
        case NODE_INT_LIT: {
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy %" PRId64 "\n", t, expr->int_lit.value);
            return t;
        }
        case NODE_FLOAT_LIT: {
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =d copy d_%a\n", t, expr->float_lit.value);
            return t;
        }
        case NODE_STRING_LIT: {
            const char *name = emit_string_data(emit, expr->string_lit.value, expr->string_lit.length);
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy %s\n", t, name);
            return t;
        }
        case NODE_STRING_INTERP: {
            // Build a single string by chaining mix_str_concat calls.
            // Each part (literal) and each interpolated expression is converted
            // to a string and concatenated onto an accumulator. The final
            // accumulator temp is returned.
            int acc = -1; // -1 means "no accumulator yet"
            for (int si = 0; si <= expr->string_interp.expr_count; si++) {
                // Part (literal text)
                if (expr->string_interp.part_lengths[si] > 0) {
                    const char *sname = emit_string_data(emit,
                        expr->string_interp.parts[si], expr->string_interp.part_lengths[si]);
                    int st = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l copy %s\n", st, sname);
                    if (acc < 0) {
                        acc = st;
                    } else {
                        int nxt = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l call $mix_str_concat(l %%t%d, l %%t%d)\n", nxt, acc, st);
                        acc = nxt;
                    }
                }
                // Interpolated expression
                if (si < expr->string_interp.expr_count) {
                    AstNode *iexpr = expr->string_interp.exprs[si];
                    int ev = emit_expr(emit, iexpr);
                    MixType *etype = iexpr->resolved_type;
                    int sv = next_temp(emit);
                    if (etype && etype->kind == TYPE_STR) {
                        // Already a string — just use it directly
                        fprintf(emit->out, "\t%%t%d =l copy %%t%d\n", sv, ev);
                    } else if (etype && type_is_float(etype)) {
                        int arg_f = ev;
                        if (etype->kind == TYPE_FLOAT32) {
                            int ext = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =d exts %%t%d\n", ext, arg_f);
                            arg_f = ext;
                        }
                        fprintf(emit->out, "\t%%t%d =l call $mix_to_string_float(d %%t%d)\n", sv, arg_f);
                    } else if (etype && etype->kind == TYPE_BOOL) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_to_string_bool(w %%t%d)\n", sv, ev);
                    } else if (etype && etype->kind == TYPE_LIST) {
                        MixType *elem = etype->list.elem_type;
                        if (elem && elem->kind == TYPE_STR) {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_list_str(l %%t%d)\n", sv, ev);
                        } else if (elem && type_is_float(elem)) {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_list_float(l %%t%d)\n", sv, ev);
                        } else if (elem && elem->kind == TYPE_BOOL) {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_list_bool(l %%t%d)\n", sv, ev);
                        } else {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_list_int(l %%t%d)\n", sv, ev);
                        }
                    } else if (etype && etype->kind == TYPE_MAP) {
                        MixType *vt = etype->map.val_type;
                        if (vt && vt->kind == TYPE_STR) {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_map_str(l %%t%d)\n", sv, ev);
                        } else {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_map(l %%t%d)\n", sv, ev);
                        }
                    } else if (etype && etype->kind == TYPE_SET) {
                        MixType *se = etype->set.elem_type;
                        if (se && type_is_integer(se)) {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_set_int(l %%t%d)\n", sv, ev);
                        } else {
                            fprintf(emit->out, "\t%%t%d =l call $mix_to_string_set(l %%t%d)\n", sv, ev);
                        }
                    } else {
                        fprintf(emit->out, "\t%%t%d =l call $mix_to_string_int(l %%t%d)\n", sv, ev);
                    }
                    if (acc < 0) {
                        acc = sv;
                    } else {
                        int nxt = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l call $mix_str_concat(l %%t%d, l %%t%d)\n", nxt, acc, sv);
                        acc = nxt;
                    }
                }
            }
            // Empty interp shouldn't happen, but guard anyway.
            if (acc < 0) {
                acc = next_temp(emit);
                const char *empty = emit_string_data(emit, "", 0);
                fprintf(emit->out, "\t%%t%d =l copy %s\n", acc, empty);
            }
            return acc;
        }
        case NODE_BOOL_LIT: {
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =w copy %d\n", t, expr->bool_lit.value ? 1 : 0);
            return t;
        }
        case NODE_NONE_LIT: {
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_optional_none()\n", t);
            return t;
        }
        case NODE_IDENT: {
            // Check if it's a field reference inside a method (translate to self.field)
            if (emit->current_shape) {
                ShapeFieldInfo *fi = type_find_field(emit->current_shape, expr->ident.name);
                if (fi) {
                    int t = next_temp(emit);
                    // Shape-typed field stores its data inline in self; expose
                    // the address (same model as NODE_FIELD_EXPR), not a load —
                    // a "loadl" of an aggregate would only read 8 bytes.
                    if (fi->type && fi->type->kind == TYPE_SHAPE) {
                        if (fi->offset == 0) {
                            fprintf(emit->out, "\t%%t%d =l copy %%v.self\n", t);
                        } else {
                            fprintf(emit->out, "\t%%t%d =l add %%v.self, %d\n", t, fi->offset);
                        }
                        return t;
                    }
                    const char *reg_ty = type_to_qbe(fi->type);
                    const char *load_ty = type_to_qbe_load(fi->type);
                    // Use %v.self directly instead of copying to intermediate temp
                    if (fi->offset == 0) {
                        fprintf(emit->out, "\t%%t%d =%s load%s %%v.self\n", t, reg_ty, load_ty);
                    } else {
                        int addr = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l add %%v.self, %d\n", addr, fi->offset);
                        fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n", t, reg_ty, load_ty, addr);
                    }
                    return t;
                }
            }
            bool has_local_binding = qbe_has_local_binding(emit, expr->ident.name);
            const char *local_name = qbe_lookup_local_name(emit, expr->ident.name);
            // Check if it's a compile-time constant.
            // We deliberately re-emit the literal at every use instead of
            // caching the first temp. Caching looked attractive, but a temp
            // defined inside one basic block (e.g. an `if` branch) does not
            // dominate uses in sibling blocks — QBE then rejects the IR with
            // "ssa temporary used undefined". Re-emitting `=l copy K` is
            // cheap and lets QBE's own constant folding clean up later.
            if (!has_local_binding) {
                for (int i = 0; i < emit->const_count; i++) {
                    if (strcmp(emit->constants[i].name, expr->ident.name) == 0) {
                        return emit_expr(emit, emit->constants[i].value);
                    }
                }
            }
            int t = next_temp(emit);
            const char *ty = qbe_type(expr->resolved_type);
            Symbol *gsym = symtab_lookup(emit->symtab, expr->ident.name);
            if (!has_local_binding && gsym && gsym->is_global) {
                const char *load_ty = type_to_qbe_load(expr->resolved_type);
                fprintf(emit->out, "\t%%t%d =%s load%s $g_%s\n",
                        t, ty, load_ty, expr->ident.name);
                return t;
            }
            // Top-level function used as a value (e.g. passed as a callback):
            // emit the function symbol address. Lambdas already do this in
            // NODE_LAMBDA; this branch handles named functions. Gate on the
            // sema-resolved type (which respects shadowing) so a local/param
            // that happens to share a name with a builtin function isn't
            // mistakenly emitted as the function symbol.
            if (expr->resolved_type && expr->resolved_type->kind == TYPE_FUNC
                && !has_local_binding
                && gsym && gsym->type && gsym->type->kind == TYPE_FUNC) {
                fprintf(emit->out, "\t%%t%d =l copy $%s\n", t, expr->ident.name);
                return t;
            }
            // Shape variables are pointers — usually `%v.x` IS the pointer
            // (NODE_VAR_DECL emits `=l copy` aliasing). For-each loop vars
            // over a shape list, in contrast, get an alloca slot holding the
            // pointer; sema flags those so we know to load instead of taking
            // the slot address.
            if (expr->resolved_type && expr->resolved_type->kind == TYPE_SHAPE) {
                if (expr->ident.is_pointer_slot) {
                    fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", t, local_name);
                } else {
                    fprintf(emit->out, "\t%%t%d =l copy %%v.%s\n", t, local_name);
                }
            } else if (expr->ident.is_stack_slot || expr->ident.is_mutable) {
                // Stack-backed locals (including read-only loop bindings) need
                // a load even when language-level mutability is false.
                const char *load_ty = type_to_qbe_load(expr->resolved_type);
                fprintf(emit->out, "\t%%t%d =%s load%s %%v.%s\n",
                        t, ty, load_ty, local_name);
            } else {
                fprintf(emit->out, "\t%%t%d =%s copy %%v.%s\n", t, ty, local_name);
            }
            return t;
        }
        case NODE_ELSE_EXPR: {
            // expr else default → check optional/result, unwrap or use fallback
            MixType *val_type = expr->else_expr.value->resolved_type;
            bool is_result = val_type && val_type->kind == TYPE_RESULT;
            MixType *inner_ok = is_result ? val_type->result.ok_type : NULL;
            bool inner_is_float = inner_ok && type_is_float(inner_ok);

            // Pre-allocate result slot on stack
            int result_slot = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l alloc8 8\n", result_slot);

            int opt = emit_expr(emit, expr->else_expr.value);
            int has = next_temp(emit);
            if (is_result) {
                fprintf(emit->out, "\t%%t%d =l call $mix_result_is_ok(l %%t%d)\n", has, opt);
            } else {
                fprintf(emit->out, "\t%%t%d =l call $mix_optional_has(l %%t%d)\n", has, opt);
            }

            int l_some = next_label(emit);
            int l_none = next_label(emit);
            int l_end = next_label(emit);

            fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", has, l_some, l_none);

            // Some/Ok: unwrap and store
            fprintf(emit->out, "@L%d\n", l_some);
            int val = next_temp(emit);
            if (is_result) {
                fprintf(emit->out, "\t%%t%d =l call $mix_result_unwrap(l %%t%d)\n", val, opt);
                if (inner_is_float) {
                    // Cast int64 bits back to double
                    int dval = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =d cast %%t%d\n", dval, val);
                    fprintf(emit->out, "\tstored %%t%d, %%t%d\n", dval, result_slot);
                } else {
                    fprintf(emit->out, "\tstorel %%t%d, %%t%d\n", val, result_slot);
                }
            } else {
                fprintf(emit->out, "\t%%t%d =l call $mix_optional_get(l %%t%d)\n", val, opt);
                fprintf(emit->out, "\tstorel %%t%d, %%t%d\n", val, result_slot);
            }
            fprintf(emit->out, "\tjmp @L%d\n", l_end);

            // None/Err: evaluate fallback and store
            fprintf(emit->out, "@L%d\n", l_none);
            int fb = emit_expr(emit, expr->else_expr.fallback);
            if (inner_is_float) {
                fprintf(emit->out, "\tstored %%t%d, %%t%d\n", fb, result_slot);
            } else {
                fprintf(emit->out, "\tstorel %%t%d, %%t%d\n", fb, result_slot);
            }
            fprintf(emit->out, "\tjmp @L%d\n", l_end);

            // Merge: load result
            fprintf(emit->out, "@L%d\n", l_end);
            int t = next_temp(emit);
            if (inner_is_float) {
                fprintf(emit->out, "\t%%t%d =d loadd %%t%d\n", t, result_slot);
            } else {
                fprintf(emit->out, "\t%%t%d =l loadl %%t%d\n", t, result_slot);
            }
            return t;
        }
        case NODE_TRY_EXPR: {
            // expr? — unwrap result/optional, propagate error if not ok
            MixType *inner_type = expr->try_expr.expr->resolved_type;
            int val = emit_expr(emit, expr->try_expr.expr);

            bool is_result = inner_type && inner_type->kind == TYPE_RESULT;

            int has = next_temp(emit);
            if (is_result) {
                fprintf(emit->out, "\t%%t%d =l call $mix_result_is_ok(l %%t%d)\n", has, val);
            } else {
                fprintf(emit->out, "\t%%t%d =l call $mix_optional_has(l %%t%d)\n", has, val);
            }

            int l_ok = next_label(emit);
            int l_err = next_label(emit);

            fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", has, l_ok, l_err);

            // Error: propagate — return the error result from current function
            fprintf(emit->out, "@L%d\n", l_err);
            if (is_result) {
                // Propagate the entire result (already an error)
                emit_return_temp(emit, val);
            } else {
                // Optional: return none wrapped as result_err
                int err_val = next_temp(emit);
                const char *msg = emit_string_data(emit, "none", 4);
                fprintf(emit->out, "\t%%t%d =l copy %s\n", err_val, msg);
                int err = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_result_err(l %%t%d)\n", err, err_val);
                emit_return_temp(emit, err);
            }

            // Ok: unwrap the value
            fprintf(emit->out, "@L%d\n", l_ok);
            int t = next_temp(emit);
            if (is_result) {
                fprintf(emit->out, "\t%%t%d =l call $mix_result_unwrap(l %%t%d)\n", t, val);
            } else {
                fprintf(emit->out, "\t%%t%d =l call $mix_optional_get(l %%t%d)\n", t, val);
            }
            return t;
        }
        case NODE_LIST_LIT: {
            // Create list, push each element. Shape lists store inline bytes.
            int list_t = next_temp(emit);
            MixType *list_type = expr->resolved_type;
            MixType *etype_for_list = (list_type && list_type->kind == TYPE_LIST)
                ? list_type->list.elem_type : NULL;
            if (etype_for_list && etype_for_list->kind == TYPE_SHAPE) {
                fprintf(emit->out,
                    "\t%%t%d =l call $mix_list_new_shape(l %d)\n",
                    list_t, etype_for_list->shape.total_size);
            } else {
                fprintf(emit->out, "\t%%t%d =l call $mix_list_new()\n", list_t);
            }
            for (int i = 0; i < expr->list_lit.element_count; i++) {
                int val = emit_expr(emit, expr->list_lit.elements[i]);
                MixType *etype = expr->list_lit.elements[i]->resolved_type;
                // Float elements: cast double bits to int64 for storage
                if (etype && type_is_float(etype)) {
                    int cast_t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", cast_t, val);
                    val = cast_t;
                }
                // Bool/small int elements: extend to 64-bit
                else if (etype && (etype->kind == TYPE_BOOL ||
                         etype->kind == TYPE_INT32 || etype->kind == TYPE_UINT32 ||
                         etype->kind == TYPE_INT16 || etype->kind == TYPE_UINT16 ||
                         etype->kind == TYPE_INT8 || etype->kind == TYPE_UINT8 ||
                         etype->kind == TYPE_BYTE)) {
                    int ext_t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l extsw %%t%d\n", ext_t, val);
                    val = ext_t;
                }
                int push_t = next_temp(emit);
                if (etype_for_list && etype_for_list->kind == TYPE_SHAPE) {
                    fprintf(emit->out, "\tcall $mix_list_push_bytes(l %%t%d, l %%t%d)\n",
                            list_t, val);
                    fprintf(emit->out, "\t%%t%d =l copy 0\n", push_t);
                } else {
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_push(l %%t%d, l %%t%d)\n",
                            push_t, list_t, val);
                }
            }
            return list_t;
        }
        case NODE_MAP_LIT: {
            int map_t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_map_new()\n", map_t);
            for (int i = 0; i < expr->map_lit.entry_count; i++) {
                int key = emit_expr(emit, expr->map_lit.keys[i]);
                int val = emit_expr(emit, expr->map_lit.values[i]);
                int set_t = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_map_set(l %%t%d, l %%t%d, l %%t%d)\n",
                        set_t, map_t, key, val);
            }
            return map_t;
        }
        case NODE_SET_LIT: {
            int set_t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_set_new()\n", set_t);
            MixType *set_elem = expr->resolved_type ? expr->resolved_type->set.elem_type : NULL;
            bool int_set = set_elem && type_is_integer(set_elem);
            for (int i = 0; i < expr->set_lit.element_count; i++) {
                int elem = emit_expr(emit, expr->set_lit.elements[i]);
                if (int_set) {
                    fprintf(emit->out, "\tcall $mix_set_add_int(l %%t%d, l %%t%d)\n", set_t, elem);
                } else {
                    fprintf(emit->out, "\tcall $mix_set_add(l %%t%d, l %%t%d)\n", set_t, elem);
                }
            }
            return set_t;
        }
        case NODE_CAST_EXPR: {
            int val = emit_expr(emit, expr->cast_expr.value);
            MixType *src = expr->cast_expr.value->resolved_type;
            int t = next_temp(emit);
            bool src_float = src && type_is_float(src);
            bool src_int = src && type_is_integer(src);

            switch (expr->cast_expr.target_type) {
                // Target: 64-bit int
                case TOK_INT: case TOK_INT64:
                    if (src_float) {
                        fprintf(emit->out, "\t%%t%d =l dtosi %%t%d\n", t, val);
                    } else {
                        fprintf(emit->out, "\t%%t%d =l extsw %%t%d\n", t, val);
                    }
                    break;
                // Target: 64-bit uint
                case TOK_UINT64:
                    if (src_float) {
                        fprintf(emit->out, "\t%%t%d =l dtosi %%t%d\n", t, val);
                    } else {
                        fprintf(emit->out, "\t%%t%d =l copy %%t%d\n", t, val);
                    }
                    break;
                // Target: 32-bit int/uint
                case TOK_INT32: case TOK_UINT32:
                    if (src_float) {
                        int tmp = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l dtosi %%t%d\n", tmp, val);
                        fprintf(emit->out, "\t%%t%d =w copy %%t%d\n", t, tmp);
                    } else {
                        fprintf(emit->out, "\t%%t%d =w copy %%t%d\n", t, val);
                    }
                    break;
                // Target: 16-bit or 8-bit int/uint/byte
                case TOK_INT16: case TOK_UINT16:
                case TOK_INT8: case TOK_UINT8: case TOK_BYTE:
                    if (src_float) {
                        int tmp = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l dtosi %%t%d\n", tmp, val);
                        fprintf(emit->out, "\t%%t%d =w copy %%t%d\n", t, tmp);
                    } else {
                        fprintf(emit->out, "\t%%t%d =w copy %%t%d\n", t, val);
                    }
                    break;
                // Target: 64-bit float (double)
                case TOK_FLOAT: case TOK_FLOAT64:
                    if (src_int) {
                        fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", t, val);
                    } else if (src && src->kind == TYPE_FLOAT32) {
                        fprintf(emit->out, "\t%%t%d =d exts %%t%d\n", t, val);
                    } else {
                        fprintf(emit->out, "\t%%t%d =d copy %%t%d\n", t, val);
                    }
                    break;
                // Target: 32-bit float
                case TOK_FLOAT32:
                    if (src_int) {
                        int tmp = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", tmp, val);
                        fprintf(emit->out, "\t%%t%d =s truncd %%t%d\n", t, tmp);
                    } else if (src_float && (!src || src->kind != TYPE_FLOAT32)) {
                        fprintf(emit->out, "\t%%t%d =s truncd %%t%d\n", t, val);
                    } else {
                        fprintf(emit->out, "\t%%t%d =s copy %%t%d\n", t, val);
                    }
                    break;
                // Target: bool
                case TOK_BOOL:
                    if (src_float) {
                        int zero = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =d copy d_0.0\n", zero);
                        fprintf(emit->out, "\t%%t%d =w cned %%t%d, %%t%d\n", t, val, zero);
                    } else {
                        int zero = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l copy 0\n", zero);
                        fprintf(emit->out, "\t%%t%d =w cnel %%t%d, %%t%d\n", t, val, zero);
                    }
                    break;
                default:
                    fprintf(emit->out, "\t%%t%d =l copy %%t%d\n", t, val);
                    break;
            }
            return t;
        }
        case NODE_INDEX_EXPR: {
            int obj = emit_expr(emit, expr->index_expr.object);
            int idx = emit_expr(emit, expr->index_expr.index);
            MixType *obj_type = ref_base_type(expr->index_expr.object->resolved_type);
            MixType *elem = (obj_type && obj_type->kind == TYPE_LIST)
                ? obj_type->list.elem_type : NULL;
            int t = next_temp(emit);
            if (obj_type && obj_type->kind == TYPE_MAP) {
                fprintf(emit->out, "\t%%t%d =l call $mix_map_get(l %%t%d, l %%t%d)\n", t, obj, idx);
            } else {
                if (elem && elem->kind == TYPE_SHAPE) {
                    int ptr = next_temp(emit);
                    int copy_slot = emit_shape_temp(emit, elem, false);
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_ptr(l %%t%d, l %%t%d)\n",
                            ptr, obj, idx);
                    emit_shape_copy(emit, copy_slot, ptr, elem);
                    return copy_slot;
                }
                fprintf(emit->out, "\t%%t%d =l call $mix_list_get(l %%t%d, l %%t%d)\n", t, obj, idx);
                if (elem && type_is_float(elem)) {
                    int cast_t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =d cast %%t%d\n", cast_t, t);
                    return cast_t;
                }
            }
            return t;
        }
        case NODE_SLICE_EXPR: {
            int obj = emit_expr(emit, expr->slice_expr.object);
            int start_v, end_v;
            if (expr->slice_expr.start) {
                start_v = emit_expr(emit, expr->slice_expr.start);
            } else {
                start_v = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", start_v);
            }
            if (expr->slice_expr.end) {
                end_v = emit_expr(emit, expr->slice_expr.end);
            } else {
                end_v = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_list_len(l %%t%d)\n", end_v, obj);
            }
            int inc_v = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =w copy %d\n", inc_v, expr->slice_expr.inclusive ? 1 : 0);
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_list_slice(l %%t%d, l %%t%d, l %%t%d, w %%t%d)\n",
                    t, obj, start_v, end_v, inc_v);
            return t;
        }
        case NODE_LIST_COMP: {
            // Create result list
            int result_list = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_list_new()\n", result_list);

            // Evaluate iterable
            int list_ptr = emit_expr(emit, expr->list_comp.iterable);
            int len_t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_list_len(l %%t%d)\n", len_t, list_ptr);

            // Internal index
            int idx_id = next_label(emit);
            char idx_name[64];
            snprintf(idx_name, sizeof(idx_name), "_cidx_%d", idx_id);
            fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", idx_name);
            fprintf(emit->out, "\tstorel 0, %%v.%s\n", idx_name);

            // Loop variable
            const char *var_name = qbe_list_comp_name(emit, expr, expr->list_comp.var_name);
            qbe_push_local_scope(emit);
            fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", var_name);
            fprintf(emit->out, "\tstorel 0, %%v.%s\n", var_name);
            qbe_bind_local_name(emit, expr->list_comp.var_name, var_name);

            int l_cond = next_label(emit);
            int l_body = next_label(emit);
            int l_end = next_label(emit);

            fprintf(emit->out, "@L%d\n", l_cond);
            int ci = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci, idx_name);
            int cmp = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =w csltl %%t%d, %%t%d\n", cmp, ci, len_t);
            fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cmp, l_body, l_end);

            fprintf(emit->out, "@L%d\n", l_body);
            int ci2 = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci2, idx_name);
            int elem = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_list_get(l %%t%d, l %%t%d)\n", elem, list_ptr, ci2);
            fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", elem, var_name);

            if (expr->list_comp.condition) {
                int cond_val = emit_expr(emit, expr->list_comp.condition);
                int l_push = next_label(emit);
                int l_skip = next_label(emit);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cond_val, l_push, l_skip);
                fprintf(emit->out, "@L%d\n", l_push);
                int val = emit_expr(emit, expr->list_comp.expr);
                MixType *comp_etype = expr->list_comp.expr->resolved_type;
                if (comp_etype && type_is_float(comp_etype)) {
                    int cast_t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", cast_t, val);
                    val = cast_t;
                }
                fprintf(emit->out, "\tcall $mix_list_push(l %%t%d, l %%t%d)\n", result_list, val);
                fprintf(emit->out, "\tjmp @L%d\n", l_skip);
                fprintf(emit->out, "@L%d\n", l_skip);
            } else {
                int val = emit_expr(emit, expr->list_comp.expr);
                MixType *comp_etype = expr->list_comp.expr->resolved_type;
                if (comp_etype && type_is_float(comp_etype)) {
                    int cast_t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", cast_t, val);
                    val = cast_t;
                }
                fprintf(emit->out, "\tcall $mix_list_push(l %%t%d, l %%t%d)\n", result_list, val);
            }

            // Increment index
            int ci3 = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci3, idx_name);
            int ni = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l add %%t%d, 1\n", ni, ci3);
            fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", ni, idx_name);
            fprintf(emit->out, "\tjmp @L%d\n", l_cond);
            fprintf(emit->out, "@L%d\n", l_end);
            qbe_pop_local_scope(emit);
            return result_list;
        }
        case NODE_LAMBDA: {
            // Generate a unique name for the lambda function
            char lname[64];
            snprintf(lname, sizeof(lname), "lambda_%d", emit->lambda_counter++);
            expr->lambda.generated_name = arena_strdup(emit->arena, lname);

            // Collect for later emission
            if (emit->lambda_count < 256) {
                emit->lambdas[emit->lambda_count++] = expr;
            }

            // Return the function pointer
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy $%s\n", t, lname);
            return t;
        }
        case NODE_BINARY_EXPR: {
            // Constant folding: if both operands are integer literals, compute at compile time
            if (expr->binary.left->kind == NODE_INT_LIT &&
                expr->binary.right->kind == NODE_INT_LIT) {
                int64_t lv = expr->binary.left->int_lit.value;
                int64_t rv = expr->binary.right->int_lit.value;
                int64_t result = 0;
                bool folded = true;
                switch (expr->binary.op) {
                    case TOK_PLUS:    result = lv + rv; break;
                    case TOK_MINUS:   result = lv - rv; break;
                    case TOK_STAR:    result = lv * rv; break;
                    case TOK_SLASH:   result = rv != 0 ? lv / rv : 0; break;
                    case TOK_PERCENT: result = rv != 0 ? lv % rv : 0; break;
                    case TOK_PIPE:    result = lv | rv; break;
                    default: folded = false; break;
                }
                if (folded) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l copy %" PRId64 "\n", t, result);
                    return t;
                }
            }
            // Constant folding for float literals
            if (expr->binary.left->kind == NODE_FLOAT_LIT &&
                expr->binary.right->kind == NODE_FLOAT_LIT) {
                double lv = expr->binary.left->float_lit.value;
                double rv = expr->binary.right->float_lit.value;
                double result = 0;
                bool folded = true;
                switch (expr->binary.op) {
                    case TOK_PLUS:    result = lv + rv; break;
                    case TOK_MINUS:   result = lv - rv; break;
                    case TOK_STAR:    result = lv * rv; break;
                    case TOK_SLASH:   result = rv != 0.0 ? lv / rv : 0.0; break;
                    default: folded = false; break;
                }
                if (folded) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =d copy d_%a\n", t, result);
                    return t;
                }
            }

            // Short-circuit evaluation for and/or
            if (expr->binary.op == TOK_AND) {
                int left = emit_expr(emit, expr->binary.left);
                int t = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =w copy 0\n", t);
                int l_right = next_label(emit);
                int l_end = next_label(emit);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", left, l_right, l_end);
                fprintf(emit->out, "@L%d\n", l_right);
                int right = emit_expr(emit, expr->binary.right);
                fprintf(emit->out, "\t%%t%d =w copy %%t%d\n", t, right);
                fprintf(emit->out, "\tjmp @L%d\n", l_end);
                fprintf(emit->out, "@L%d\n", l_end);
                return t;
            }
            if (expr->binary.op == TOK_OR) {
                int left = emit_expr(emit, expr->binary.left);
                int t = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =w copy 1\n", t);
                int l_right = next_label(emit);
                int l_end = next_label(emit);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", left, l_end, l_right);
                fprintf(emit->out, "@L%d\n", l_right);
                int right = emit_expr(emit, expr->binary.right);
                fprintf(emit->out, "\t%%t%d =w copy %%t%d\n", t, right);
                fprintf(emit->out, "\tjmp @L%d\n", l_end);
                fprintf(emit->out, "@L%d\n", l_end);
                return t;
            }

            int left = emit_expr(emit, expr->binary.left);
            int right = emit_expr(emit, expr->binary.right);
            int t = next_temp(emit);

            MixType *ltype = expr->binary.left->resolved_type;

            // String concatenation: str + str -> str
            if (ltype && ltype->kind == TYPE_STR && expr->binary.op == TOK_PLUS) {
                fprintf(emit->out, "\t%%t%d =l call $mix_str_concat(l %%t%d, l %%t%d)\n",
                        t, left, right);
                return t;
            }

            // String comparison: use strcmp
            if (ltype && ltype->kind == TYPE_STR) {
                const char *cmp_op = NULL;
                switch (expr->binary.op) {
                    case TOK_EQEQ: cmp_op = "ceqw"; break;
                    case TOK_NEQ:  cmp_op = "cnew"; break;
                    case TOK_LT:   cmp_op = "csltw"; break;
                    case TOK_GT:   cmp_op = "csgtw"; break;
                    case TOK_LTE:  cmp_op = "cslew"; break;
                    case TOK_GTE:  cmp_op = "csgew"; break;
                    default: break;
                }
                if (cmp_op) {
                    int cmp = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =w call $strcmp(l %%t%d, l %%t%d)\n",
                            cmp, left, right);
                    fprintf(emit->out, "\t%%t%d =w %s %%t%d, 0\n", t, cmp_op, cmp);
                    return t;
                }
            }

            // Check for operator overloading on shapes
            if (ltype && ltype->kind == TYPE_SHAPE) {
                const char *op_method = NULL;
                switch (expr->binary.op) {
                    case TOK_PLUS: op_method = "op_add"; break;
                    case TOK_MINUS: op_method = "op_sub"; break;
                    case TOK_STAR: op_method = "op_mul"; break;
                    case TOK_SLASH: op_method = "op_div"; break;
                    case TOK_PERCENT: op_method = "op_mod"; break;
                    case TOK_EQEQ: op_method = "op_eq"; break;
                    case TOK_NEQ: op_method = "op_neq"; break;
                    case TOK_LT: op_method = "op_lt"; break;
                    case TOK_GT: op_method = "op_gt"; break;
                    case TOK_LTE: op_method = "op_lte"; break;
                    case TOK_GTE: op_method = "op_gte"; break;
                    default: break;
                }
                if (op_method) {
                    char mangled[256];
                    snprintf(mangled, sizeof(mangled), "%s_%s", ltype->shape.name, op_method);
                    Symbol *msym = symtab_lookup(emit->symtab, mangled);
                    if (msym) {
                        bool shape_return = expr->resolved_type &&
                            expr->resolved_type->kind == TYPE_SHAPE;
                        int ret_slot = shape_return
                            ? emit_shape_temp(emit, expr->resolved_type, false)
                            : t;
                        const char *ret_sig = shape_return ? NULL
                            : qbe_sig_type(expr->resolved_type, emit->arena);
                        MixType *rtype = expr->binary.right->resolved_type;
                        const char *right_ty = qbe_sig_type(rtype, emit->arena);
                        // Self always passes as a pointer (matches emit_method).
                        if (shape_return) {
                            fprintf(emit->out, "\tcall $%s(l %%t%d, l %%t%d, %s %%t%d)\n",
                                    mangled, ret_slot, left, right_ty, right);
                        } else {
                            fprintf(emit->out, "\t%%t%d =%s call $%s(l %%t%d, %s %%t%d)\n",
                                    t, ret_sig, mangled, left, right_ty, right);
                        }
                        return ret_slot;
                    }
                }
            }

            // If the two integer operands disagree on width (typical when a
            // C function returning Uint32 is used with a MIX `int` literal),
            // promote the narrower to `l` first so QBE accepts the op.
            MixType *rtype_for_widen = expr->binary.right->resolved_type;
            const char *lq = qbe_type(ltype);
            const char *rq = qbe_type(rtype_for_widen);
            if (strcmp(lq, "w") == 0 && strcmp(rq, "l") == 0) {
                left = widen_int_to_long(emit, left, ltype);
                ltype = rtype_for_widen;
            } else if (strcmp(lq, "l") == 0 && strcmp(rq, "w") == 0) {
                right = widen_int_to_long(emit, right, rtype_for_widen);
            }

            const char *ty = qbe_type(ltype);
            bool is_flt = ltype && type_is_float(ltype);

            switch (expr->binary.op) {
                case TOK_PLUS:
                    fprintf(emit->out, "\t%%t%d =%s add %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_MINUS:
                    fprintf(emit->out, "\t%%t%d =%s sub %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_STAR:
                    fprintf(emit->out, "\t%%t%d =%s mul %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_SLASH:
                    fprintf(emit->out, "\t%%t%d =%s div %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_PERCENT:
                    fprintf(emit->out, "\t%%t%d =%s rem %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_PIPE:
                    fprintf(emit->out, "\t%%t%d =%s or %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_EQEQ:
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w ceq%s %%t%d, %%t%d\n", t, ty, left, right);
                    else
                        fprintf(emit->out, "\t%%t%d =w ceq%s %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_NEQ:
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w cne%s %%t%d, %%t%d\n", t, ty, left, right);
                    else
                        fprintf(emit->out, "\t%%t%d =w cne%s %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_LT:
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w clt%s %%t%d, %%t%d\n", t, ty, left, right);
                    else
                        fprintf(emit->out, "\t%%t%d =w cslt%s %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_GT:
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w cgt%s %%t%d, %%t%d\n", t, ty, left, right);
                    else
                        fprintf(emit->out, "\t%%t%d =w csgt%s %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_LTE:
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w cle%s %%t%d, %%t%d\n", t, ty, left, right);
                    else
                        fprintf(emit->out, "\t%%t%d =w csle%s %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                case TOK_GTE:
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w cge%s %%t%d, %%t%d\n", t, ty, left, right);
                    else
                        fprintf(emit->out, "\t%%t%d =w csge%s %%t%d, %%t%d\n", t, ty, left, right);
                    break;
                default:
                    mix_error(expr->loc, "unsupported binary operator '%s' in codegen", token_kind_name(expr->binary.op));
                    break;
            }
            return t;
        }
        case NODE_UNARY_EXPR: {
            int operand = emit_expr(emit, expr->unary.operand);
            int t = next_temp(emit);
            const char *ty = qbe_type(expr->unary.operand->resolved_type);

            if (expr->unary.op == TOK_MINUS) {
                fprintf(emit->out, "\t%%t%d =%s sub 0, %%t%d\n", t, ty, operand);
            } else if (expr->unary.op == TOK_NOT) {
                fprintf(emit->out, "\t%%t%d =w ceqw %%t%d, 0\n", t, operand);
            } else if (expr->unary.op == TOK_REF ||
                       expr->unary.op == TOK_REF_MUT) {
                return emit_borrow_expr(emit, expr->unary.operand);
            } else if (expr->unary.op == TOK_AMPERSAND) {
                // Address-of operator: &x
                AstNode *inner = expr->unary.operand;
                if (inner->kind == NODE_IDENT && inner->ident.is_mutable) {
                    // Mutable variable: %v.x is already a pointer (from alloc)
                    fprintf(emit->out, "\t%%t%d =l copy %%v.%s\n",
                            t, qbe_lookup_local_name(emit, inner->ident.name));
                } else {
                    // Immutable variable or expression: spill to stack slot
                    fprintf(emit->out, "\t%%t%d =l alloc8 8\n", t);
                    fprintf(emit->out, "\tstorel %%t%d, %%t%d\n", operand, t);
                }
            } else if (expr->unary.op == TOK_STAR &&
                       expr->unary.operand->resolved_type &&
                       (expr->unary.operand->resolved_type->kind == TYPE_REF ||
                        expr->unary.operand->resolved_type->kind == TYPE_BOX)) {
                MixType *base = expr->unary.operand->resolved_type->kind == TYPE_REF
                    ? expr->unary.operand->resolved_type->ref.base
                    : expr->unary.operand->resolved_type->box.inner;
                if (ref_uses_direct_value(base)) {
                    return operand;
                }
                const char *load_ty = type_to_qbe_load(base);
                const char *reg_ty = qbe_type(base);
                fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n",
                        t, reg_ty, load_ty, operand);
            } else {
                fprintf(emit->out, "\t%%t%d =%s copy %%t%d\n", t, ty, operand);
            }
            return t;
        }
        case NODE_CALL_EXPR: {
            if (expr->call.arg_count > 64) {
                mix_error(expr->loc, "too many function arguments (max 64)");
                return -1;
            }
            Symbol *fn_sym = symtab_lookup(emit->symtab, expr->call.name);
            MixType *fn_type = (fn_sym && fn_sym->type && fn_sym->type->kind == TYPE_FUNC)
                ? fn_sym->type : NULL;
            int arg_temps[64];
            for (int i = 0; i < expr->call.arg_count; i++) {
                MixType *param_type = (fn_type && i < fn_type->func.param_count)
                    ? fn_type->func.param_types[i]
                    : expr->call.args[i]->resolved_type;
                if (fn_type && i < fn_type->func.param_count &&
                    fn_type->func.param_mutable &&
                    fn_type->func.param_mutable[i]) {
                    arg_temps[i] = emit_mutable_arg_address(
                        emit, expr->call.args[i], param_type);
                } else {
                    arg_temps[i] = emit_expr(emit, expr->call.args[i]);
                }
            }

            int t = next_temp(emit);

            // Special handling for print()
            if (strcmp(expr->call.name, "print") == 0 && expr->call.arg_count == 1) {
                AstNode *arg = expr->call.args[0];
                // String interpolation: emit_expr returned the built string; print it.
                if (arg->kind == NODE_STRING_INTERP) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_print_str(l %%t%d)\n", t, arg_temps[0]);
                    return t;
                }
                MixType *atype = arg->resolved_type;
                if (atype && atype->kind == TYPE_STR) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_print_str(l %%t%d)\n", t, arg_temps[0]);
                } else if (atype && type_is_float(atype)) {
                    int arg_f = arg_temps[0];
                    if (atype->kind == TYPE_FLOAT32) {
                        int ext = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =d exts %%t%d\n", ext, arg_f);
                        arg_f = ext;
                    }
                    fprintf(emit->out, "\t%%t%d =l call $mix_print_float(d %%t%d)\n", t, arg_f);
                } else if (atype && atype->kind == TYPE_BOOL) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_print_bool(w %%t%d)\n", t, arg_temps[0]);
                } else if (atype && atype->kind == TYPE_LIST) {
                    MixType *elem = atype->list.elem_type;
                    if (elem && elem->kind == TYPE_STR) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_list_str(l %%t%d)\n", t, arg_temps[0]);
                    } else if (elem && type_is_float(elem)) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_list_float(l %%t%d)\n", t, arg_temps[0]);
                    } else if (elem && elem->kind == TYPE_BOOL) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_list_bool(l %%t%d)\n", t, arg_temps[0]);
                    } else {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_list_int(l %%t%d)\n", t, arg_temps[0]);
                    }
                } else if (atype && atype->kind == TYPE_MAP) {
                    MixType *val_elem = atype->map.val_type;
                    if (val_elem && val_elem->kind == TYPE_STR) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_map_str(l %%t%d)\n", t, arg_temps[0]);
                    } else {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_map(l %%t%d)\n", t, arg_temps[0]);
                    }
                } else if (atype && atype->kind == TYPE_SET) {
                    MixType *selem = atype->set.elem_type;
                    if (selem && type_is_integer(selem)) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_set_int(l %%t%d)\n", t, arg_temps[0]);
                    } else {
                        fprintf(emit->out, "\t%%t%d =l call $mix_print_set(l %%t%d)\n", t, arg_temps[0]);
                    }
                } else {
                    // For small integer types (w), extend to l before printing
                    int arg_t = arg_temps[0];
                    if (atype && (atype->kind == TYPE_INT32 || atype->kind == TYPE_UINT32 ||
                                  atype->kind == TYPE_INT16 || atype->kind == TYPE_UINT16 ||
                                  atype->kind == TYPE_INT8 || atype->kind == TYPE_UINT8 ||
                                  atype->kind == TYPE_BYTE)) {
                        int ext = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l extsw %%t%d\n", ext, arg_t);
                        arg_t = ext;
                    }
                    fprintf(emit->out, "\t%%t%d =l call $mix_print_int(l %%t%d)\n", t, arg_t);
                }
                return t;
            }

            // File I/O built-ins
            if (strcmp(expr->call.name, "file_open") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =l call $mix_file_open(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "file_read") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_file_read(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_write") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\tcall $mix_file_write(l %%t%d, l %%t%d)\n",
                        arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "file_close") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\tcall $mix_file_close(l %%t%d)\n", arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_read_all") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_file_read_all(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_write_all") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =w call $mix_file_write_all(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }

            // OS built-ins
            if (strcmp(expr->call.name, "shell") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_shell(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "shell_output") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_shell_output(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_exists") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =w call $mix_file_exists(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "list_dir") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_list_dir(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "env") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_env(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "exit") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\tcall $mix_exit(l %%t%d)\n", arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "getcwd") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_getcwd()\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "mkdir") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =w call $mix_mkdir(l %%t%d)\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "args") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_args()\n", t);
                return t;
            }

            // Math built-ins (single arg: float -> float)
            {
                const char *math1[] = {"sqrt", "abs", "sin", "cos", "tan", "log", "floor", "ceil", "round"};
                const char *rt1[]   = {"mix_math_sqrt", "mix_math_abs", "mix_math_sin", "mix_math_cos",
                                       "mix_math_tan", "mix_math_log", "mix_math_floor", "mix_math_ceil",
                                       "mix_math_round"};
                for (int mi = 0; mi < 9; mi++) {
                    if (strcmp(expr->call.name, math1[mi]) == 0 && expr->call.arg_count == 1) {
                        // If argument is int, convert to float first
                        MixType *atype = expr->call.args[0]->resolved_type;
                        int arg = arg_temps[0];
                        if (atype && type_is_integer(atype)) {
                            int conv = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", conv, arg);
                            arg = conv;
                        }
                        fprintf(emit->out, "\t%%t%d =d call $%s(d %%t%d)\n", t, rt1[mi], arg);
                        return t;
                    }
                }
            }
            // Math built-ins (two args: float, float -> float)
            {
                const char *math2[] = {"pow", "min", "max"};
                const char *rt2[]   = {"mix_math_pow", "mix_math_min", "mix_math_max"};
                for (int mi = 0; mi < 3; mi++) {
                    if (strcmp(expr->call.name, math2[mi]) == 0 && expr->call.arg_count == 2) {
                        MixType *atype0 = expr->call.args[0]->resolved_type;
                        MixType *atype1 = expr->call.args[1]->resolved_type;
                        int a0 = arg_temps[0], a1 = arg_temps[1];
                        if (atype0 && type_is_integer(atype0)) {
                            int conv = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", conv, a0);
                            a0 = conv;
                        }
                        if (atype1 && type_is_integer(atype1)) {
                            int conv = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", conv, a1);
                            a1 = conv;
                        }
                        fprintf(emit->out, "\t%%t%d =d call $%s(d %%t%d, d %%t%d)\n",
                                t, rt2[mi], a0, a1);
                        return t;
                    }
                }
            }

            // to_string(value) -> str
            if (strcmp(expr->call.name, "to_string") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                if (atype && type_is_float(atype)) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_to_string_float(d %%t%d)\n",
                            t, arg_temps[0]);
                } else {
                    int v = widen_int_to_long(emit, arg_temps[0], atype);
                    fprintf(emit->out, "\t%%t%d =l call $mix_to_string_int(l %%t%d)\n",
                            t, v);
                }
                return t;
            }

            // str_reverse(s: str) -> str
            if (strcmp(expr->call.name, "str_reverse") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_str_reverse(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }

            // str_count(s: str, sub: str) -> int
            if (strcmp(expr->call.name, "str_count") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =l call $mix_str_count(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }

            // to_int(value) -> int
            if (strcmp(expr->call.name, "to_int") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                if (atype && type_is_float(atype)) {
                    // float -> int: QBE dtosi
                    fprintf(emit->out, "\t%%t%d =l dtosi %%t%d\n", t, arg_temps[0]);
                } else if (atype && atype->kind == TYPE_STR) {
                    // str -> int: call runtime
                    fprintf(emit->out, "\t%%t%d =l call $mix_parse_int(l %%t%d)\n",
                            t, arg_temps[0]);
                } else {
                    // int -> int (or smaller int types): just copy
                    fprintf(emit->out, "\t%%t%d =l extsw %%t%d\n", t, arg_temps[0]);
                }
                return t;
            }

            // to_float(value) -> float
            if (strcmp(expr->call.name, "to_float") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                if (atype && type_is_integer(atype)) {
                    // int -> float: QBE sltof
                    fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", t, arg_temps[0]);
                } else if (atype && atype->kind == TYPE_STR) {
                    // str -> float: call runtime
                    fprintf(emit->out, "\t%%t%d =d call $mix_parse_float(l %%t%d)\n",
                            t, arg_temps[0]);
                } else {
                    // float -> float: just copy
                    fprintf(emit->out, "\t%%t%d =d copy %%t%d\n", t, arg_temps[0]);
                }
                return t;
            }

            // to_set(list) -> set
            if (strcmp(expr->call.name, "to_set") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                MixType *elem = (atype && atype->kind == TYPE_LIST) ? atype->list.elem_type : NULL;
                if (elem && elem->kind == TYPE_STR) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_from_list(l %%t%d)\n",
                            t, arg_temps[0]);
                } else {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_from_list_int(l %%t%d)\n",
                            t, arg_temps[0]);
                }
                return t;
            }

            // ord(s) -> int (Unicode code point)
            if (strcmp(expr->call.name, "ord") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_ord(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }

            // chr(n) -> str (code point to character)
            if (strcmp(expr->call.name, "chr") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_chr(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }

            // panic(msg) -> never returns
            if (strcmp(expr->call.name, "panic") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\tcall $mix_panic(l %%t%d)\n", arg_temps[0]);
                return t;
            }

            // assert(cond, msg) -> aborts if cond is false
            if (strcmp(expr->call.name, "assert") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\tcall $mix_assert(w %%t%d, l %%t%d)\n",
                        arg_temps[0], arg_temps[1]);
                return t;
            }

            // len(x) -> int  polymorphic
            if (strcmp(expr->call.name, "len") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                const char *fn = "mix_list_len";
                if (atype && atype->kind == TYPE_STR) fn = "mix_str_len";
                else if (atype && atype->kind == TYPE_MAP) fn = "mix_map_len";
                else if (atype && atype->kind == TYPE_SET) fn = "mix_set_len";
                fprintf(emit->out, "\t%%t%d =l call $%s(l %%t%d)\n",
                        t, fn, arg_temps[0]);
                return t;
            }

            // type_of(x) -> str  (compile-time type name)
            if (strcmp(expr->call.name, "type_of") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                const char *tname = atype ? type_kind_name(atype->kind) : "unknown";
                const char *sname = emit_string_data(emit, tname, (int)strlen(tname));
                fprintf(emit->out, "\t%%t%d =l copy %s\n", t, sname);
                return t;
            }

            // sizeof(x) -> int  (compile-time size of value's type)
            if (strcmp(expr->call.name, "sizeof") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                int sz = atype ? type_size(atype) : 8;
                fprintf(emit->out, "\t%%t%d =l copy %d\n", t, sz);
                return t;
            }

            // Memory builtins
            if (strcmp(expr->call.name, "alloc") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_alloc(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "bytes") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_bytes(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "peek_u32") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =l call $mix_peek_u32_at(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "peek_byte") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =l call $mix_peek_byte(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "peek_f32") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =d call $mix_peek_f32(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "memcpy") == 0 && expr->call.arg_count == 3) {
                fprintf(emit->out, "\tcall $mix_memcpy(l %%t%d, l %%t%d, l %%t%d)\n",
                        arg_temps[0], arg_temps[1], arg_temps[2]);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "poke_f32") == 0 && expr->call.arg_count == 3) {
                // Convert arg to double if it's an int
                int val_temp = arg_temps[2];
                MixType *atype = expr->call.args[2]->resolved_type;
                if (atype && type_is_integer(atype)) {
                    int conv = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =d sltof %%t%d\n", conv, val_temp);
                    val_temp = conv;
                }
                fprintf(emit->out, "\tcall $mix_poke_f32(l %%t%d, l %%t%d, d %%t%d)\n",
                        arg_temps[0], arg_temps[1], val_temp);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "poke_u32") == 0 && expr->call.arg_count == 3) {
                int v = widen_int_to_long(emit, arg_temps[2],
                                          expr->call.args[2]->resolved_type);
                fprintf(emit->out, "\tcall $mix_poke_u32(l %%t%d, l %%t%d, l %%t%d)\n",
                        arg_temps[0], arg_temps[1], v);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "poke_ptr") == 0 && expr->call.arg_count == 3) {
                fprintf(emit->out, "\tcall $mix_poke_ptr(l %%t%d, l %%t%d, l %%t%d)\n",
                        arg_temps[0], arg_temps[1], arg_temps[2]);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "peek_ptr") == 0 && expr->call.arg_count == 2) {
                fprintf(emit->out, "\t%%t%d =l call $mix_peek_ptr(l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "pack2") == 0 && expr->call.arg_count == 3) {
                fprintf(emit->out, "\t%%t%d =l call $mix_pack2(l %%t%d, l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1], arg_temps[2]);
                return t;
            }
            if (strcmp(expr->call.name, "pack3") == 0 && expr->call.arg_count == 4) {
                fprintf(emit->out, "\t%%t%d =l call $mix_pack3(l %%t%d, l %%t%d, l %%t%d, l %%t%d)\n",
                        t, arg_temps[0], arg_temps[1], arg_temps[2], arg_temps[3]);
                return t;
            }
            if (strcmp(expr->call.name, "list_to_f32") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_list_to_f32(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "free_mem") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\tcall $mix_free(l %%t%d)\n", arg_temps[0]);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                return t;
            }
            if ((strcmp(expr->call.name, "box") == 0 ||
                 strcmp(expr->call.name, "promote") == 0) &&
                expr->call.arg_count == 2) {
                MixType *src_type = expr->call.args[1]->resolved_type;
                MixType *boxed_type = src_type;
                int src_ptr = arg_temps[1];
                if (boxed_type && boxed_type->kind == TYPE_REF) {
                    boxed_type = boxed_type->ref.base;
                } else if (boxed_type && boxed_type->kind == TYPE_BOX) {
                    boxed_type = boxed_type->box.inner;
                } else if (!(boxed_type && boxed_type->kind == TYPE_SHAPE)) {
                    src_ptr = emit_value_spill_ptr(emit, boxed_type, arg_temps[1]);
                }
                fprintf(emit->out, "\t%%t%d =l call $mix_box_clone(l %%t%d, l %%t%d, l %d)\n",
                        t, arg_temps[0], src_ptr, type_size(boxed_type));
                return t;
            }
            if (strcmp(expr->call.name, "random_seed") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\tcall $mix_random_seed(l %%t%d)\n", arg_temps[0]);
                fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "random_int") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_random_int()\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "_mix_rc_alloc_count") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_rc_get_alloc_count()\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "_mix_rc_free_count") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_rc_get_free_count()\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "random_float") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =d call $mix_random_float()\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "time_now_ms") == 0 && expr->call.arg_count == 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_time_now_ms()\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "int_to_hex") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_int_to_hex(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "int_to_bin") == 0 && expr->call.arg_count == 1) {
                fprintf(emit->out, "\t%%t%d =l call $mix_int_to_bin(l %%t%d)\n",
                        t, arg_temps[0]);
                return t;
            }

            // Regular function call
            // Check if this is a direct function call or an indirect call (lambda/function pointer)
            // Check if the name is a variable holding a function pointer (lambda)
            bool is_lambda_var = false;
            for (int fv = 0; fv < emit->fn_ptr_var_count; fv++) {
                if (strcmp(emit->fn_ptr_vars[fv], expr->call.name) == 0) {
                    is_lambda_var = true;
                    break;
                }
            }
            if (qbe_has_local_binding(emit, expr->call.name)) {
                is_lambda_var = true;
            }
            // Also treat as indirect if the symbol is not found in global scope
            // (parameters and local variables aren't in the symtab after sema)
            // or if found but not a function type
            if (!is_lambda_var) {
                if (!fn_sym) {
                    // Not in global scope → must be a parameter or local variable
                    is_lambda_var = true;
                } else if (fn_sym->type && fn_sym->type->kind != TYPE_FUNC) {
                    is_lambda_var = true;
                }
            }

            bool shape_return = expr->resolved_type &&
                expr->resolved_type->kind == TYPE_SHAPE;
            const char *ret_sig = shape_return ? NULL
                : qbe_sig_type(expr->resolved_type, emit->arena);
            bool has_return = !(expr->resolved_type &&
                expr->resolved_type->kind == TYPE_VOID) && !shape_return;
            int ret_slot = -1;
            if (shape_return) {
                ret_slot = emit_shape_temp(emit, expr->resolved_type, false);
            }

            // Pre-convert args that need type coercion (int→float, float64→float32, etc.)
            for (int i = 0; i < expr->call.arg_count; i++) {
                if (fn_type && i < fn_type->func.param_count &&
                    fn_type->func.param_mutable &&
                    fn_type->func.param_mutable[i]) {
                    continue;
                }
                MixType *param_type = (fn_type && i < fn_type->func.param_count)
                    ? fn_type->func.param_types[i]
                    : expr->call.args[i]->resolved_type;
                MixType *arg_type = expr->call.args[i]->resolved_type;
                arg_temps[i] = coerce_to_field_type(emit, arg_temps[i], arg_type, param_type);
            }

            if (is_lambda_var) {
                // Indirect call through variable holding function pointer
                int fptr = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l copy %%v.%s\n",
                        fptr, qbe_lookup_local_name(emit, expr->call.name));
                if (has_return) {
                    fprintf(emit->out, "\t%%t%d =%s call %%t%d(", t, ret_sig, fptr);
                } else if (shape_return) {
                    fprintf(emit->out, "\tcall %%t%d(l %%t%d", fptr, ret_slot);
                } else {
                    fprintf(emit->out, "\tcall %%t%d(", fptr);
                }
            } else if (fn_sym && fn_sym->c_name) {
                // Indirect call through function pointer global (e.g. GLAD)
                // Load the function pointer: %fptr =l loadl $glad_glClear
                int fptr = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl $%s\n", fptr, fn_sym->c_name);
                if (has_return) {
                    fprintf(emit->out, "\t%%t%d =%s call %%t%d(", t, ret_sig, fptr);
                } else if (shape_return) {
                    fprintf(emit->out, "\tcall %%t%d(l %%t%d", fptr, ret_slot);
                } else {
                    fprintf(emit->out, "\tcall %%t%d(", fptr);
                }
            } else if (has_return) {
                fprintf(emit->out, "\t%%t%d =%s call $%s(", t, ret_sig, expr->call.name);
            } else if (shape_return) {
                fprintf(emit->out, "\tcall $%s(l %%t%d", expr->call.name, ret_slot);
            } else {
                fprintf(emit->out, "\tcall $%s(", expr->call.name);
            }

            int arg_base = 0;
            if (shape_return) {
                arg_base = 1;
            }
            for (int i = 0; i < expr->call.arg_count; i++) {
                if (i + arg_base > 0) fprintf(emit->out, ", ");
                MixType *param_type = (fn_type && i < fn_type->func.param_count)
                    ? fn_type->func.param_types[i]
                    : expr->call.args[i]->resolved_type;
                bool param_mutable = fn_type && i < fn_type->func.param_count &&
                    fn_type->func.param_mutable &&
                    fn_type->func.param_mutable[i];
                const char *aty = qbe_param_sig_type(param_type, param_mutable,
                                                     emit->arena);
                fprintf(emit->out, "%s %%t%d", aty, arg_temps[i]);
            }
            fprintf(emit->out, ")\n");

            return shape_return ? ret_slot : t;
        }
        case NODE_METHOD_CALL: {
            // obj.method(args) → ShapeName_method(obj, args)
            int obj_temp = emit_expr(emit, expr->method_call.object);
            MixType *obj_type = ref_base_type(expr->method_call.object->resolved_type);
            const char *shape_name = (obj_type && obj_type->kind == TYPE_SHAPE)
                ? obj_type->shape.name : "Unknown";
            char mangled[256];
            mangled[0] = '\0';
            MixType *mtype = NULL;
            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                snprintf(mangled, sizeof(mangled), "%s_%s",
                         shape_name, expr->method_call.method_name);
                Symbol *msym = symtab_lookup(emit->symtab, mangled);
                if (msym && msym->type && msym->type->kind == TYPE_FUNC) {
                    mtype = msym->type;
                }
            }
            int arg_temps2[64];
            for (int i = 0; i < expr->method_call.arg_count; i++) {
                MixType *param_type = (mtype && i + 1 < mtype->func.param_count)
                    ? mtype->func.param_types[i + 1]
                    : expr->method_call.args[i]->resolved_type;
                bool param_mutable = mtype && i + 1 < mtype->func.param_count &&
                    mtype->func.param_mutable &&
                    mtype->func.param_mutable[i + 1];
                if (param_mutable) {
                    arg_temps2[i] = emit_mutable_arg_address(
                        emit, expr->method_call.args[i], param_type);
                } else {
                    arg_temps2[i] = emit_expr(emit, expr->method_call.args[i]);
                }
            }

            // Indirect call through a fn-typed field: load the field, then
            // call it with just the user args (no `self`). Sema sets
            // is_field_call when no real method exists by that name but a
            // fn-typed field does.
            if (expr->method_call.is_field_call
                && obj_type && obj_type->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi = type_find_field(obj_type, expr->method_call.method_name);
                if (fi) {
                    int faddr = obj_temp;
                    if (fi->offset != 0) {
                        faddr = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n",
                                faddr, obj_temp, fi->offset);
                    }
                    int fptr = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l loadl %%t%d\n", fptr, faddr);
                    bool shape_return = expr->resolved_type &&
                        expr->resolved_type->kind == TYPE_SHAPE;
                    int t = shape_return ? emit_shape_temp(emit, expr->resolved_type, false)
                                         : next_temp(emit);
                    const char *ret_sig = shape_return ? NULL
                        : qbe_sig_type(expr->resolved_type, emit->arena);
                    bool has_return = !(expr->resolved_type
                            && expr->resolved_type->kind == TYPE_VOID) && !shape_return;
                    if (has_return) {
                        fprintf(emit->out, "\t%%t%d =%s call %%t%d(", t, ret_sig, fptr);
                    } else if (shape_return) {
                        fprintf(emit->out, "\tcall %%t%d(l %%t%d", fptr, t);
                    } else {
                        fprintf(emit->out, "\tcall %%t%d(", fptr);
                    }
                    for (int i = 0; i < expr->method_call.arg_count; i++) {
                        if (i > 0 || shape_return) fprintf(emit->out, ", ");
                        const char *aty = qbe_sig_type(
                            expr->method_call.args[i]->resolved_type, emit->arena);
                        fprintf(emit->out, "%s %%t%d", aty, arg_temps2[i]);
                    }
                    fprintf(emit->out, ")\n");
                    return t;
                }
            }

            // Shared built-in methods: .read(), .update!(fn)
            if (obj_type && obj_type->kind == TYPE_SHARED) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "read") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_shared_read(l %%t%d)\n", t, obj_temp);
                } else if (strcmp(m, "update") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\tcall $mix_shared_update(l %%t%d, l %%t%d)\n",
                            obj_temp, arg_temps2[0]);
                }
                return t;
            }

            // List built-in methods: push!(val), pop!(), remove!(idx), insert!(idx, val), sort!(), reverse!(), contains(val), index_of(val), join(sep)
            if (obj_type && obj_type->kind == TYPE_LIST) {
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "push") == 0 && expr->method_call.arg_count == 1) {
                    int t = next_temp(emit);
                    MixType *elem = obj_type->list.elem_type;
                    if (elem && elem->kind == TYPE_SHAPE) {
                        fprintf(emit->out, "\tcall $mix_list_push_bytes(l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0]);
                    } else {
                        int push_val = arg_temps2[0];
                        if (elem && type_is_float(elem)) {
                            int cast_t = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", cast_t, push_val);
                            push_val = cast_t;
                        }
                        fprintf(emit->out, "\tcall $mix_list_push(l %%t%d, l %%t%d)\n",
                                obj_temp, push_val);
                    }
                    return t;
                } else if (strcmp(m, "pop") == 0 && expr->method_call.arg_count == 0) {
                    MixType *elem = (obj_type && obj_type->kind == TYPE_LIST)
                        ? obj_type->list.elem_type : NULL;
                    if (elem && elem->kind == TYPE_SHAPE) {
                        int ret_slot = emit_shape_temp(emit, elem, false);
                        fprintf(emit->out, "\tcall $mix_list_pop_bytes(l %%t%d, l %%t%d)\n",
                                obj_temp, ret_slot);
                        return ret_slot;
                    }
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_pop(l %%t%d)\n", t, obj_temp);
                    if (elem && type_is_float(elem)) {
                        int cast_t = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =d cast %%t%d\n", cast_t, t);
                        t = cast_t;
                    }
                    return t;
                } else if ((strcmp(m, "at") == 0 || strcmp(m, "at_mut") == 0) &&
                           expr->method_call.arg_count == 1) {
                    MixType *elem = obj_type->list.elem_type;
                    int idx = arg_temps2[0];
                    idx = widen_int_to_long(
                        emit, idx, expr->method_call.args[0]->resolved_type);
                    bool want_ref = expr->resolved_type &&
                        expr->resolved_type->kind == TYPE_REF;
                    if (elem && elem->kind == TYPE_SHAPE) {
                        int ptr = next_temp(emit);
                        fprintf(emit->out,
                                "\t%%t%d =l call $mix_list_ptr(l %%t%d, l %%t%d)\n",
                                ptr, obj_temp, idx);
                        if (want_ref) return ptr;
                        int tmp = emit_shape_temp(emit, elem, false);
                        emit_shape_copy(emit, tmp, ptr, elem);
                        return tmp;
                    }
                    int t = next_temp(emit);
                    if (want_ref && !ref_uses_direct_value(elem)) {
                        fprintf(emit->out,
                                "\t%%t%d =l call $mix_list_ptr(l %%t%d, l %%t%d)\n",
                                t, obj_temp, idx);
                    } else {
                        fprintf(emit->out,
                                "\t%%t%d =l call $mix_list_get(l %%t%d, l %%t%d)\n",
                                t, obj_temp, idx);
                        if (elem && type_is_float(elem)) {
                            int dt = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =d cast %%t%d\n", dt, t);
                            t = dt;
                        }
                    }
                    return t;
                } else if (strcmp(m, "remove") == 0 && expr->method_call.arg_count == 1) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\tcall $mix_list_remove(l %%t%d, l %%t%d)\n",
                            obj_temp, arg_temps2[0]);
                    return t;
                } else if (strcmp(m, "insert") == 0 && expr->method_call.arg_count == 2) {
                    int t = next_temp(emit);
                    MixType *elem = obj_type->list.elem_type;
                    if (elem && elem->kind == TYPE_SHAPE) {
                        fprintf(emit->out,
                                "\tcall $mix_list_insert_bytes(l %%t%d, l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0], arg_temps2[1]);
                    } else {
                        fprintf(emit->out, "\tcall $mix_list_insert(l %%t%d, l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0], arg_temps2[1]);
                    }
                    return t;
                } else if (strcmp(m, "sort") == 0 && expr->method_call.arg_count == 0) {
                    int t = next_temp(emit);
                    MixType *elem = obj_type->list.elem_type;
                    if (elem && elem->kind == TYPE_STR)
                        fprintf(emit->out, "\tcall $mix_list_sort_str(l %%t%d)\n", obj_temp);
                    else if (elem && type_is_float(elem))
                        fprintf(emit->out, "\tcall $mix_list_sort_float(l %%t%d)\n", obj_temp);
                    else
                        fprintf(emit->out, "\tcall $mix_list_sort(l %%t%d)\n", obj_temp);
                    return t;
                } else if (strcmp(m, "reverse") == 0 && expr->method_call.arg_count == 0) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\tcall $mix_list_reverse(l %%t%d)\n", obj_temp);
                    return t;
                } else if (strcmp(m, "contains") == 0 && expr->method_call.arg_count == 1) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =w call $mix_list_contains(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                    return t;
                } else if (strcmp(m, "index_of") == 0 && expr->method_call.arg_count == 1) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_index_of(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                    return t;
                } else if (strcmp(m, "join") == 0 && expr->method_call.arg_count == 1) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_join(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                    return t;
                }
            }

            // String built-in methods
            if (obj_type && obj_type->kind == TYPE_STR) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "upper") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_upper(l %%t%d)\n", t, obj_temp);
                } else if (strcmp(m, "lower") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_lower(l %%t%d)\n", t, obj_temp);
                } else if (strcmp(m, "trim") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_trim(l %%t%d)\n", t, obj_temp);
                } else if (strcmp(m, "split") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_split(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "contains") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =w call $mix_str_contains(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "starts_with") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =w call $mix_str_starts_with(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "replace") == 0 && expr->method_call.arg_count == 2) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_replace(l %%t%d, l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0], arg_temps2[1]);
                } else if (strcmp(m, "ends_with") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =w call $mix_str_ends_with(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "char_at") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_char_at(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "slice") == 0 && expr->method_call.arg_count == 2) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_slice(l %%t%d, l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0], arg_temps2[1]);
                } else if (strcmp(m, "repeat") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_repeat(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "reverse") == 0 && expr->method_call.arg_count == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_reverse(l %%t%d)\n",
                            t, obj_temp);
                } else if (strcmp(m, "index_of") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_index_of(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "code") == 0 && expr->method_call.arg_count == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_ord(l %%t%d)\n",
                            t, obj_temp);
                } else if (strcmp(m, "sort") == 0 && expr->method_call.arg_count == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_sort(l %%t%d)\n",
                            t, obj_temp);
                }
                return t;
            }

            // Map built-in methods
            if (obj_type && obj_type->kind == TYPE_MAP) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "has") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =w call $mix_map_has(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "remove") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\tcall $mix_map_remove(l %%t%d, l %%t%d)\n",
                            obj_temp, arg_temps2[0]);
                }
                return t;
            }

            // Set built-in methods
            if (obj_type && obj_type->kind == TYPE_SET) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                MixType *selem = obj_type->set.elem_type;
                bool is_int_set = selem && type_is_integer(selem);
                if (strcmp(m, "has") == 0 && expr->method_call.arg_count == 1) {
                    if (is_int_set) {
                        fprintf(emit->out, "\t%%t%d =w call $mix_set_has_int(l %%t%d, l %%t%d)\n",
                                t, obj_temp, arg_temps2[0]);
                    } else {
                        fprintf(emit->out, "\t%%t%d =w call $mix_set_has(l %%t%d, l %%t%d)\n",
                                t, obj_temp, arg_temps2[0]);
                    }
                } else if (strcmp(m, "add") == 0 && expr->method_call.arg_count == 1) {
                    if (is_int_set) {
                        fprintf(emit->out, "\tcall $mix_set_add_int(l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0]);
                    } else {
                        fprintf(emit->out, "\tcall $mix_set_add(l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0]);
                    }
                } else if (strcmp(m, "remove") == 0 && expr->method_call.arg_count == 1) {
                    if (is_int_set) {
                        fprintf(emit->out, "\tcall $mix_set_remove_int(l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0]);
                    } else {
                        fprintf(emit->out, "\tcall $mix_set_remove(l %%t%d, l %%t%d)\n",
                                obj_temp, arg_temps2[0]);
                    }
                } else if (strcmp(m, "union") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_union(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "intersect") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_intersect(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "diff") == 0 && expr->method_call.arg_count == 1) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_diff(l %%t%d, l %%t%d)\n",
                            t, obj_temp, arg_temps2[0]);
                }
                return t;
            }

            // Built-in Project shape: build() method
            if (obj_type && obj_type->kind == TYPE_SHAPE &&
                strcmp(obj_type->shape.name, "Project") == 0) {
                if (strcmp(expr->method_call.method_name, "build") == 0) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\tcall $mix_project_build(l %%t%d)\n", obj_temp);
                    return t;
                }
            }

            bool shape_return = expr->resolved_type &&
                expr->resolved_type->kind == TYPE_SHAPE;
            int t = shape_return ? emit_shape_temp(emit, expr->resolved_type, false)
                                 : next_temp(emit);
            const char *ret_sig = shape_return ? NULL
                : qbe_sig_type(expr->resolved_type, emit->arena);
            bool has_return = !(expr->resolved_type &&
                expr->resolved_type->kind == TYPE_VOID) && !shape_return;

            if (has_return) {
                fprintf(emit->out, "\t%%t%d =%s call $%s(", t, ret_sig, mangled);
                fprintf(emit->out, "l %%t%d", obj_temp);
            } else if (shape_return) {
                fprintf(emit->out, "\tcall $%s(l %%t%d, l %%t%d",
                        mangled, t, obj_temp);
            } else {
                fprintf(emit->out, "\tcall $%s(", mangled);
                fprintf(emit->out, "l %%t%d", obj_temp);
            }

            // Remaining args
            for (int i = 0; i < expr->method_call.arg_count; i++) {
                fprintf(emit->out, ", ");
                MixType *pt = (mtype && i + 1 < mtype->func.param_count)
                    ? mtype->func.param_types[i + 1]
                    : expr->method_call.args[i]->resolved_type;
                bool param_mutable = mtype && i + 1 < mtype->func.param_count &&
                    mtype->func.param_mutable &&
                    mtype->func.param_mutable[i + 1];
                const char *aty = qbe_param_sig_type(pt, param_mutable,
                                                     emit->arena);
                fprintf(emit->out, "%s %%t%d", aty, arg_temps2[i]);
            }
            fprintf(emit->out, ")\n");
            return t;
        }
        case NODE_SHAPE_LIT: {
            MixType *stype = expr->resolved_type;
            int t = emit_shape_temp(emit, stype, true);

            if (stype && stype->kind == TYPE_SHAPE && stype->shape.is_tagged_union) {
                // Tagged union construction: store tag + variant fields
                ShapeVariant *sv = type_find_variant(stype, expr->shape_lit.shape_name);
                if (sv) {
                    // Store tag at offset 0
                    fprintf(emit->out, "\tstorel %d, %%t%d\n", sv->tag, t);
                    // Store variant fields at offset 8+
                    for (int i = 0; i < expr->shape_lit.field_count && i < sv->field_count; i++) {
                        int val = emit_expr(emit, expr->shape_lit.field_values[i]);
                        val = coerce_to_field_type(emit, val,
                                expr->shape_lit.field_values[i]->resolved_type,
                                sv->fields[i].type);
                        const char *fty = type_to_qbe_mem(sv->fields[i].type);
                        int foff = 8 + sv->fields[i].offset;
                        int addr = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, t, foff);
                        fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", fty, val, addr);
                    }
                }
            } else if (stype && stype->kind == TYPE_SHAPE) {
                // Regular struct construction
                for (int i = 0; i < expr->shape_lit.field_count; i++) {
                    ShapeFieldInfo *fi = type_find_field(stype, expr->shape_lit.field_names[i]);
                    if (!fi) continue;
                    int val = emit_expr(emit, expr->shape_lit.field_values[i]);

                    // Inline sub-struct: memcpy the sub-shape's data into parent.
                    if (fi->type && fi->type->kind == TYPE_SHAPE) {
                        int dst_addr = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", dst_addr, t, fi->offset);
                        emit_shape_copy(emit, dst_addr, val, fi->type);
                    } else {
                        val = coerce_to_field_type(emit, val,
                                expr->shape_lit.field_values[i]->resolved_type, fi->type);
                        const char *mem_ty = type_to_qbe_mem(fi->type);
                        if (fi->offset == 0) {
                            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, val, t);
                        } else {
                            int addr = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, t, fi->offset);
                            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, val, addr);
                        }
                    }
                }
            }
            return t;
        }
        case NODE_FIELD_EXPR: {
            // Get the object pointer
            int obj = emit_expr(emit, expr->field_expr.object);
            MixType *obj_type = ref_base_type(expr->field_expr.object->resolved_type);

            // List built-in fields
            if (obj_type && obj_type->kind == TYPE_LIST) {
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_len(l %%t%d)\n", t, obj);
                    return t;
                }
            }

            // String built-in fields
            if (obj_type && obj_type->kind == TYPE_STR) {
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    int t = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_str_len(l %%t%d)\n", t, obj);
                    return t;
                }
            }

            // Map built-in fields
            if (obj_type && obj_type->kind == TYPE_MAP) {
                const char *fn = expr->field_expr.field_name;
                int t = next_temp(emit);
                if (strcmp(fn, "len") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_map_len(l %%t%d)\n", t, obj);
                } else if (strcmp(fn, "keys") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_map_keys(l %%t%d)\n", t, obj);
                } else if (strcmp(fn, "values") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_map_values(l %%t%d)\n", t, obj);
                }
                return t;
            }

            // Set built-in fields
            if (obj_type && obj_type->kind == TYPE_SET) {
                const char *fn = expr->field_expr.field_name;
                int t = next_temp(emit);
                MixType *selem = obj_type->set.elem_type;
                bool is_int_set = selem && type_is_integer(selem);
                if (strcmp(fn, "len") == 0) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_len(l %%t%d)\n", t, obj);
                } else if (strcmp(fn, "values") == 0) {
                    if (is_int_set) {
                        fprintf(emit->out, "\t%%t%d =l call $mix_set_values_int(l %%t%d)\n", t, obj);
                    } else {
                        fprintf(emit->out, "\t%%t%d =l call $mix_set_values(l %%t%d)\n", t, obj);
                    }
                }
                return t;
            }

            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi = type_find_field(obj_type, expr->field_expr.field_name);
                if (fi) {
                    // Shape-typed field: return address of inline data (not load)
                    if (fi->type && fi->type->kind == TYPE_SHAPE) {
                        int t = next_temp(emit);
                        if (fi->offset == 0) {
                            fprintf(emit->out, "\t%%t%d =l copy %%t%d\n", t, obj);
                        } else {
                            fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", t, obj, fi->offset);
                        }
                        return t;
                    }
                    const char *reg_ty = type_to_qbe(fi->type);
                    const char *load_ty = type_to_qbe_load(fi->type);
                    int t = next_temp(emit);
                    if (fi->offset == 0) {
                        fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n", t, reg_ty, load_ty, obj);
                    } else {
                        int addr = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, obj, fi->offset);
                        fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n", t, reg_ty, load_ty, addr);
                    }
                    return t;
                }
                // Try computed field (zero-param method): obj.area → obj.area()
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                         obj_type->shape.name, expr->field_expr.field_name);
                Symbol *msym = symtab_lookup(emit->symtab, mangled);
                if (msym) {
                    bool shape_return = expr->resolved_type &&
                        expr->resolved_type->kind == TYPE_SHAPE;
                    int t = shape_return
                        ? emit_shape_temp(emit, expr->resolved_type, false)
                        : next_temp(emit);
                    const char *ret_sig = shape_return ? NULL
                        : qbe_sig_type(expr->resolved_type, emit->arena);
                    const char *shape_ty = qbe_sig_type(obj_type, emit->arena);
                    if (shape_return) {
                        fprintf(emit->out, "\tcall $%s(l %%t%d, %s %%t%d)\n",
                                mangled, t, shape_ty, obj);
                    } else {
                        fprintf(emit->out, "\t%%t%d =%s call $%s(%s %%t%d)\n",
                                t, ret_sig, mangled, shape_ty, obj);
                    }
                    return t;
                }
            }
            // Fallback
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
            return t;
        }
        case NODE_SHARED_EXPR: {
            int init_val = emit_expr(emit, expr->shared_expr.init_expr);
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_shared_new(l %%t%d)\n", t, init_val);
            return t;
        }
        case NODE_GO_EXPR: {
            // go compute(5) → pack fn ptr + args, call mix_task_spawn
            AstNode *call = expr->go_expr.call_expr;
            if (call->kind != NODE_CALL_EXPR) {
                mix_error(expr->loc, "go requires a function call");
                return next_temp(emit);
            }
            // Get function pointer
            int fn_ptr = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l copy $%s\n", fn_ptr, call->call.name);
            // Evaluate args
            int arg_count = call->call.arg_count;
            int args_array = next_temp(emit);
            if (arg_count > 0) {
                fprintf(emit->out, "\t%%t%d =l call $mix_alloc(l %d)\n", args_array, arg_count * 8);
                for (int i = 0; i < arg_count; i++) {
                    int av = emit_expr(emit, call->call.args[i]);
                    if (i == 0) {
                        fprintf(emit->out, "\tstorel %%t%d, %%t%d\n", av, args_array);
                    } else {
                        int addr = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, args_array, i * 8);
                        fprintf(emit->out, "\tstorel %%t%d, %%t%d\n", av, addr);
                    }
                }
            } else {
                fprintf(emit->out, "\t%%t%d =l copy 0\n", args_array);
            }
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_task_spawn(l %%t%d, l %%t%d, l %d)\n",
                    t, fn_ptr, args_array, arg_count);
            return t;
        }
        case NODE_WAIT_EXPR: {
            int handle = emit_expr(emit, expr->wait_expr.handle_expr);
            int t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_task_wait(l %%t%d)\n", t, handle);
            return t;
        }
        default:
            mix_error(expr->loc, "unsupported expression node (kind %d) in codegen", expr->kind);
            return next_temp(emit);
    }
}

// --- Statement emission ---

static void emit_stmt(QbeEmitter *emit, AstNode *stmt);

static void emit_block(QbeEmitter *emit, AstNode *block) {
    if (!block || block->kind != NODE_BLOCK) return;
    qbe_push_local_scope(emit);
    for (int i = 0; i < block->block.stmt_count; i++) {
        emit_stmt(emit, block->block.stmts[i]);
    }
    qbe_pop_local_scope(emit);
}

static void emit_stmt(QbeEmitter *emit, AstNode *stmt) {
    if (!stmt) return;

    emit_dbgloc(emit, stmt->loc);

    switch (stmt->kind) {
        case NODE_VAR_DECL: {
            // Track lambda variables for indirect call detection
            if (stmt->var_decl.init_expr && stmt->var_decl.init_expr->kind == NODE_LAMBDA) {
                if (emit->fn_ptr_var_count < 128) {
                    emit->fn_ptr_vars[emit->fn_ptr_var_count++] =
                        arena_strdup(emit->arena, stmt->var_decl.name);
                }
            }

            const char *ty = qbe_type(stmt->resolved_type);
            const char *slot_name = qbe_var_decl_name(emit, stmt);

            if (stmt->resolved_type && stmt->resolved_type->kind == TYPE_SHAPE) {
                int init = emit_expr(emit, stmt->var_decl.init_expr);
                fprintf(emit->out, "\t%%v.%s =l alloc8 %d\n",
                        slot_name, stmt->resolved_type->shape.total_size);
                fprintf(emit->out, "\tcall $memcpy(l %%v.%s, l %%t%d, l %d)\n",
                        slot_name, init, stmt->resolved_type->shape.total_size);
            } else if (stmt->var_decl.is_mutable) {
                int init = emit_expr(emit, stmt->var_decl.init_expr);
                // Allocate stack space for mutable variable
                int size = 8; // default to 8 bytes for most types
                if (stmt->resolved_type) {
                    switch (stmt->resolved_type->kind) {
                        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: case TYPE_BOOL: size = 4; break;
                        case TYPE_INT16: case TYPE_UINT16: size = 2; break;
                        case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: size = 1; break;
                        default: size = 8; break;
                    }
                }
                fprintf(emit->out, "\t%%v.%s =l alloc%d %d\n",
                        slot_name, size >= 4 ? size : 4, size);
                init = coerce_to_field_type(emit, init,
                        stmt->var_decl.init_expr->resolved_type, stmt->resolved_type);
                fprintf(emit->out, "\tstore%s %%t%d, %%v.%s\n", ty, init, slot_name);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_INT_LIT) {
                // Immutable int literal: emit value directly, skip intermediate temp
                fprintf(emit->out, "\t%%v.%s =%s copy %" PRId64 "\n",
                        slot_name, ty, stmt->var_decl.init_expr->int_lit.value);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_FLOAT_LIT) {
                fprintf(emit->out, "\t%%v.%s =%s copy d_%a\n",
                        slot_name, ty, stmt->var_decl.init_expr->float_lit.value);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_BOOL_LIT) {
                fprintf(emit->out, "\t%%v.%s =%s copy %d\n",
                        slot_name, ty, stmt->var_decl.init_expr->bool_lit.value ? 1 : 0);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_STRING_LIT) {
                const char *sname = emit_string_data(emit,
                    stmt->var_decl.init_expr->string_lit.value,
                    stmt->var_decl.init_expr->string_lit.length);
                fprintf(emit->out, "\t%%v.%s =l copy %s\n", slot_name, sname);
            } else {
                // Immutable: SSA copy
                int init = emit_expr(emit, stmt->var_decl.init_expr);
                fprintf(emit->out, "\t%%v.%s =%s copy %%t%d\n",
                        slot_name, ty, init);
            }
            qbe_bind_local_name(emit, stmt->var_decl.name, slot_name);
            break;
        }
        case NODE_ASSIGN: {
            int val = emit_expr(emit, stmt->assign.value);
            // Determine the target variable's type for proper coercion
            Symbol *assign_sym = symtab_lookup(emit->symtab, stmt->assign.name);
            MixType *var_type = assign_sym ? assign_sym->type : NULL;
            MixType *val_type = stmt->assign.value->resolved_type;
            if (var_type && var_type->kind == TYPE_SHAPE) {
                bool is_global = !qbe_has_local_binding(emit, stmt->assign.name) &&
                                 assign_sym && assign_sym->is_global;
                const char *slot_name = is_global
                    ? stmt->assign.name
                    : qbe_lookup_local_name(emit, stmt->assign.name);
                if (stmt->assign.op != TOK_EQ) {
                    mix_error(stmt->loc, "compound assignment is not supported on shape values");
                    break;
                }
                if (is_global) {
                    fprintf(emit->out, "\tcall $memcpy(l $g_%s, l %%t%d, l %d)\n",
                            slot_name, val, var_type->shape.total_size);
                } else {
                    fprintf(emit->out, "\tcall $memcpy(l %%v.%s, l %%t%d, l %d)\n",
                            slot_name, val, var_type->shape.total_size);
                }
                break;
            }
            const char *ty = qbe_type(var_type ? var_type : val_type);
            const char *mem_ty = type_to_qbe_mem(var_type ? var_type : val_type);
            const char *load_ty = type_to_qbe_load(var_type ? var_type : val_type);
            val = coerce_to_field_type(emit, val, val_type, var_type);
            bool is_global = !qbe_has_local_binding(emit, stmt->assign.name) &&
                             assign_sym && assign_sym->is_global;
            const char *slot_name = is_global
                ? stmt->assign.name
                : qbe_lookup_local_name(emit, stmt->assign.name);
            const char *slot_prefix = is_global ? "$g_" : "%v.";

            if (stmt->assign.op == TOK_EQ) {
                fprintf(emit->out, "\tstore%s %%t%d, %s%s\n",
                        mem_ty, val, slot_prefix, slot_name);
            } else {
                // Compound assignment: load, op, store
                int old = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =%s load%s %s%s\n",
                        old, ty, load_ty, slot_prefix, slot_name);
                int result = next_temp(emit);
                const char *op;
                switch (stmt->assign.op) {
                    case TOK_PLUS_EQ:  op = "add"; break;
                    case TOK_MINUS_EQ: op = "sub"; break;
                    case TOK_STAR_EQ:  op = "mul"; break;
                    case TOK_SLASH_EQ: op = "div"; break;
                    default: op = "add"; break;
                }
                fprintf(emit->out, "\t%%t%d =%s %s %%t%d, %%t%d\n", result, ty, op, old, val);
                fprintf(emit->out, "\tstore%s %%t%d, %s%s\n",
                        mem_ty, result, slot_prefix, slot_name);
            }
            break;
        }
        case NODE_IF_STMT: {
            int cond = emit_expr(emit, stmt->if_stmt.condition);
            int l_then = next_label(emit);
            int l_else = next_label(emit);
            int l_end = next_label(emit);

            if (stmt->if_stmt.else_block) {
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cond, l_then, l_else);
            } else {
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cond, l_then, l_end);
            }

            fprintf(emit->out, "@L%d\n", l_then);
            emit_block(emit, stmt->if_stmt.then_block);
            fprintf(emit->out, "\tjmp @L%d\n", l_end);

            if (stmt->if_stmt.else_block) {
                fprintf(emit->out, "@L%d\n", l_else);
                if (stmt->if_stmt.else_block->kind == NODE_BLOCK) {
                    emit_block(emit, stmt->if_stmt.else_block);
                } else {
                    qbe_push_local_scope(emit);
                    emit_stmt(emit, stmt->if_stmt.else_block);
                    qbe_pop_local_scope(emit);
                }
                fprintf(emit->out, "\tjmp @L%d\n", l_end);
            }

            fprintf(emit->out, "@L%d\n", l_end);
            break;
        }
        case NODE_WHILE_STMT: {
            int l_cond = next_label(emit);
            int l_body = next_label(emit);
            int l_end = next_label(emit);

            // Push loop labels for break/continue
            if (emit->loop_depth >= 32) {
                mix_error(stmt->loc, "too many nested loops (max 32)");
                break;
            }
            emit->break_labels[emit->loop_depth] = l_end;
            emit->continue_labels[emit->loop_depth] = l_cond;
            emit->loop_depth++;

            fprintf(emit->out, "@L%d\n", l_cond);
            int cond = emit_expr(emit, stmt->while_stmt.condition);
            fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cond, l_body, l_end);

            fprintf(emit->out, "@L%d\n", l_body);
            emit_block(emit, stmt->while_stmt.body);
            fprintf(emit->out, "\tjmp @L%d\n", l_cond);

            fprintf(emit->out, "@L%d\n", l_end);
            emit->loop_depth--;
            break;
        }
        case NODE_FOR_STMT: {
            AstNode *iter = stmt->for_stmt.iterable;
            MixType *iter_type = iter ? iter->resolved_type : NULL;
            bool is_range = iter && iter->kind == NODE_BINARY_EXPR &&
                (iter->binary.op == TOK_DOTDOT || iter->binary.op == TOK_DOTDOT_EQ);
            bool is_list = iter_type && iter_type->kind == TYPE_LIST;
            bool is_map = iter_type && iter_type->kind == TYPE_MAP;
            bool is_set = iter_type && iter_type->kind == TYPE_SET;

            if (is_map) {
                // for key, value in map → get keys, iterate keys, get value for each
                int map_ptr = emit_expr(emit, iter);
                int keys_list = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_map_keys(l %%t%d)\n", keys_list, map_ptr);
                int len_t = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_list_len(l %%t%d)\n", len_t, keys_list);

                // Internal index
                int idx_id = next_label(emit);
                char idx_name[64];
                snprintf(idx_name, sizeof(idx_name), "_midx_%d", idx_id);
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", idx_name);
                fprintf(emit->out, "\tstorel 0, %%v.%s\n", idx_name);

                int l_cond = next_label(emit);
                int l_body = next_label(emit);
                int l_end2 = next_label(emit);
                int l_inc = next_label(emit);

                // Push loop labels for break/continue
                if (emit->loop_depth >= 32) {
                    mix_error(stmt->loc, "too many nested loops (max 32)");
                    break;
                }
                qbe_push_local_scope(emit);
                const char *index_slot_name = NULL;
                if (stmt->for_stmt.index_name) {
                    index_slot_name = qbe_for_binding_name(
                        emit, stmt, stmt->for_stmt.index_name, "for_idx");
                    fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", index_slot_name);
                    fprintf(emit->out, "\tstorel 0, %%v.%s\n", index_slot_name);
                    qbe_bind_local_name(emit, stmt->for_stmt.index_name, index_slot_name);
                }
                const char *var_slot_name = qbe_for_binding_name(
                    emit, stmt, stmt->for_stmt.var_name, "for_var");
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", var_slot_name);
                fprintf(emit->out, "\tstorel 0, %%v.%s\n", var_slot_name);
                qbe_bind_local_name(emit, stmt->for_stmt.var_name, var_slot_name);
                emit->break_labels[emit->loop_depth] = l_end2;
                emit->continue_labels[emit->loop_depth] = l_inc;
                emit->loop_depth++;

                fprintf(emit->out, "@L%d\n", l_cond);
                int ci = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci, idx_name);
                int cmp = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =w csltl %%t%d, %%t%d\n", cmp, ci, len_t);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cmp, l_body, l_end2);

                fprintf(emit->out, "@L%d\n", l_body);
                int ci2 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci2, idx_name);
                // Get key from keys list
                int key = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_list_get(l %%t%d, l %%t%d)\n", key, keys_list, ci2);
                if (index_slot_name) {
                    fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", key, index_slot_name);
                }
                // Get value from map using key
                int val = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_map_get(l %%t%d, l %%t%d)\n", val, map_ptr, key);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", val, var_slot_name);

                emit_block(emit, stmt->for_stmt.body);

                fprintf(emit->out, "@L%d\n", l_inc);
                int ci3 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci3, idx_name);
                int ni = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l add %%t%d, 1\n", ni, ci3);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", ni, idx_name);
                fprintf(emit->out, "\tjmp @L%d\n", l_cond);
                fprintf(emit->out, "@L%d\n", l_end2);
                emit->loop_depth--;
                qbe_pop_local_scope(emit);
            } else if (is_list) {
                // for item in list → index loop with element load
                int list_ptr = emit_expr(emit, iter);
                int len_t = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_list_len(l %%t%d)\n", len_t, list_ptr);
                MixType *elem_type = iter_type->list.elem_type;

                // Internal index
                int idx_id = next_label(emit);
                char idx_name[64];
                snprintf(idx_name, sizeof(idx_name), "_idx_%d", idx_id);
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", idx_name);
                fprintf(emit->out, "\tstorel 0, %%v.%s\n", idx_name);

                int l_cond = next_label(emit);
                int l_body = next_label(emit);
                int l_end2 = next_label(emit);
                int l_inc = next_label(emit);

                // Push loop labels for break/continue
                if (emit->loop_depth >= 32) {
                    mix_error(stmt->loc, "too many nested loops (max 32)");
                    break;
                }
                qbe_push_local_scope(emit);
                const char *index_slot_name = NULL;
                if (stmt->for_stmt.index_name) {
                    index_slot_name = qbe_for_binding_name(
                        emit, stmt, stmt->for_stmt.index_name, "for_idx");
                    fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", index_slot_name);
                    fprintf(emit->out, "\tstorel 0, %%v.%s\n", index_slot_name);
                    qbe_bind_local_name(emit, stmt->for_stmt.index_name, index_slot_name);
                }
                const char *var_slot_name = qbe_for_binding_name(
                    emit, stmt, stmt->for_stmt.var_name, "for_var");
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", var_slot_name);
                fprintf(emit->out, "\tstorel 0, %%v.%s\n", var_slot_name);
                qbe_bind_local_name(emit, stmt->for_stmt.var_name, var_slot_name);
                emit->break_labels[emit->loop_depth] = l_end2;
                emit->continue_labels[emit->loop_depth] = l_inc;
                emit->loop_depth++;

                fprintf(emit->out, "@L%d\n", l_cond);
                int ci = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci, idx_name);
                int cmp = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =w csltl %%t%d, %%t%d\n", cmp, ci, len_t);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cmp, l_body, l_end2);

                fprintf(emit->out, "@L%d\n", l_body);
                int ci2 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci2, idx_name);
                if (index_slot_name) {
                    fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", ci2, index_slot_name);
                }
                if (elem_type && elem_type->kind == TYPE_SHAPE) {
                    int elem_ptr = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_ptr(l %%t%d, l %%t%d)\n",
                            elem_ptr, list_ptr, ci2);
                    fprintf(emit->out, "\tcall $memcpy(l %%v.%s, l %%t%d, l %d)\n",
                            var_slot_name, elem_ptr, elem_type->shape.total_size);
                } else {
                    int elem = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_get(l %%t%d, l %%t%d)\n", elem, list_ptr, ci2);
                    fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", elem, var_slot_name);
                }

                emit_block(emit, stmt->for_stmt.body);

                fprintf(emit->out, "@L%d\n", l_inc);
                if (stmt->for_stmt.var_is_mutable) {
                    int write_idx = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", write_idx, idx_name);
                    if (elem_type && elem_type->kind == TYPE_SHAPE) {
                        fprintf(emit->out,
                                "\tcall $mix_list_set_bytes(l %%t%d, l %%t%d, l %%v.%s)\n",
                                list_ptr, write_idx, var_slot_name);
                    } else {
                        int write_val = emit_list_store_value_from_slot(
                            emit, var_slot_name, elem_type);
                        fprintf(emit->out,
                                "\tcall $mix_list_set(l %%t%d, l %%t%d, l %%t%d)\n",
                                list_ptr, write_idx, write_val);
                    }
                }
                int ci3 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci3, idx_name);
                int ni = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l add %%t%d, 1\n", ni, ci3);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", ni, idx_name);
                fprintf(emit->out, "\tjmp @L%d\n", l_cond);
                fprintf(emit->out, "@L%d\n", l_end2);
                emit->loop_depth--;
                qbe_pop_local_scope(emit);
            } else if (is_set) {
                // for item in set → convert to list via mix_set_values, then list iterate
                int set_ptr = emit_expr(emit, iter);
                int list_ptr = next_temp(emit);
                MixType *selem = iter_type->set.elem_type;
                bool is_int_set = selem && type_is_integer(selem);
                if (is_int_set) {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_values_int(l %%t%d)\n", list_ptr, set_ptr);
                } else {
                    fprintf(emit->out, "\t%%t%d =l call $mix_set_values(l %%t%d)\n", list_ptr, set_ptr);
                }
                int len_t = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_list_len(l %%t%d)\n", len_t, list_ptr);

                int idx_id = next_label(emit);
                char idx_name[64];
                snprintf(idx_name, sizeof(idx_name), "_sidx_%d", idx_id);
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", idx_name);
                fprintf(emit->out, "\tstorel 0, %%v.%s\n", idx_name);

                int l_cond = next_label(emit);
                int l_body = next_label(emit);
                int l_end2 = next_label(emit);
                int l_inc = next_label(emit);

                // Push loop labels for break/continue
                if (emit->loop_depth >= 32) {
                    mix_error(stmt->loc, "too many nested loops (max 32)");
                    break;
                }
                qbe_push_local_scope(emit);
                const char *var_slot_name = qbe_for_binding_name(
                    emit, stmt, stmt->for_stmt.var_name, "for_var");
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", var_slot_name);
                fprintf(emit->out, "\tstorel 0, %%v.%s\n", var_slot_name);
                qbe_bind_local_name(emit, stmt->for_stmt.var_name, var_slot_name);
                emit->break_labels[emit->loop_depth] = l_end2;
                emit->continue_labels[emit->loop_depth] = l_inc;
                emit->loop_depth++;

                fprintf(emit->out, "@L%d\n", l_cond);
                int ci = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci, idx_name);
                int cmp = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =w csltl %%t%d, %%t%d\n", cmp, ci, len_t);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cmp, l_body, l_end2);

                fprintf(emit->out, "@L%d\n", l_body);
                int ci2 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci2, idx_name);
                int elem = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_list_get(l %%t%d, l %%t%d)\n", elem, list_ptr, ci2);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", elem, var_slot_name);

                emit_block(emit, stmt->for_stmt.body);

                fprintf(emit->out, "@L%d\n", l_inc);
                int ci3 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", ci3, idx_name);
                int ni = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l add %%t%d, 1\n", ni, ci3);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", ni, idx_name);
                fprintf(emit->out, "\tjmp @L%d\n", l_cond);
                fprintf(emit->out, "@L%d\n", l_end2);
                emit->loop_depth--;
                qbe_pop_local_scope(emit);
            } else {
                // For-range: for i in start..end
                int start_val, end_val;
                bool inclusive = false;
                if (is_range) {
                    start_val = emit_expr(emit, iter->binary.left);
                    end_val = emit_expr(emit, iter->binary.right);
                    inclusive = (iter->binary.op == TOK_DOTDOT_EQ);
                } else {
                    start_val = emit_expr(emit, iter);
                    end_val = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l copy 0\n", end_val);
                }

                int l_cond = next_label(emit);
                int l_body = next_label(emit);
                int l_end2 = next_label(emit);
                int l_inc = next_label(emit);

                // Push loop labels for break/continue
                if (emit->loop_depth >= 32) {
                    mix_error(stmt->loc, "too many nested loops (max 32)");
                    break;
                }
                qbe_push_local_scope(emit);
                const char *var_slot_name = qbe_for_binding_name(
                    emit, stmt, stmt->for_stmt.var_name, "for_var");
                fprintf(emit->out, "\t%%v.%s =l alloc8 8\n", var_slot_name);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", start_val, var_slot_name);
                qbe_bind_local_name(emit, stmt->for_stmt.var_name, var_slot_name);
                emit->break_labels[emit->loop_depth] = l_end2;
                emit->continue_labels[emit->loop_depth] = l_inc;
                emit->loop_depth++;

                fprintf(emit->out, "@L%d\n", l_cond);
                int cur = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", cur, var_slot_name);
                int cmp = next_temp(emit);
                if (inclusive)
                    fprintf(emit->out, "\t%%t%d =w cslel %%t%d, %%t%d\n", cmp, cur, end_val);
                else
                    fprintf(emit->out, "\t%%t%d =w csltl %%t%d, %%t%d\n", cmp, cur, end_val);
                fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cmp, l_body, l_end2);

                fprintf(emit->out, "@L%d\n", l_body);
                emit_block(emit, stmt->for_stmt.body);

                fprintf(emit->out, "@L%d\n", l_inc);
                int cur2 = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%v.%s\n", cur2, var_slot_name);
                int next_v = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l add %%t%d, 1\n", next_v, cur2);
                fprintf(emit->out, "\tstorel %%t%d, %%v.%s\n", next_v, var_slot_name);
                fprintf(emit->out, "\tjmp @L%d\n", l_cond);
                fprintf(emit->out, "@L%d\n", l_end2);
                emit->loop_depth--;
                qbe_pop_local_scope(emit);
            }
            break;
        }
        case NODE_MATCH_STMT: {
            int subject = emit_expr(emit, stmt->match_stmt.subject);
            MixType *subj_type = stmt->match_stmt.subject->resolved_type;
            const char *sty = qbe_type(subj_type);
            bool is_flt = subj_type && type_is_float(subj_type);
            bool is_tagged = subj_type && subj_type->kind == TYPE_SHAPE && subj_type->shape.is_tagged_union;
            bool is_optional = subj_type && subj_type->kind == TYPE_OPTIONAL;
            bool is_result = subj_type && subj_type->kind == TYPE_RESULT;
            int l_end3 = next_label(emit);

            // Only thread results when match has a resolved type (used as expression)
            MixType *match_type = stmt->resolved_type;
            bool has_result = match_type && match_type->kind != TYPE_VOID;
            int match_result = -1;
            if (has_result) {
                match_result = next_temp(emit);
                const char *rty = qbe_type(match_type);
                fprintf(emit->out, "\t%%t%d =%s copy 0\n", match_result, rty);
            }
            emit->last_match_temp = match_result;

            // For tagged unions, load the tag first
            int tag_val = -1;
            if (is_tagged) {
                tag_val = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l loadl %%t%d\n", tag_val, subject);
            }
            // For optional/result, query the discriminator once.
            int has_val = -1;
            if (is_optional) {
                has_val = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_optional_has(l %%t%d)\n",
                        has_val, subject);
            } else if (is_result) {
                has_val = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_result_is_ok(l %%t%d)\n",
                        has_val, subject);
            }

            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                struct MatchArm *arm = &stmt->match_stmt.arms[i];

                if (arm->is_wildcard) {
                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK)
                            emit_block(emit, arm->body);
                        else {
                            int val = emit_expr(emit, arm->body);
                            if (has_result) {
                                const char *rty = qbe_type(match_type);
                                fprintf(emit->out, "\t%%t%d =%s copy %%t%d\n", match_result, rty, val);
                            }
                        }
                    }
                    fprintf(emit->out, "\tjmp @L%d\n", l_end3);
                } else if ((is_optional || is_result) && arm->pattern) {
                    // Pattern arms over optional/result subjects:
                    //   some(v) / ok(v)  → matches when discriminator == 1
                    //   none / err(e)    → matches when discriminator == 0
                    // The captured name (v or e) is bound by loading from the
                    // appropriate runtime accessor.
                    AstNode *pat = arm->pattern;
                    bool match_truthy;     // true → take this arm when has_val != 0
                    const char *bind_name = NULL;
                    const char *accessor = NULL;
                    if (pat->kind == NODE_NONE_LIT) {
                        match_truthy = false;
                    } else if (pat->kind == NODE_CALL_EXPR) {
                        if (strcmp(pat->call.name, "some") == 0 ||
                            strcmp(pat->call.name, "ok") == 0) {
                            match_truthy = true;
                            accessor = is_optional ? "mix_optional_get" : "mix_result_unwrap";
                        } else if (strcmp(pat->call.name, "err") == 0) {
                            match_truthy = false;
                            accessor = "mix_result_unwrap_err";
                        } else {
                            // Unknown call-shaped pattern — fall through to next arm.
                            continue;
                        }
                        if (pat->call.arg_count > 0 &&
                            pat->call.args[0]->kind == NODE_IDENT)
                            bind_name = pat->call.args[0]->ident.name;
                    } else {
                        continue;
                    }

                    int l_match = next_label(emit);
                    int l_next = next_label(emit);
                    if (match_truthy)
                        fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", has_val, l_match, l_next);
                    else
                        fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", has_val, l_next, l_match);

                    fprintf(emit->out, "@L%d\n", l_match);
                    qbe_push_local_scope(emit);

                    if (bind_name && accessor) {
                        const char *bind_slot = qbe_match_binding_name(
                            emit, pat->call.args[0], bind_name, "match");
                        int v = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l call $%s(l %%t%d)\n",
                                v, accessor, subject);
                        fprintf(emit->out, "\t%%v.%s =l copy %%t%d\n", bind_slot, v);
                        qbe_bind_local_name(emit, bind_name, bind_slot);
                    }

                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK)
                            emit_block(emit, arm->body);
                        else {
                            int val = emit_expr(emit, arm->body);
                            if (has_result) {
                                const char *rty = qbe_type(match_type);
                                fprintf(emit->out, "\t%%t%d =%s copy %%t%d\n", match_result, rty, val);
                            }
                        }
                    }
                    qbe_pop_local_scope(emit);
                    fprintf(emit->out, "\tjmp @L%d\n", l_end3);
                    fprintf(emit->out, "@L%d\n", l_next);
                } else if (is_tagged && arm->pattern && arm->pattern->kind == NODE_CALL_EXPR) {
                    // Tagged union match: Circle(r) => ...
                    const char *var_name = arm->pattern->call.name;
                    ShapeVariant *sv = type_find_variant(subj_type, var_name);

                    int l_match = next_label(emit);
                    int l_next = next_label(emit);

                    if (sv) {
                        // Compare tag
                        int tag_cmp = next_temp(emit);
                        int tag_const = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l copy %d\n", tag_const, sv->tag);
                        fprintf(emit->out, "\t%%t%d =w ceql %%t%d, %%t%d\n", tag_cmp, tag_val, tag_const);
                        fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", tag_cmp, l_match, l_next);

                        fprintf(emit->out, "@L%d\n", l_match);
                        qbe_push_local_scope(emit);

                        // Extract variant fields into local variables
                        for (int k = 0; k < arm->pattern->call.arg_count && k < sv->field_count; k++) {
                            AstNode *binding = arm->pattern->call.args[k];
                            if (binding->kind == NODE_IDENT) {
                                int foff = 8 + sv->fields[k].offset;
                                const char *fty = type_to_qbe(sv->fields[k].type);
                                int addr = next_temp(emit);
                                fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, subject, foff);
                                int fval = next_temp(emit);
                                fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n", fval, fty, fty, addr);
                                const char *bind_slot = qbe_match_binding_name(
                                    emit, binding, binding->ident.name, "match");
                                fprintf(emit->out, "\t%%v.%s =%s copy %%t%d\n",
                                        bind_slot, fty, fval);
                                qbe_bind_local_name(emit, binding->ident.name, bind_slot);
                            }
                        }

                        if (arm->body) {
                            if (arm->body->kind == NODE_BLOCK)
                                emit_block(emit, arm->body);
                            else {
                                int val = emit_expr(emit, arm->body);
                                if (has_result) {
                                    const char *rty = qbe_type(match_type);
                                    fprintf(emit->out, "\t%%t%d =%s copy %%t%d\n", match_result, rty, val);
                                }
                            }
                        }
                        qbe_pop_local_scope(emit);
                    }
                    fprintf(emit->out, "\tjmp @L%d\n", l_end3);
                    fprintf(emit->out, "@L%d\n", l_next);
                } else {
                    // Regular value match
                    int pat = emit_expr(emit, arm->pattern);
                    int cmp2 = next_temp(emit);
                    if (is_flt)
                        fprintf(emit->out, "\t%%t%d =w ceq%s %%t%d, %%t%d\n", cmp2, sty, subject, pat);
                    else
                        fprintf(emit->out, "\t%%t%d =w ceq%s %%t%d, %%t%d\n", cmp2, sty, subject, pat);
                    int l_match = next_label(emit);
                    int l_next = next_label(emit);
                    fprintf(emit->out, "\tjnz %%t%d, @L%d, @L%d\n", cmp2, l_match, l_next);

                    fprintf(emit->out, "@L%d\n", l_match);
                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK)
                            emit_block(emit, arm->body);
                        else {
                            int val = emit_expr(emit, arm->body);
                            if (has_result) {
                                const char *rty = qbe_type(match_type);
                                fprintf(emit->out, "\t%%t%d =%s copy %%t%d\n", match_result, rty, val);
                            }
                        }
                    }
                    fprintf(emit->out, "\tjmp @L%d\n", l_end3);
                    fprintf(emit->out, "@L%d\n", l_next);
                }
            }

            fprintf(emit->out, "@L%d\n", l_end3);
            break;
        }
        case NODE_DONE_STMT: {
            if (stmt->done_stmt.value) {
                int val = emit_expr(emit, stmt->done_stmt.value);
                // If returning from result function, wrap value in result_ok()
                if (emit->current_return_type &&
                    emit->current_return_type->kind == TYPE_RESULT &&
                    stmt->done_stmt.value->kind != NODE_NONE_LIT) {
                    // If the ok_type is float, cast to int64 bits before wrapping
                    int wrap_val = val;
                    MixType *ok_type = emit->current_return_type->result.ok_type;
                    if (ok_type && type_is_float(ok_type)) {
                        int cast = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", cast, val);
                        wrap_val = cast;
                    }
                    int wrapped = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_result_ok(l %%t%d)\n", wrapped, wrap_val);
                    emit_return_temp(emit, wrapped);
                // If returning from optional function and value isn't already none, wrap it
                } else if (emit->current_return_type &&
                    emit->current_return_type->kind == TYPE_OPTIONAL &&
                    stmt->done_stmt.value->kind != NODE_NONE_LIT) {
                    int wrapped = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_optional_some(l %%t%d)\n", wrapped, val);
                    emit_return_temp(emit, wrapped);
                } else {
                    emit_return_temp(emit, val);
                }
            } else {
                emit_return_void(emit);
            }
            // Emit dead-code label so subsequent instructions are valid QBE
            fprintf(emit->out, "@dead%d\n", next_label(emit));
            break;
        }
        case NODE_DEFER_STMT: {
            // Push deferred statement onto stack — will be emitted before returns
            if (emit->defer_count < 64) {
                emit->deferred[emit->defer_count++] = stmt->defer_stmt.stmt;
            }
            break;
        }
        case NODE_EXPR_STMT: {
            (void)emit_expr(emit, stmt->expr_stmt.expr);
            break;
        }
        case NODE_UNSAFE_BLOCK:
            // Unsafe just emits the block — the safety gate is in sema
            emit_block(emit, stmt->unsafe_block.body);
            break;
        case NODE_ZONE_STMT: {
            // Enter zone, emit body, exit zone
            int enter_t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_zone_enter()\n", enter_t);
            emit_block(emit, stmt->zone_stmt.body);
            int exit_t = next_temp(emit);
            fprintf(emit->out, "\t%%t%d =l call $mix_zone_exit()\n", exit_t);
            break;
        }
        case NODE_DEREF_ASSIGN: {
            // *ptr = val → store val at ptr address
            int ptr = emit_expr(emit, stmt->deref_assign.ptr_expr);
            int val = emit_expr(emit, stmt->deref_assign.value);
            MixType *val_type = stmt->deref_assign.value->resolved_type;
            const char *ty = qbe_type(val_type);
            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", ty, val, ptr);
            break;
        }
        case NODE_INDEX_ASSIGN: {
            int obj = emit_expr(emit, stmt->index_assign.object);
            int idx = emit_expr(emit, stmt->index_assign.index);
            int val = emit_expr(emit, stmt->index_assign.value);
            MixType *obj_type = ref_base_type(stmt->index_assign.object->resolved_type);
            int t = next_temp(emit);
            if (obj_type && obj_type->kind == TYPE_MAP) {
                fprintf(emit->out, "\t%%t%d =l call $mix_map_set(l %%t%d, l %%t%d, l %%t%d)\n",
                        t, obj, idx, val);
            } else {
                MixType *elem = (obj_type && obj_type->kind == TYPE_LIST) ? obj_type->list.elem_type : NULL;
                if (elem && elem->kind == TYPE_SHAPE) {
                    fprintf(emit->out, "\tcall $mix_list_set_bytes(l %%t%d, l %%t%d, l %%t%d)\n",
                            obj, idx, val);
                    fprintf(emit->out, "\t%%t%d =l copy 0\n", t);
                } else {
                    int store_val = val;
                    if (elem && type_is_float(elem)) {
                        int bits = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", bits, val);
                        store_val = bits;
                    }
                    fprintf(emit->out, "\t%%t%d =l call $mix_list_set(l %%t%d, l %%t%d, l %%t%d)\n",
                            t, obj, idx, store_val);
                }
            }
            break;
        }
        case NODE_FAIL_STMT: {
            int fval = emit_expr(emit, stmt->fail_stmt.value);
            // If current function returns TYPE_RESULT, emit result_err + ret
            if (emit->current_return_type &&
                emit->current_return_type->kind == TYPE_RESULT) {
                int err = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =l call $mix_result_err(l %%t%d)\n", err, fval);
                emit_return_temp(emit, err);
            } else {
                // Backward compat: void functions or non-result → panic
                MixType *vtype = stmt->fail_stmt.value->resolved_type;
                if (vtype && vtype->kind == TYPE_STR) {
                    fprintf(emit->out, "\tcall $mix_panic(l %%t%d)\n", fval);
                } else {
                    const char *msg = emit_string_data(emit, "fail", 4);
                    int mt = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l copy %s\n", mt, msg);
                    fprintf(emit->out, "\tcall $mix_panic(l %%t%d)\n", mt);
                }
            }
            fprintf(emit->out, "@dead%d\n", next_label(emit));
            break;
        }
        case NODE_FIELD_ASSIGN: {
            int obj = emit_expr(emit, stmt->field_assign.object);
            int val = emit_expr(emit, stmt->field_assign.value);
            MixType *obj_type = ref_base_type(stmt->field_assign.object->resolved_type);
            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi = type_find_field(obj_type, stmt->field_assign.field_name);
                if (fi) {
                    // Whole-shape replacement: `s.pos = Vec2(...)`. Both sides are
                    // pointers; the field is stored inline so a single store would
                    // only copy 8 bytes. memcpy the full shape size instead.
                    if (fi->type && fi->type->kind == TYPE_SHAPE &&
                        stmt->field_assign.op == TOK_EQ) {
                        int dst_addr;
                        if (fi->offset == 0) {
                            dst_addr = obj;
                        } else {
                            dst_addr = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n",
                                    dst_addr, obj, fi->offset);
                        }
                        emit_shape_copy(emit, dst_addr, val, fi->type);
                        break;
                    }
                    const char *mem_ty = type_to_qbe_mem(fi->type);
                    if (stmt->field_assign.op != TOK_EQ) {
                        // Compound assignment: load old value, apply op, store
                        const char *reg_ty = type_to_qbe(fi->type);
                        const char *load_ty = type_to_qbe_load(fi->type);
                        int old_val = next_temp(emit);
                        int addr = -1;
                        if (fi->offset == 0) {
                            fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n", old_val, reg_ty, load_ty, obj);
                        } else {
                            addr = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, obj, fi->offset);
                            fprintf(emit->out, "\t%%t%d =%s load%s %%t%d\n", old_val, reg_ty, load_ty, addr);
                        }
                        const char *op;
                        switch (stmt->field_assign.op) {
                            case TOK_PLUS_EQ:  op = "add"; break;
                            case TOK_MINUS_EQ: op = "sub"; break;
                            case TOK_STAR_EQ:  op = "mul"; break;
                            case TOK_SLASH_EQ: op = "div"; break;
                            default: op = "add"; break;
                        }
                        int result = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =%s %s %%t%d, %%t%d\n", result, reg_ty, op, old_val, val);
                        if (fi->offset == 0) {
                            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, result, obj);
                        } else {
                            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, result, addr);
                        }
                    } else {
                        // Simple assignment
                        val = coerce_to_field_type(emit, val,
                                stmt->field_assign.value->resolved_type, fi->type);
                        if (fi->offset == 0) {
                            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, val, obj);
                        } else {
                            int addr = next_temp(emit);
                            fprintf(emit->out, "\t%%t%d =l add %%t%d, %d\n", addr, obj, fi->offset);
                            fprintf(emit->out, "\tstore%s %%t%d, %%t%d\n", mem_ty, val, addr);
                        }
                    }
                }
            }
            break;
        }
        case NODE_BREAK_STMT: {
            if (emit->loop_depth > 0) {
                fprintf(emit->out, "\tjmp @L%d\n", emit->break_labels[emit->loop_depth - 1]);
                fprintf(emit->out, "@dead%d\n", next_label(emit));
            }
            break;
        }
        case NODE_CONTINUE_STMT: {
            if (emit->loop_depth > 0) {
                fprintf(emit->out, "\tjmp @L%d\n", emit->continue_labels[emit->loop_depth - 1]);
                fprintf(emit->out, "@dead%d\n", next_label(emit));
            }
            break;
        }
        default:
            break;
    }
}

// --- Function emission ---

static void emit_fn_decl(QbeEmitter *emit, AstNode *fn) {
    bool is_main = strcmp(fn->fn_decl.name, "main") == 0;

    // Function signature
    if (is_main) {
        fprintf(emit->out, "export function w $main(w %%p.argc, l %%p.argv) {\n@start\n");
        // Store argc/argv for args() builtin
        fprintf(emit->out, "\tcall $mix_set_args(w %%p.argc, l %%p.argv)\n");
    } else {
        MixType *ret_type = fn->fn_decl.return_type
            ? fn->fn_decl.return_type->resolved_type : NULL;
        // Check if the function was wrapped in TYPE_RESULT by sema
        Symbol *fn_sig_sym = symtab_lookup(emit->symtab, fn->fn_decl.name);
        if (fn_sig_sym && fn_sig_sym->type && fn_sig_sym->type->kind == TYPE_FUNC &&
            fn_sig_sym->type->func.return_type &&
            fn_sig_sym->type->func.return_type->kind == TYPE_RESULT) {
            ret_type = fn_sig_sym->type->func.return_type;
        }
        bool shape_return = ret_type && ret_type->kind == TYPE_SHAPE;
        const char *ret = ret_type ? qbe_sig_type(ret_type, emit->arena) : NULL;
        const char *export_kw = fn->fn_decl.is_pub ? "export " : "";

        if (ret && !shape_return) {
            fprintf(emit->out, "%sfunction %s $%s(", export_kw, ret, fn->fn_decl.name);
        } else {
            fprintf(emit->out, "%sfunction $%s(", export_kw, fn->fn_decl.name);
        }

        bool needs_comma = false;
        if (shape_return) {
            fprintf(emit->out, "l %%p._ret");
            needs_comma = true;
        }

        for (int i = 0; i < fn->fn_decl.param_count; i++) {
            if (needs_comma) fprintf(emit->out, ", ");
            Param *param = &fn->fn_decl.params[i];
            MixType *ptype = param->type ? param->type->resolved_type : NULL;
            const char *ty = qbe_param_sig_type(ptype, param->is_mutable,
                                                emit->arena);
            fprintf(emit->out, "%s %%p.%s", ty, param->name);
            needs_comma = true;
        }

        fprintf(emit->out, ") {\n@start\n");
    }

    // Reset per-function state before binding parameters or walking the body.
    emit->defer_count = 0;
    emit->fn_ptr_var_count = 0;
    emit->rc_local_count = 0;
    emit->mutable_param_count = 0;
    emit->dbg_line = 0;
    qbe_local_names_reset(emit);

    // Copy parameters to variable names
    for (int i = 0; i < fn->fn_decl.param_count; i++) {
        Param *param = &fn->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        const char *ty = qbe_type(ptype);

        if (param->is_mutable) {
            if (ptype && ptype->kind == TYPE_SHAPE) {
                fprintf(emit->out, "\t%%v.%s =l copy %%p.%s\n",
                        param->name, param->name);
            } else {
                int size = qbe_stack_slot_size(ptype);
                fprintf(emit->out, "\t%%v.%s =l alloc8 %d\n", param->name, size);
                int init = next_temp(emit);
                const char *load_ty = type_to_qbe_load(ptype);
                fprintf(emit->out, "\t%%t%d =%s load%s %%p.%s\n",
                        init, ty, load_ty, param->name);
                fprintf(emit->out, "\tstore%s %%t%d, %%v.%s\n",
                        type_to_qbe_mem(ptype), init, param->name);
                qbe_track_mutable_param(emit, param->name, ptype);
            }
        } else if (ptype && ptype->kind == TYPE_SHAPE) {
            fprintf(emit->out, "\t%%v.%s =l alloc8 %d\n",
                    param->name, ptype->shape.total_size);
            fprintf(emit->out, "\tcall $memcpy(l %%v.%s, l %%p.%s, l %d)\n",
                    param->name, param->name, ptype->shape.total_size);
        } else {
            fprintf(emit->out, "\t%%v.%s =%s copy %%p.%s\n", param->name, ty, param->name);
        }
        qbe_bind_local_name(emit, param->name, param->name);
    }
    // Reset const memoization (temps are per-function in QBE)
    for (int ci = 0; ci < emit->const_count; ci++)
        emit->constants[ci].cached_temp = -1;

    // Track return type for optional/result wrapping
    // First check symtab for the function type (which may have been wrapped in TYPE_RESULT)
    MixType *ret_type_resolved = fn->fn_decl.return_type
        ? fn->fn_decl.return_type->resolved_type : NULL;
    Symbol *fn_sym_lookup = symtab_lookup(emit->symtab, fn->fn_decl.name);
    if (fn_sym_lookup && fn_sym_lookup->type && fn_sym_lookup->type->kind == TYPE_FUNC) {
        emit->current_return_type = fn_sym_lookup->type->func.return_type;
    } else {
        emit->current_return_type = ret_type_resolved;
    }

    // Register function-typed parameters as indirect call targets
    for (int i = 0; i < fn->fn_decl.param_count; i++) {
        Param *param = &fn->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        if (ptype && ptype->kind == TYPE_FUNC && emit->fn_ptr_var_count < 128) {
            emit->fn_ptr_vars[emit->fn_ptr_var_count++] =
                arena_strdup(emit->arena, param->name);
        }
    }

    // Emit body — handle implicit return of last expression
    if (fn->fn_decl.body) {
        AstNode *body = fn->fn_decl.body;
        int stmt_count = body->block.stmt_count;

        // Emit all statements except the last one normally
        for (int i = 0; i < stmt_count - 1; i++) {
            emit_stmt(emit, body->block.stmts[i]);
        }

        // Last statement: if it's an expression statement in a non-main function
        // with a return type, emit it as an implicit return
        if (stmt_count > 0) {
            AstNode *last = body->block.stmts[stmt_count - 1];
            if (!is_main && fn->fn_decl.return_type && last->kind == NODE_EXPR_STMT) {
                int val = emit_expr(emit, last->expr_stmt.expr);
                // Wrap in optional if needed
                if (ret_type_resolved && ret_type_resolved->kind == TYPE_OPTIONAL &&
                    last->expr_stmt.expr->kind != NODE_NONE_LIT) {
                    int wrapped = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_optional_some(l %%t%d)\n", wrapped, val);
                    emit_return_temp(emit, wrapped);
                } else if (emit->current_return_type &&
                           emit->current_return_type->kind == TYPE_RESULT &&
                           last->expr_stmt.expr->kind != NODE_NONE_LIT) {
                    // The function's return was promoted to Result by sema
                    // (because it's a `~` fn with `fail`s). Wrap the implicit
                    // value in mix_result_ok().
                    int wrap_val = val;
                    MixType *ok_type = emit->current_return_type->result.ok_type;
                    if (ok_type && type_is_float(ok_type)) {
                        int cast = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l cast %%t%d\n", cast, val);
                        wrap_val = cast;
                    }
                    int wrapped = next_temp(emit);
                    fprintf(emit->out, "\t%%t%d =l call $mix_result_ok(l %%t%d)\n",
                            wrapped, wrap_val);
                    emit_return_temp(emit, wrapped);
                } else {
                    emit_return_temp(emit, val);
                }
                fprintf(emit->out, "}\n\n");
                return;
            } else if (!is_main && fn->fn_decl.return_type && last->kind == NODE_MATCH_STMT) {
                // Match as implicit return — emit match, then return its result temp
                emit->last_match_temp = -1;
                emit_stmt(emit, last);
                if (emit->last_match_temp >= 0) {
                    int val = emit->last_match_temp;
                    if (ret_type_resolved && ret_type_resolved->kind == TYPE_OPTIONAL) {
                        int wrapped = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l call $mix_optional_some(l %%t%d)\n", wrapped, val);
                        emit_return_temp(emit, wrapped);
                    } else if (emit->current_return_type && emit->current_return_type->kind == TYPE_RESULT) {
                        int wrapped = next_temp(emit);
                        fprintf(emit->out, "\t%%t%d =l call $mix_result_ok(l %%t%d)\n", wrapped, val);
                        emit_return_temp(emit, wrapped);
                    } else {
                        emit_return_temp(emit, val);
                    }
                    fprintf(emit->out, "}\n\n");
                    return;
                }
            } else {
                emit_stmt(emit, last);
            }
        }
    }

    emit_function_epilogue(emit);

    // Default return
    if (is_main) {
        fprintf(emit->out, "\tret 0\n");
    } else if (!fn->fn_decl.return_type || (emit->current_return_type &&
               emit->current_return_type->kind == TYPE_SHAPE)) {
        fprintf(emit->out, "\tret\n");
    } else {
        fprintf(emit->out, "\tret 0\n"); // fallback
    }

    fprintf(emit->out, "}\n\n");
}

// Emit a shape method as a regular function with self parameter
static void emit_method(QbeEmitter *emit, AstNode *method, const char *shape_name, MixType *shape_type, bool is_pub) {
    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s_%s", shape_name, method->fn_decl.name);

    MixType *ret_type = method->fn_decl.return_type
        ? method->fn_decl.return_type->resolved_type : NULL;
    bool shape_return = ret_type && ret_type->kind == TYPE_SHAPE;
    const char *ret_sig = ret_type ? qbe_sig_type(ret_type, emit->arena) : NULL;
    const char *export_kw = is_pub ? "export " : "";

    if (ret_sig && !shape_return) {
        fprintf(emit->out, "%sfunction %s $%s(", export_kw, ret_sig, mangled);
    } else {
        fprintf(emit->out, "%sfunction $%s(", export_kw, mangled);
    }

    bool needs_comma = false;
    if (shape_return) {
        fprintf(emit->out, "l %%p._ret");
        needs_comma = true;
    }

    (void)shape_name;
    if (needs_comma) fprintf(emit->out, ", ");
    fprintf(emit->out, "l %%p.self");
    needs_comma = true;

    // User params
    for (int i = 0; i < method->fn_decl.param_count; i++) {
        if (needs_comma) fprintf(emit->out, ", ");
        Param *param = &method->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        const char *ty = qbe_param_sig_type(ptype, param->is_mutable,
                                            emit->arena);
        fprintf(emit->out, "%s %%p.%s", ty, param->name);
        needs_comma = true;
    }

    fprintf(emit->out, ") {\n@start\n");

    emit->defer_count = 0;
    emit->fn_ptr_var_count = 0;
    emit->rc_local_count = 0;
    emit->mutable_param_count = 0;
    qbe_local_names_reset(emit);

    // Copy self to variable
    fprintf(emit->out, "\t%%v.self =l copy %%p.self\n");
    qbe_bind_local_name(emit, "self", "self");

    // Copy user params
    for (int i = 0; i < method->fn_decl.param_count; i++) {
        Param *param = &method->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        const char *ty = qbe_type(ptype);
        if (param->is_mutable) {
            if (ptype && ptype->kind == TYPE_SHAPE) {
                fprintf(emit->out, "\t%%v.%s =l copy %%p.%s\n",
                        param->name, param->name);
            } else {
                int size = qbe_stack_slot_size(ptype);
                fprintf(emit->out, "\t%%v.%s =l alloc8 %d\n", param->name, size);
                int init = next_temp(emit);
                fprintf(emit->out, "\t%%t%d =%s load%s %%p.%s\n",
                        init, ty, type_to_qbe_load(ptype), param->name);
                fprintf(emit->out, "\tstore%s %%t%d, %%v.%s\n",
                        type_to_qbe_mem(ptype), init, param->name);
                qbe_track_mutable_param(emit, param->name, ptype);
            }
        } else if (ptype && ptype->kind == TYPE_SHAPE) {
            fprintf(emit->out, "\t%%v.%s =l alloc8 %d\n",
                    param->name, ptype->shape.total_size);
            fprintf(emit->out, "\tcall $memcpy(l %%v.%s, l %%p.%s, l %d)\n",
                    param->name, param->name, ptype->shape.total_size);
        } else {
            fprintf(emit->out, "\t%%v.%s =%s copy %%p.%s\n", param->name, ty, param->name);
        }
        qbe_bind_local_name(emit, param->name, param->name);
    }

    // Set current_shape for field resolution
    emit->current_shape = shape_type;
    emit->current_return_type = ret_type;
    // Reset const memoization (temps are per-function in QBE)
    for (int ci = 0; ci < emit->const_count; ci++)
        emit->constants[ci].cached_temp = -1;

    // Emit body with implicit return
    if (method->fn_decl.body) {
        AstNode *body = method->fn_decl.body;
        int stmt_count = body->block.stmt_count;

        for (int i = 0; i < stmt_count - 1; i++) {
            emit_stmt(emit, body->block.stmts[i]);
        }

        if (stmt_count > 0) {
            AstNode *last = body->block.stmts[stmt_count - 1];
            if (ret_type && last->kind == NODE_EXPR_STMT) {
                int val = emit_expr(emit, last->expr_stmt.expr);
                emit_return_temp(emit, val);
                emit->current_shape = NULL;
                emit->current_return_type = NULL;
                fprintf(emit->out, "}\n\n");
                return;
            } else {
                emit_stmt(emit, last);
            }
        }
    }

    emit_function_epilogue(emit);

    if (ret_type && ret_type->kind != TYPE_SHAPE) {
        fprintf(emit->out, "\tret 0\n");
    } else {
        fprintf(emit->out, "\tret\n");
    }

    emit->current_shape = NULL;
    emit->current_return_type = NULL;
    fprintf(emit->out, "}\n\n");
}

// --- Program emission ---

void qbe_emit_program(QbeEmitter *emit, AstNode *program) {
    if (!program || program->kind != NODE_PROGRAM) return;

    // Emit debug file declaration for DWARF line info
    if (emit->emit_debug_info && program->loc.filename) {
        fprintf(emit->out, "dbgfile \"%s\"\n", program->loc.filename);
    }

    // Register compile-time constants
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_CONST_DECL) {
            if (emit->const_count < 8192) {
                emit->constants[emit->const_count].name = decl->const_decl.name;
                emit->constants[emit->const_count].value = decl->const_decl.value;
                emit->constants[emit->const_count].cached_temp = -1;
                emit->const_count++;
            }
        }
    }

    // Emit QBE type definitions for shapes (both local and imported)
    // First: emit from local shape declarations
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_SHAPE_DECL) {
            // Skip generic shape templates — only their instantiations
            // (which sema spliced into the program) get emitted.
            if (decl->shape_decl.type_param_count > 0) continue;
            Symbol *shape_sym2 = symtab_lookup(emit->symtab, decl->shape_decl.name);
            MixType *st = (shape_sym2 && shape_sym2->type) ? shape_sym2->type : NULL;

            if (st && st->shape.is_tagged_union) {
                // Tagged union: emit as { l (tag), then pad to total_size }
                int data_bytes = st->shape.total_size - 8;
                int data_longs = (data_bytes + 7) / 8;
                fprintf(emit->out, "type :%s = { l", decl->shape_decl.name);
                for (int j = 0; j < data_longs; j++)
                    fprintf(emit->out, ", l");
                fprintf(emit->out, " }\n");
            } else if (st && st->shape.is_union) {
                // C-style union: pad to total_size with longs
                int nlongs = (st->shape.total_size + 7) / 8;
                fprintf(emit->out, "type :%s = { ", decl->shape_decl.name);
                for (int j = 0; j < nlongs; j++) {
                    if (j > 0) fprintf(emit->out, ", ");
                    fprintf(emit->out, "l");
                }
                fprintf(emit->out, " }\n");
            } else {
                fprintf(emit->out, "type :%s = { ", decl->shape_decl.name);
                for (int j = 0; j < decl->shape_decl.field_count; j++) {
                    if (j > 0) fprintf(emit->out, ", ");
                    ShapeField *sf = &decl->shape_decl.fields[j];
                    AstNode *ftype_node = sf->type;
                    MixType *ftype = ftype_node ? ftype_node->resolved_type : NULL;
                    fprintf(emit->out, "%s", type_to_qbe_mem(ftype));
                }
                fprintf(emit->out, " }\n");
            }
        }
    }
    // Second: emit type defs for imported shapes (from symbol table)
    // Walk all scopes looking for shape types not already emitted
    {
        char *emitted_shapes[128];
        int emitted_count = 0;
        // Collect locally-defined shape names
        for (int i = 0; i < program->program.decl_count; i++) {
            if (program->program.decls[i]->kind == NODE_SHAPE_DECL && emitted_count < 128) {
                emitted_shapes[emitted_count++] = program->program.decls[i]->shape_decl.name;
            }
        }
        // Scan symbol table for shape types not in the local list
        for (Scope *scope = emit->symtab->current; scope; scope = scope->parent) {
            for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
                if (sym->type && sym->type->kind == TYPE_SHAPE) {
                    bool already_emitted = false;
                    for (int j = 0; j < emitted_count; j++) {
                        if (strcmp(emitted_shapes[j], sym->type->shape.name) == 0) {
                            already_emitted = true;
                            break;
                        }
                    }
                    if (!already_emitted && emitted_count < 128) {
                        if (sym->type->shape.is_union || sym->type->shape.is_tagged_union) {
                            // Union/tagged union: pad to total_size with longs
                            int base = sym->type->shape.is_tagged_union ? 8 : 0;
                            int data_bytes = sym->type->shape.total_size - base;
                            int nlongs = (data_bytes + 7) / 8;
                            if (sym->type->shape.is_tagged_union) {
                                fprintf(emit->out, "type :%s = { l", sym->type->shape.name);
                                for (int j = 0; j < nlongs; j++)
                                    fprintf(emit->out, ", l");
                            } else {
                                fprintf(emit->out, "type :%s = { ", sym->type->shape.name);
                                for (int j = 0; j < nlongs; j++) {
                                    if (j > 0) fprintf(emit->out, ", ");
                                    fprintf(emit->out, "l");
                                }
                            }
                            fprintf(emit->out, " }\n");
                        } else {
                            fprintf(emit->out, "type :%s = { ", sym->type->shape.name);
                            for (int j = 0; j < sym->type->shape.field_count; j++) {
                                if (j > 0) fprintf(emit->out, ", ");
                                fprintf(emit->out, "%s", type_to_qbe_mem(sym->type->shape.fields[j].type));
                            }
                            fprintf(emit->out, " }\n");
                        }
                        emitted_shapes[emitted_count++] = sym->type->shape.name;
                    }
                }
            }
        }
    }
    fprintf(emit->out, "\n");

    // ---------------------------------------------------------------
    // Refcount Phase 1: emit per-shape release_<ShapeName> functions.
    // For now each is just `mix_shape_free(p)` — Phase 2 will add the
    // recursive release of shape-typed and list-typed fields.
    //
    // We emit one release fn for every locally-declared non-generic
    // shape, plus extern declarations for shapes imported from other
    // modules (their release fn is emitted by the defining module).
    // ---------------------------------------------------------------
    {
        // Reset and populate the local-shape-name set so SHAPE_LIT
        // codegen knows whether this module owns a release fn for a
        // given shape (and should reference it) versus an imported /
        // C-bound shape (where we pass 0 and let runtime fall back to
        // plain mix_shape_free).
        emit->local_shape_name_count = 0;

        for (int i = 0; i < program->program.decl_count; i++) {
            AstNode *decl = program->program.decls[i];
            if (decl->kind != NODE_SHAPE_DECL) continue;
            if (decl->shape_decl.type_param_count > 0) continue;  // generic templates
            const char *name = decl->shape_decl.name;
            // Track for SHAPE_LIT's release-fn-reference decision.
            if (emit->local_shape_name_count < 1024) {
                emit->local_shape_names[emit->local_shape_name_count++] =
                    arena_strdup(emit->arena, name);
            }
            // Emit as `export function` so cross-module SHAPE_LIT calls
            // resolve at link time.
            fprintf(emit->out,
                "export function $release_%s(l %%p) {\n"
                "@start\n"
                "\tcall $mix_shape_free(l %%p)\n"
                "\tret\n"
                "}\n",
                name);
        }
        // Imported / C-bound shapes have no release fn from us; their
        // SHAPE_LIT passes 0 as release_fn so mix_release calls
        // mix_shape_free directly (no recursive field cleanup needed
        // for plain C structs without shape-typed children).
    }
    fprintf(emit->out, "\n");

    // Emit data sections for module-level mutables (`name! = literal` at
    // module scope). `pub` ones are exported so importing modules can
    // resolve `$g_<name>` at link time; private ones are local to the
    // current translation unit. Reads/writes against $g_<name> are
    // routed through NODE_IDENT/NODE_ASSIGN special-cases.
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind != NODE_VAR_DECL || !decl->var_decl.is_global) continue;
        AstNode *init = decl->var_decl.init_expr;
        bool neg = false;
        if (init && init->kind == NODE_UNARY_EXPR && init->unary.op == TOK_MINUS) {
            neg = true;
            init = init->unary.operand;
        }
        const char *name = decl->var_decl.name;
        const char *export_kw = decl->var_decl.is_pub ? "export " : "";
        if (init && init->kind == NODE_INT_LIT) {
            int64_t v = init->int_lit.value;
            if (neg) v = -v;
            fprintf(emit->out, "%sdata $g_%s = align 8 { l %" PRId64 " }\n",
                    export_kw, name, v);
        } else if (init && init->kind == NODE_FLOAT_LIT) {
            double v = init->float_lit.value;
            if (neg) v = -v;
            fprintf(emit->out, "%sdata $g_%s = align 8 { d d_%a }\n",
                    export_kw, name, v);
        } else if (init && init->kind == NODE_BOOL_LIT) {
            fprintf(emit->out, "%sdata $g_%s = align 4 { w %d }\n",
                    export_kw, name, init->bool_lit.value ? 1 : 0);
        } else if (init && init->kind == NODE_STRING_LIT) {
            const char *s = emit_string_data(emit,
                init->string_lit.value, init->string_lit.length);
            fprintf(emit->out, "%sdata $g_%s = align 8 { l %s }\n",
                    export_kw, name, s);
        }
    }
    fprintf(emit->out, "\n");

    // Emit functions and shape methods
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            emit_fn_decl(emit, decl);
        } else if (decl->kind == NODE_SHAPE_DECL) {
            // Skip generic shape templates — instantiations are emitted
            // separately as their own decls.
            if (decl->shape_decl.type_param_count > 0) continue;
            // Emit methods
            Symbol *shape_sym = symtab_lookup(emit->symtab, decl->shape_decl.name);
            MixType *shape_type = (shape_sym && shape_sym->type) ? shape_sym->type : NULL;
            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                emit_method(emit, decl->shape_decl.methods[j],
                            decl->shape_decl.name, shape_type, decl->shape_decl.is_pub);
            }
        } else if (decl->kind == NODE_COND_DECL && decl->cond_decl.active) {
            for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                AstNode *cd = decl->cond_decl.decls[j];
                if (cd->kind == NODE_FN_DECL) {
                    emit_fn_decl(emit, cd);
                }
            }
        }
    }

    // Emit lambda functions (collected during expression emission)
    for (int i = 0; i < emit->lambda_count; i++) {
        AstNode *lam = emit->lambdas[i];
        const char *lname = lam->lambda.generated_name;

        // For now, lambda params are all 'l' (64-bit) since types are inferred
        // Return type comes from the body expression's resolved type
        MixType *body_type = lam->lambda.body ? lam->lambda.body->resolved_type : NULL;
        const char *ret = body_type ? qbe_type(body_type) : "l";

        fprintf(emit->out, "\nfunction %s $%s(", ret, lname);
        for (int j = 0; j < lam->lambda.param_count; j++) {
            if (j > 0) fprintf(emit->out, ", ");
            fprintf(emit->out, "l %%p.%s", lam->lambda.param_names[j]);
        }
        fprintf(emit->out, ") {\n@start\n");

        emit->dbg_line = 0; // reset debug tracking for lambda
        emit->defer_count = 0;
        emit->rc_local_count = 0;
        qbe_local_names_reset(emit);

        // Copy params to variable names
        for (int j = 0; j < lam->lambda.param_count; j++) {
            fprintf(emit->out, "\t%%v.%s =l copy %%p.%s\n",
                    lam->lambda.param_names[j], lam->lambda.param_names[j]);
            qbe_bind_local_name(emit, lam->lambda.param_names[j],
                                lam->lambda.param_names[j]);
        }

        // Emit body expression as return value
        int val = emit_expr(emit, lam->lambda.body);
        fprintf(emit->out, "\tret %%t%d\n", val);
        fprintf(emit->out, "}\n");
    }

    // Emit string data sections
    for (int i = 0; i < emit->string_count; i++) {
        fprintf(emit->out, "data %s = { b \"", emit->strings[i].qbe_name);
        // Escape the string for QBE
        for (int j = 0; j < emit->strings[i].length; j++) {
            char c = emit->strings[i].value[j];
            if (c == '\\' && j + 1 < emit->strings[i].length) {
                char next = emit->strings[i].value[j + 1];
                if (next == 'n') { fprintf(emit->out, "\\n"); j++; continue; }
                if (next == 't') { fprintf(emit->out, "\\t"); j++; continue; }
                if (next == '\\') { fprintf(emit->out, "\\\\"); j++; continue; }
                if (next == '"') { fprintf(emit->out, "\\\""); j++; continue; }
            }
            if (c == '"') fprintf(emit->out, "\\\"");
            else fputc(c, emit->out);
        }
        fprintf(emit->out, "\", b 0 }\n");
    }
}
