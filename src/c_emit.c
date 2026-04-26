#include "c_emit.h"
#include "errors.h"
#include "arena.h"
#include <inttypes.h>

CEmitter c_emitter_create(FILE *out, Arena *arena, SymTab *symtab) {
    CEmitter emit = {0};
    emit.out = out;
    emit.arena = arena;
    emit.symtab = symtab;
    emit.last_match_temp = -1;
    return emit;
}

static int next_temp(CEmitter *emit) { return emit->temp_counter++; }
static int next_label(CEmitter *emit) { return emit->label_counter++; }

/* Variable shadowing: each MIX `x = 1; x! = 2` shadow gets a unique C name.
 * cname_decl() bumps the suffix and returns the new C name (e.g. "x", then "x__1").
 * cname_ref() returns the currently-active C name for a MIX identifier.
 * Both return strings owned by the emitter arena.
 */
static char *cname_with_suffix(CEmitter *emit, const char *name, int idx) {
    if (idx == 0) return arena_strdup(emit->arena, name);
    int len = (int)strlen(name) + 16;
    char *out = arena_alloc(emit->arena, len);
    snprintf(out, len, "%s__%d", name, idx);
    return out;
}

static const char *cname_ref(CEmitter *emit, const char *name) {
    for (int i = 0; i < emit->var_decl_count; i++) {
        if (strcmp(emit->var_decls[i].name, name) == 0) {
            return cname_with_suffix(emit, name, emit->var_decls[i].active_idx);
        }
    }
    return name;
}

static const char *cname_decl(CEmitter *emit, const char *name) {
    for (int i = 0; i < emit->var_decl_count; i++) {
        if (strcmp(emit->var_decls[i].name, name) == 0) {
            emit->var_decls[i].active_idx++;
            return cname_with_suffix(emit, name, emit->var_decls[i].active_idx);
        }
    }
    if (emit->var_decl_count < 256) {
        emit->var_decls[emit->var_decl_count].name = arena_strdup(emit->arena, name);
        emit->var_decls[emit->var_decl_count].active_idx = 0;
        emit->var_decl_count++;
    }
    return name;
}

static void cname_reset(CEmitter *emit) {
    emit->var_decl_count = 0;
}

static void ind(CEmitter *emit) {
    for (int i = 0; i < emit->indent; i++) fprintf(emit->out, "    ");
}

/* Map MixType to C type string */
static const char *c_type(MixType *type) {
    if (!type) return "int64_t";
    switch (type->kind) {
        case TYPE_FLOAT: case TYPE_FLOAT64: return "double";
        case TYPE_FLOAT32: return "float";
        case TYPE_BOOL: return "int32_t";
        case TYPE_INT32: return "int32_t";
        case TYPE_UINT32: return "uint32_t";
        case TYPE_INT16: return "int16_t";
        case TYPE_UINT16: return "uint16_t";
        case TYPE_INT8: return "int8_t";
        case TYPE_UINT8: case TYPE_BYTE: return "uint8_t";
        case TYPE_STR: return "const char *";
        case TYPE_LIST: case TYPE_MAP: case TYPE_SET:
        case TYPE_OPTIONAL: case TYPE_RESULT:
        case TYPE_SHARED: case TYPE_TASK:
        case TYPE_PTR: case TYPE_FUNC: return "void *";
        case TYPE_VOID: return "void";
        default: return "int64_t";
    }
}

/* Write C-escaped string to output */
static void c_escape_string(FILE *out, const char *str, int len) {
    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c == '"') { fprintf(out, "\\\""); }
        else if (c == '\\' && i + 1 < len) {
            char next = str[i + 1];
            if (next == 'n' || next == 't' || next == '\\' || next == '"' || next == '0') {
                fputc('\\', out); fputc(next, out); i++;
            } else { fprintf(out, "\\\\"); }
        } else { fputc(c, out); }
    }
}

/* Emit all runtime extern declarations */
static void emit_runtime_decls(CEmitter *emit) {
    fprintf(emit->out,
        "/* Runtime */\n"
        "#include <string.h>\n"
        "static inline int64_t mix_double_to_bits(double d) { int64_t i; memcpy(&i, &d, 8); return i; }\n"
        "static inline double mix_bits_to_double(int64_t i) { double d; memcpy(&d, &i, 8); return d; }\n"
        "extern void mix_set_args(int32_t, char **);\n"
        "extern void mix_print_int(int64_t);\n"
        "extern void mix_print_float(double);\n"
        "extern void mix_print_str(const char *);\n"
        "extern void mix_print_bool(int);\n"
        "extern void mix_print_list_int(const void *);\n"
        "extern void mix_print_list_str(const void *);\n"
        "extern void mix_print_list_float(const void *);\n"
        "extern void mix_print_list_bool(const void *);\n"
        "extern void mix_print_map(const void *);\n"
        "extern void mix_print_map_str(const void *);\n"
        "extern void mix_print_set(const void *);\n"
        "extern void mix_print_set_int(const void *);\n"
        "extern void mix_write_list_int(const void *);\n"
        "extern void mix_write_list_str(const void *);\n"
        "extern void mix_write_list_float(const void *);\n"
        "extern void mix_write_list_bool(const void *);\n"
        "extern void mix_write_map(const void *);\n"
        "extern void mix_write_map_str(const void *);\n"
        "extern void mix_write_set(const void *);\n"
        "extern void mix_write_set_int(const void *);\n"
        "extern void mix_write_int(int64_t);\n"
        "extern void mix_write_float(double);\n"
        "extern void mix_write_str(const char *);\n"
        "extern void mix_write_bool(int);\n"
        "extern void mix_write_newline(void);\n"
        "extern void mix_panic(const char *);\n"
        "extern void mix_assert(int32_t, const char *);\n"
        "extern void *mix_alloc(int64_t);\n"
        "extern void *mix_list_new(void);\n"
        "extern int64_t mix_list_len(const void *);\n"
        "extern void mix_list_push(void *, int64_t);\n"
        "extern int64_t mix_list_get(const void *, int64_t);\n"
        "extern void mix_list_set(void *, int64_t, int64_t);\n"
        "extern void *mix_list_slice(const void *, int64_t, int64_t, int32_t);\n"
        "extern int64_t mix_list_pop(void *);\n"
        "extern void mix_list_remove(void *, int64_t);\n"
        "extern void mix_list_insert(void *, int64_t, int64_t);\n"
        "extern void mix_list_sort(void *);\n"
        "extern void mix_list_sort_str(void *);\n"
        "extern void mix_list_sort_float(void *);\n"
        "extern void mix_list_reverse(void *);\n"
        "extern int32_t mix_list_contains(const void *, int64_t);\n"
        "extern int64_t mix_list_index_of(const void *, int64_t);\n"
        "extern void *mix_map_new(void);\n"
        "extern int64_t mix_map_len(const void *);\n"
        "extern void mix_map_set(void *, const char *, int64_t);\n"
        "extern int64_t mix_map_get(const void *, const char *);\n"
        "extern int32_t mix_map_has(const void *, const char *);\n"
        "extern void mix_map_remove(void *, const char *);\n"
        "extern void *mix_map_keys(const void *);\n"
        "extern void *mix_map_values(const void *);\n"
        "extern void *mix_set_new(void);\n"
        "extern int64_t mix_set_len(const void *);\n"
        "extern void mix_set_add(void *, const char *);\n"
        "extern void mix_set_add_int(void *, int64_t);\n"
        "extern void mix_set_remove(void *, const char *);\n"
        "extern void mix_set_remove_int(void *, int64_t);\n"
        "extern int32_t mix_set_has(const void *, const char *);\n"
        "extern int32_t mix_set_has_int(const void *, int64_t);\n"
        "extern void *mix_set_values(const void *);\n"
        "extern void *mix_set_values_int(const void *);\n"
        "extern void *mix_set_union(const void *, const void *);\n"
        "extern void *mix_set_intersect(const void *, const void *);\n"
        "extern void *mix_set_diff(const void *, const void *);\n"
        "extern void *mix_set_from_list(const void *);\n"
        "extern void *mix_set_from_list_int(const void *);\n"
        "extern int64_t mix_str_len(const char *);\n"
        "extern char *mix_str_upper(const char *);\n"
        "extern char *mix_str_lower(const char *);\n"
        "extern char *mix_str_trim(const char *);\n"
        "extern void *mix_str_split(const char *, const char *);\n"
        "extern int32_t mix_str_contains(const char *, const char *);\n"
        "extern int32_t mix_str_starts_with(const char *, const char *);\n"
        "extern int32_t mix_str_ends_with(const char *, const char *);\n"
        "extern char *mix_str_replace(const char *, const char *, const char *);\n"
        "extern char *mix_str_concat(const char *, const char *);\n"
        "extern char *mix_str_char_at(const char *, int64_t);\n"
        "extern char *mix_str_join(const void *, const char *);\n"
        "extern char *mix_str_reverse(const char *);\n"
        "extern char *mix_str_sort(const char *);\n"
        "extern int64_t mix_str_count(const char *, const char *);\n"
        "extern int64_t mix_ord(const char *);\n"
        "extern const char *mix_chr(int64_t);\n"
        "extern char *mix_str_slice(const char *, int64_t, int64_t);\n"
        "extern char *mix_str_repeat(const char *, int64_t);\n"
        "extern int64_t mix_str_index_of(const char *, const char *);\n"
        "extern char *mix_to_string_int(int64_t);\n"
        "extern char *mix_to_string_float(double);\n"
        "extern char *mix_to_string_bool(int);\n"
        "extern char *mix_to_string_list_int(const void *);\n"
        "extern char *mix_to_string_list_str(const void *);\n"
        "extern char *mix_to_string_list_float(const void *);\n"
        "extern char *mix_to_string_list_bool(const void *);\n"
        "extern char *mix_to_string_map(const void *);\n"
        "extern char *mix_to_string_map_str(const void *);\n"
        "extern char *mix_to_string_set(const void *);\n"
        "extern char *mix_to_string_set_int(const void *);\n"
        "extern int64_t mix_parse_int(const char *);\n"
        "extern double mix_parse_float(const char *);\n"
        "extern int64_t mix_file_open(const char *, const char *);\n"
        "extern char *mix_file_read(int64_t);\n"
        "extern void mix_file_write(int64_t, const char *);\n"
        "extern void mix_file_close(int64_t);\n"
        "extern char *mix_file_read_all(const char *);\n"
        "extern int32_t mix_file_write_all(const char *, const char *);\n"
        "extern int64_t mix_file_exists(const char *);\n"
        "extern void *mix_list_dir(const char *);\n"
        "extern double mix_math_sqrt(double);\n"
        "extern double mix_math_abs(double);\n"
        "extern double mix_math_pow(double, double);\n"
        "extern double mix_math_sin(double);\n"
        "extern double mix_math_cos(double);\n"
        "extern double mix_math_tan(double);\n"
        "extern double mix_math_log(double);\n"
        "extern double mix_math_floor(double);\n"
        "extern double mix_math_ceil(double);\n"
        "extern double mix_math_round(double);\n"
        "extern double mix_math_min(double, double);\n"
        "extern double mix_math_max(double, double);\n"
        "extern void mix_random_seed(int64_t);\n"
        "extern int64_t mix_random_int(void);\n"
        "extern double mix_random_float(void);\n"
        "extern int64_t mix_time_now_ms(void);\n"
        "extern char *mix_int_to_hex(int64_t);\n"
        "extern char *mix_int_to_bin(int64_t);\n"
        "extern int64_t mix_shell(const char *);\n"
        "extern char *mix_shell_output(const char *);\n"
        "extern char *mix_env(const char *);\n"
        "extern void mix_exit(int64_t);\n"
        "extern char *mix_getcwd(void);\n"
        "extern int64_t mix_mkdir(const char *);\n"
        "extern void *mix_args(void);\n"
        "extern void *mix_result_ok(int64_t);\n"
        "extern void *mix_result_err(int64_t);\n"
        "extern int64_t mix_result_is_ok(const void *);\n"
        "extern int64_t mix_result_unwrap(const void *);\n"
        "extern int64_t mix_result_unwrap_err(const void *);\n"
        "extern void *mix_optional_some(int64_t);\n"
        "extern void *mix_optional_none(void);\n"
        "extern int64_t mix_optional_has(const void *);\n"
        "extern int64_t mix_optional_get(const void *);\n"
        "extern void *mix_shared_new(int64_t);\n"
        "extern int64_t mix_shared_read(void *);\n"
        "extern void mix_shared_update(void *, void *);\n"
        "extern void *mix_task_spawn(void *, void *, int64_t);\n"
        "extern int64_t mix_task_wait(void *);\n"
        "extern void mix_zone_enter(void);\n"
        "extern void mix_zone_exit(void);\n"
        "extern void *mix_zone_alloc(int64_t);\n"
        "extern void mix_project_build(void *);\n"
        "extern void mix_free(void *);\n"
        "extern void *mix_bytes(int64_t);\n"
        "extern uint32_t mix_peek_u32(const void *);\n"
        "extern int64_t mix_peek_u32_at(const void *, int64_t);\n"
        "extern int64_t mix_peek_byte(const void *, int64_t);\n"
        "extern double mix_peek_f32(const void *, int64_t);\n"
        "extern void mix_memcpy(void *, const void *, int64_t);\n"
        "extern void mix_poke_f32(void *, int64_t, double);\n"
        "extern void mix_poke_u32(void *, int64_t, int64_t);\n"
        "extern void mix_poke_ptr(void *, int64_t, int64_t);\n"
        "extern int64_t mix_peek_ptr(const void *, int64_t);\n"
        "extern void *mix_pack2(void *, void *, int64_t);\n"
        "extern void *mix_pack3(void *, void *, void *, int64_t);\n"
        "extern void *mix_list_to_f32(void *);\n"
        "\n"
    );
}

/* Pre-scan AST to collect and name lambdas, then emit forward declarations */
static void prescan_lambdas(CEmitter *emit, AstNode *node) {
    if (!node) return;
    if (node->kind == NODE_LAMBDA) {
        char lname[64];
        snprintf(lname, sizeof(lname), "lambda_%d", emit->lambda_counter++);
        node->lambda.generated_name = arena_strdup(emit->arena, lname);
        if (emit->lambda_count < 256)
            emit->lambdas[emit->lambda_count++] = node;
        /* Don't recurse into lambda body — it's a separate scope */
        return;
    }
    /* Recurse into all child nodes */
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.decl_count; i++)
                prescan_lambdas(emit, node->program.decls[i]);
            break;
        case NODE_FN_DECL:
            prescan_lambdas(emit, node->fn_decl.body);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++)
                prescan_lambdas(emit, node->block.stmts[i]);
            break;
        case NODE_VAR_DECL:
            prescan_lambdas(emit, node->var_decl.init_expr);
            break;
        case NODE_ASSIGN:
            prescan_lambdas(emit, node->assign.value);
            break;
        case NODE_IF_STMT:
            prescan_lambdas(emit, node->if_stmt.condition);
            prescan_lambdas(emit, node->if_stmt.then_block);
            prescan_lambdas(emit, node->if_stmt.else_block);
            break;
        case NODE_WHILE_STMT:
            prescan_lambdas(emit, node->while_stmt.condition);
            prescan_lambdas(emit, node->while_stmt.body);
            break;
        case NODE_FOR_STMT:
            prescan_lambdas(emit, node->for_stmt.iterable);
            prescan_lambdas(emit, node->for_stmt.body);
            break;
        case NODE_EXPR_STMT:
            prescan_lambdas(emit, node->expr_stmt.expr);
            break;
        case NODE_DONE_STMT:
            prescan_lambdas(emit, node->done_stmt.value);
            break;
        case NODE_CALL_EXPR:
            for (int i = 0; i < node->call.arg_count; i++)
                prescan_lambdas(emit, node->call.args[i]);
            break;
        case NODE_METHOD_CALL:
            prescan_lambdas(emit, node->method_call.object);
            for (int i = 0; i < node->method_call.arg_count; i++)
                prescan_lambdas(emit, node->method_call.args[i]);
            break;
        case NODE_BINARY_EXPR:
            prescan_lambdas(emit, node->binary.left);
            prescan_lambdas(emit, node->binary.right);
            break;
        case NODE_UNARY_EXPR:
            prescan_lambdas(emit, node->unary.operand);
            break;
        case NODE_SHAPE_DECL:
            for (int i = 0; i < node->shape_decl.method_count; i++)
                prescan_lambdas(emit, node->shape_decl.methods[i]);
            break;
        case NODE_COND_DECL:
            for (int i = 0; i < node->cond_decl.decl_count; i++)
                prescan_lambdas(emit, node->cond_decl.decls[i]);
            break;
        case NODE_SHAPE_LIT:
            for (int i = 0; i < node->shape_lit.field_count; i++)
                prescan_lambdas(emit, node->shape_lit.field_values[i]);
            break;
        default: break;
    }
}
static int emit_expr(CEmitter *emit, AstNode *expr);

static int emit_expr(CEmitter *emit, AstNode *expr) {
    if (!expr) return -1;

    switch (expr->kind) {
        case NODE_INT_LIT: {
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "int64_t t%d = %" PRId64 ";\n", t, expr->int_lit.value);
            return t;
        }
        case NODE_FLOAT_LIT: {
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "double t%d = %.17g;\n", t, expr->float_lit.value);
            return t;
        }
        case NODE_STRING_LIT: {
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "const char *t%d = \"", t);
            c_escape_string(emit->out, expr->string_lit.value, expr->string_lit.length);
            fprintf(emit->out, "\";\n");
            return t;
        }
        case NODE_STRING_INTERP: {
            // Build a single string by chaining mix_str_concat calls.
            // Each piece (literal or expression) produces a const char *
            // local; the accumulator threads them together.
            int acc = -1;
            for (int si = 0; si <= expr->string_interp.expr_count; si++) {
                if (expr->string_interp.part_lengths[si] > 0) {
                    int st = next_temp(emit);
                    ind(emit); fprintf(emit->out, "const char *t%d = \"", st);
                    c_escape_string(emit->out, expr->string_interp.parts[si],
                                    expr->string_interp.part_lengths[si]);
                    fprintf(emit->out, "\";\n");
                    if (acc < 0) {
                        acc = st;
                    } else {
                        int nxt = next_temp(emit);
                        ind(emit); fprintf(emit->out,
                            "const char *t%d = mix_str_concat((const char *)t%d, (const char *)t%d);\n",
                            nxt, acc, st);
                        acc = nxt;
                    }
                }
                if (si < expr->string_interp.expr_count) {
                    AstNode *iexpr = expr->string_interp.exprs[si];
                    int ev = emit_expr(emit, iexpr);
                    MixType *etype = iexpr->resolved_type;
                    int sv = next_temp(emit);
                    if (etype && etype->kind == TYPE_STR) {
                        ind(emit); fprintf(emit->out, "const char *t%d = (const char *)t%d;\n", sv, ev);
                    } else if (etype && type_is_float(etype)) {
                        ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_float((double)t%d);\n", sv, ev);
                    } else if (etype && etype->kind == TYPE_BOOL) {
                        ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_bool((int)t%d);\n", sv, ev);
                    } else if (etype && etype->kind == TYPE_LIST) {
                        MixType *elem = etype->list.elem_type;
                        if (elem && elem->kind == TYPE_STR) {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_list_str(t%d);\n", sv, ev);
                        } else if (elem && type_is_float(elem)) {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_list_float(t%d);\n", sv, ev);
                        } else if (elem && elem->kind == TYPE_BOOL) {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_list_bool(t%d);\n", sv, ev);
                        } else {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_list_int(t%d);\n", sv, ev);
                        }
                    } else if (etype && etype->kind == TYPE_MAP) {
                        MixType *vt = etype->map.val_type;
                        if (vt && vt->kind == TYPE_STR) {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_map_str(t%d);\n", sv, ev);
                        } else {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_map(t%d);\n", sv, ev);
                        }
                    } else if (etype && etype->kind == TYPE_SET) {
                        MixType *se = etype->set.elem_type;
                        if (se && type_is_integer(se)) {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_set_int(t%d);\n", sv, ev);
                        } else {
                            ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_set(t%d);\n", sv, ev);
                        }
                    } else {
                        ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_int((int64_t)t%d);\n", sv, ev);
                    }
                    if (acc < 0) {
                        acc = sv;
                    } else {
                        int nxt = next_temp(emit);
                        ind(emit); fprintf(emit->out,
                            "const char *t%d = mix_str_concat((const char *)t%d, (const char *)t%d);\n",
                            nxt, acc, sv);
                        acc = nxt;
                    }
                }
            }
            if (acc < 0) {
                acc = next_temp(emit);
                ind(emit); fprintf(emit->out, "const char *t%d = \"\";\n", acc);
            }
            return acc;
        }
        case NODE_BOOL_LIT: {
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "int32_t t%d = %d;\n", t, expr->bool_lit.value ? 1 : 0);
            return t;
        }
        case NODE_NONE_LIT: {
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_optional_none();\n", t);
            return t;
        }
        case NODE_IDENT: {
            /* Check compile-time constants */
            for (int i = 0; i < emit->const_count; i++) {
                if (strcmp(emit->constants[i].name, expr->ident.name) == 0) {
                    if (emit->constants[i].cached_temp >= 0)
                        return emit->constants[i].cached_temp;
                    int ct = emit_expr(emit, emit->constants[i].value);
                    emit->constants[i].cached_temp = ct;
                    return ct;
                }
            }
            /* Check shape field reference inside method */
            if (emit->current_shape) {
                ShapeFieldInfo *fi = type_find_field(emit->current_shape, expr->ident.name);
                if (fi) {
                    int t = next_temp(emit);
                    // Sub-shapes are stored inline; expose their address rather
                    // than scalar-loading 8 bytes (which would compile-error
                    // and at best read a pointer-sized chunk of an aggregate).
                    if (fi->type && fi->type->kind == TYPE_SHAPE) {
                        ind(emit); fprintf(emit->out, "%s *t%d = &v_self->%s;\n",
                                fi->type->shape.name, t, fi->name);
                        return t;
                    }
                    const char *fty = c_type(fi->type);
                    ind(emit); fprintf(emit->out, "%s t%d = v_self->%s;\n", fty, t, fi->name);
                    return t;
                }
            }
            int t = next_temp(emit);
            MixType *itype = expr->resolved_type;
            const char *cn = cname_ref(emit, expr->ident.name);
            // Top-level function used as a value (callback): emit the function
            // symbol address. Mirrors the QBE backend; gate on resolved_type to
            // respect local/param shadowing of builtin function names.
            if (itype && itype->kind == TYPE_FUNC) {
                Symbol *fsym = symtab_lookup(emit->symtab, expr->ident.name);
                if (fsym && fsym->type && fsym->type->kind == TYPE_FUNC) {
                    ind(emit); fprintf(emit->out, "void *t%d = (void *)%s;\n", t, cn);
                    return t;
                }
            }
            if (itype && itype->kind == TYPE_SHAPE) {
                ind(emit); fprintf(emit->out, "%s *t%d = (%s *)v_%s;\n",
                        itype->shape.name, t, itype->shape.name, cn);
            } else {
                const char *ty = c_type(itype);
                if (!itype || itype->kind == TYPE_VOID) ty = "int64_t";
                ind(emit); fprintf(emit->out, "%s t%d = (%s)v_%s;\n", ty, t, ty, cn);
            }
            return t;
        }
        case NODE_ELSE_EXPR: {
            MixType *val_type = expr->else_expr.value->resolved_type;
            bool is_result = val_type && val_type->kind == TYPE_RESULT;
            const char *rty = c_type(expr->resolved_type);
            /* Guard against void result type */
            if (!expr->resolved_type || expr->resolved_type->kind == TYPE_VOID)
                rty = "int64_t";
            int result = next_temp(emit);
            ind(emit); fprintf(emit->out, "%s t%d;\n", rty, result);
            int opt = emit_expr(emit, expr->else_expr.value);
            int has = next_temp(emit);
            if (is_result) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_result_is_ok((void *)t%d);\n", has, opt);
            } else {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_optional_has((void *)t%d);\n", has, opt);
            }
            ind(emit); fprintf(emit->out, "if (t%d) {\n", has);
            emit->indent++;
            if (is_result) {
                int val = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_result_unwrap((void *)t%d);\n", val, opt);
                MixType *ok_type = is_result ? val_type->result.ok_type : NULL;
                if (ok_type && type_is_float(ok_type)) {
                    int dval = next_temp(emit);
                    ind(emit); fprintf(emit->out, "double t%d; memcpy(&t%d, &t%d, sizeof(double));\n", dval, dval, val);
                    ind(emit); fprintf(emit->out, "t%d = t%d;\n", result, dval);
                } else {
                    ind(emit); fprintf(emit->out, "t%d = (%s)t%d;\n", result, rty, val);
                }
            } else {
                int val = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_optional_get((void *)t%d);\n", val, opt);
                ind(emit); fprintf(emit->out, "t%d = (%s)t%d;\n", result, rty, val);
            }
            emit->indent--;
            ind(emit); fprintf(emit->out, "} else {\n");
            emit->indent++;
            int fb = emit_expr(emit, expr->else_expr.fallback);
            ind(emit); fprintf(emit->out, "t%d = (%s)t%d;\n", result, rty, fb);
            emit->indent--;
            ind(emit); fprintf(emit->out, "}\n");
            return result;
        }
        case NODE_TRY_EXPR: {
            MixType *inner_type = expr->try_expr.expr->resolved_type;
            int val = emit_expr(emit, expr->try_expr.expr);
            bool is_result = inner_type && inner_type->kind == TYPE_RESULT;
            int has = next_temp(emit);
            if (is_result) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_result_is_ok((void *)t%d);\n", has, val);
            } else {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_optional_has((void *)t%d);\n", has, val);
            }
            ind(emit); fprintf(emit->out, "if (!t%d) {\n", has);
            emit->indent++;
            if (is_result) {
                ind(emit); fprintf(emit->out, "return (void *)t%d;\n", val);
            } else {
                ind(emit); fprintf(emit->out, "return mix_result_err((int64_t)\"none\");\n");
            }
            emit->indent--;
            ind(emit); fprintf(emit->out, "}\n");
            int t = next_temp(emit);
            if (is_result) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_result_unwrap((void *)t%d);\n", t, val);
            } else {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_optional_get((void *)t%d);\n", t, val);
            }
            return t;
        }
        case NODE_LIST_LIT: {
            int list_t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_list_new();\n", list_t);
            for (int i = 0; i < expr->list_lit.element_count; i++) {
                int val = emit_expr(emit, expr->list_lit.elements[i]);
                MixType *etype = expr->list_lit.elements[i]->resolved_type;
                if (etype && type_is_float(etype))
                    { ind(emit); fprintf(emit->out, "mix_list_push(t%d, mix_double_to_bits(t%d));\n", list_t, val); }
                else
                    { ind(emit); fprintf(emit->out, "mix_list_push(t%d, (int64_t)t%d);\n", list_t, val); }
            }
            return list_t;
        }
        case NODE_MAP_LIT: {
            int map_t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_map_new();\n", map_t);
            for (int i = 0; i < expr->map_lit.entry_count; i++) {
                int key = emit_expr(emit, expr->map_lit.keys[i]);
                int val = emit_expr(emit, expr->map_lit.values[i]);
                ind(emit); fprintf(emit->out, "mix_map_set(t%d, (const char *)t%d, (int64_t)t%d);\n",
                        map_t, key, val);
            }
            return map_t;
        }
        case NODE_SET_LIT: {
            int set_t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_set_new();\n", set_t);
            MixType *set_elem = expr->resolved_type ? expr->resolved_type->set.elem_type : NULL;
            bool int_set = set_elem && type_is_integer(set_elem);
            for (int i = 0; i < expr->set_lit.element_count; i++) {
                int elem = emit_expr(emit, expr->set_lit.elements[i]);
                if (int_set) {
                    ind(emit); fprintf(emit->out, "mix_set_add_int(t%d, (int64_t)t%d);\n", set_t, elem);
                } else {
                    ind(emit); fprintf(emit->out, "mix_set_add(t%d, (const char *)t%d);\n", set_t, elem);
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
                case TOK_INT: case TOK_INT64:
                    ind(emit); fprintf(emit->out, "int64_t t%d = (int64_t)t%d;\n", t, val); break;
                case TOK_UINT64:
                    ind(emit); fprintf(emit->out, "uint64_t t%d = (uint64_t)t%d;\n", t, val); break;
                case TOK_INT32:
                    ind(emit); fprintf(emit->out, "int32_t t%d = (int32_t)t%d;\n", t, val); break;
                case TOK_UINT32:
                    ind(emit); fprintf(emit->out, "uint32_t t%d = (uint32_t)t%d;\n", t, val); break;
                case TOK_INT16:
                    ind(emit); fprintf(emit->out, "int16_t t%d = (int16_t)t%d;\n", t, val); break;
                case TOK_UINT16:
                    ind(emit); fprintf(emit->out, "uint16_t t%d = (uint16_t)t%d;\n", t, val); break;
                case TOK_INT8:
                    ind(emit); fprintf(emit->out, "int8_t t%d = (int8_t)t%d;\n", t, val); break;
                case TOK_UINT8: case TOK_BYTE:
                    ind(emit); fprintf(emit->out, "uint8_t t%d = (uint8_t)t%d;\n", t, val); break;
                case TOK_FLOAT: case TOK_FLOAT64:
                    if (src_int) {
                        ind(emit); fprintf(emit->out, "double t%d = (double)t%d;\n", t, val);
                    } else if (src && src->kind == TYPE_FLOAT32) {
                        ind(emit); fprintf(emit->out, "double t%d = (double)t%d;\n", t, val);
                    } else {
                        ind(emit); fprintf(emit->out, "double t%d = t%d;\n", t, val);
                    }
                    break;
                case TOK_FLOAT32:
                    ind(emit); fprintf(emit->out, "float t%d = (float)t%d;\n", t, val); break;
                case TOK_BOOL:
                    if (src_float) {
                        ind(emit); fprintf(emit->out, "int32_t t%d = (t%d != 0.0);\n", t, val);
                    } else {
                        ind(emit); fprintf(emit->out, "int32_t t%d = (t%d != 0);\n", t, val);
                    }
                    break;
                default:
                    ind(emit); fprintf(emit->out, "int64_t t%d = (int64_t)t%d;\n", t, val); break;
            }
            (void)src_float; (void)src_int;
            return t;
        }
        case NODE_INDEX_EXPR: {
            int obj = emit_expr(emit, expr->index_expr.object);
            int idx = emit_expr(emit, expr->index_expr.index);
            MixType *obj_type = expr->index_expr.object->resolved_type;
            int t = next_temp(emit);
            if (obj_type && obj_type->kind == TYPE_MAP) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_map_get(t%d, (const char *)t%d);\n",
                        t, obj, idx);
            } else {
                MixType *elem = (obj_type && obj_type->kind == TYPE_LIST) ? obj_type->list.elem_type : NULL;
                if (elem && type_is_float(elem)) {
                    ind(emit); fprintf(emit->out, "double t%d = mix_bits_to_double(mix_list_get(t%d, (int64_t)t%d));\n",
                            t, obj, idx);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_get(t%d, (int64_t)t%d);\n",
                            t, obj, idx);
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
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", start_v);
            }
            if (expr->slice_expr.end) {
                end_v = emit_expr(emit, expr->slice_expr.end);
            } else {
                end_v = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_len(t%d);\n", end_v, obj);
            }
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_list_slice(t%d, (int64_t)t%d, (int64_t)t%d, %d);\n",
                    t, obj, start_v, end_v, expr->slice_expr.inclusive ? 1 : 0);
            return t;
        }
        case NODE_LIST_COMP: {
            int result_list = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_list_new();\n", result_list);
            int list_ptr = emit_expr(emit, expr->list_comp.iterable);
            int len_t = next_temp(emit);
            ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_len(t%d);\n", len_t, list_ptr);
            int idx_id = next_label(emit);
            ind(emit); fprintf(emit->out, "for (int64_t _cidx_%d = 0; _cidx_%d < t%d; _cidx_%d++) {\n",
                    idx_id, idx_id, len_t, idx_id);
            emit->indent++;
            ind(emit); fprintf(emit->out, "int64_t v_%s = mix_list_get(t%d, _cidx_%d);\n",
                    cname_decl(emit, expr->list_comp.var_name), list_ptr, idx_id);
            if (expr->list_comp.condition) {
                int cond_val = emit_expr(emit, expr->list_comp.condition);
                ind(emit); fprintf(emit->out, "if (t%d) {\n", cond_val);
                emit->indent++;
                int val = emit_expr(emit, expr->list_comp.expr);
                MixType *comp_etype = expr->list_comp.expr->resolved_type;
                if (comp_etype && type_is_float(comp_etype))
                    { ind(emit); fprintf(emit->out, "mix_list_push(t%d, mix_double_to_bits(t%d));\n", result_list, val); }
                else
                    { ind(emit); fprintf(emit->out, "mix_list_push(t%d, (int64_t)t%d);\n", result_list, val); }
                emit->indent--;
                ind(emit); fprintf(emit->out, "}\n");
            } else {
                int val = emit_expr(emit, expr->list_comp.expr);
                MixType *comp_etype = expr->list_comp.expr->resolved_type;
                if (comp_etype && type_is_float(comp_etype))
                    { ind(emit); fprintf(emit->out, "mix_list_push(t%d, mix_double_to_bits(t%d));\n", result_list, val); }
                else
                    { ind(emit); fprintf(emit->out, "mix_list_push(t%d, (int64_t)t%d);\n", result_list, val); }
            }
            emit->indent--;
            ind(emit); fprintf(emit->out, "}\n");
            return result_list;
        }
        case NODE_LAMBDA: {
            char lname[64];
            snprintf(lname, sizeof(lname), "lambda_%d", emit->lambda_counter++);
            expr->lambda.generated_name = arena_strdup(emit->arena, lname);
            if (emit->lambda_count < 256)
                emit->lambdas[emit->lambda_count++] = expr;
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = (void *)%s;\n", t, lname);
            return t;
        }
        case NODE_BINARY_EXPR: {
            /* Constant folding: int literals */
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
                    ind(emit); fprintf(emit->out, "int64_t t%d = %" PRId64 ";\n", t, result);
                    return t;
                }
            }
            /* Constant folding: float literals */
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
                    ind(emit); fprintf(emit->out, "double t%d = %.17g;\n", t, result);
                    return t;
                }
            }

            int left = emit_expr(emit, expr->binary.left);
            int right = emit_expr(emit, expr->binary.right);
            int t = next_temp(emit);
            MixType *ltype = expr->binary.left->resolved_type;

            /* String concatenation */
            if (ltype && ltype->kind == TYPE_STR && expr->binary.op == TOK_PLUS) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_str_concat((const char *)t%d, (const char *)t%d);\n",
                        t, left, right);
                return t;
            }

            /* String comparison via strcmp */
            if (ltype && ltype->kind == TYPE_STR) {
                const char *cmp_op = NULL;
                switch (expr->binary.op) {
                    case TOK_EQEQ: cmp_op = "== 0"; break;
                    case TOK_NEQ:  cmp_op = "!= 0"; break;
                    case TOK_LT:   cmp_op = "< 0"; break;
                    case TOK_GT:   cmp_op = "> 0"; break;
                    case TOK_LTE:  cmp_op = "<= 0"; break;
                    case TOK_GTE:  cmp_op = ">= 0"; break;
                    default: break;
                }
                if (cmp_op) {
                    ind(emit); fprintf(emit->out, "int32_t t%d = (strcmp((const char *)t%d, (const char *)t%d) %s);\n",
                            t, left, right, cmp_op);
                    return t;
                }
            }

            /* Shape operator overloading */
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
                        const char *rty = c_type(expr->resolved_type);
                        if (expr->resolved_type && expr->resolved_type->kind == TYPE_SHAPE)
                            rty = "void *";
                        MixType *rtype = expr->binary.right->resolved_type;
                        ind(emit); fprintf(emit->out, "%s t%d = %s((%s *)t%d, ",
                                rty, t, mangled, ltype->shape.name, left);
                        if (rtype && rtype->kind == TYPE_SHAPE)
                            fprintf(emit->out, "(%s *)t%d", rtype->shape.name, right);
                        else if (rtype && type_is_float(rtype))
                            fprintf(emit->out, "(double)t%d", right);
                        else
                            fprintf(emit->out, "(int64_t)t%d", right);
                        fprintf(emit->out, ");\n");
                        return t;
                    }
                }
            }

            bool is_flt = ltype && type_is_float(ltype);
            const char *ty = c_type(ltype);
            const char *resty = c_type(expr->resolved_type);

            switch (expr->binary.op) {
                case TOK_PLUS:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d + t%d;\n", ty, t, left, right); break;
                case TOK_MINUS:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d - t%d;\n", ty, t, left, right); break;
                case TOK_STAR:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d * t%d;\n", ty, t, left, right); break;
                case TOK_SLASH:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d / t%d;\n", ty, t, left, right); break;
                case TOK_PERCENT:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d %% t%d;\n", ty, t, left, right); break;
                case TOK_PIPE:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d | t%d;\n", ty, t, left, right); break;
                case TOK_EQEQ:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d == t%d);\n", resty, t, left, right); break;
                case TOK_NEQ:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d != t%d);\n", resty, t, left, right); break;
                case TOK_LT:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d < t%d);\n", resty, t, left, right); break;
                case TOK_GT:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d > t%d);\n", resty, t, left, right); break;
                case TOK_LTE:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d <= t%d);\n", resty, t, left, right); break;
                case TOK_GTE:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d >= t%d);\n", resty, t, left, right); break;
                case TOK_AND:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d && t%d);\n", resty, t, left, right); break;
                case TOK_OR:
                    ind(emit); fprintf(emit->out, "%s t%d = (t%d || t%d);\n", resty, t, left, right); break;
                default:
                    ind(emit); fprintf(emit->out, "%s t%d = t%d;\n", ty, t, left); break;
            }
            (void)is_flt;
            return t;
        }
        case NODE_UNARY_EXPR: {
            int operand = emit_expr(emit, expr->unary.operand);
            int t = next_temp(emit);
            const char *ty = c_type(expr->unary.operand->resolved_type);
            if (expr->unary.op == TOK_MINUS) {
                ind(emit); fprintf(emit->out, "%s t%d = -t%d;\n", ty, t, operand);
            } else if (expr->unary.op == TOK_NOT) {
                ind(emit); fprintf(emit->out, "int32_t t%d = !t%d;\n", t, operand);
            } else if (expr->unary.op == TOK_AMPERSAND) {
                AstNode *inner = expr->unary.operand;
                if (inner->kind == NODE_IDENT && inner->ident.is_mutable) {
                    ind(emit); fprintf(emit->out, "void *t%d = &v_%s;\n", t,
                            cname_ref(emit, inner->ident.name));
                } else {
                    ind(emit); fprintf(emit->out, "int64_t _addr_%d = (int64_t)t%d;\n", t, operand);
                    ind(emit); fprintf(emit->out, "void *t%d = &_addr_%d;\n", t, t);
                }
            } else {
                ind(emit); fprintf(emit->out, "%s t%d = t%d;\n", ty, t, operand);
            }
            return t;
        }
        case NODE_CALL_EXPR: {
            /* Emit arguments first */
            if (expr->call.arg_count > 64) {
                mix_error(expr->loc, "too many function arguments (max 64)");
                return -1;
            }
            int arg_temps[64];
            for (int i = 0; i < expr->call.arg_count; i++) {
                arg_temps[i] = emit_expr(emit, expr->call.args[i]);
            }
            int t = next_temp(emit);

            /* print() */
            if (strcmp(expr->call.name, "print") == 0 && expr->call.arg_count == 1) {
                AstNode *arg = expr->call.args[0];
                if (arg->kind == NODE_STRING_INTERP) {
                    ind(emit); fprintf(emit->out, "mix_print_str((const char *)t%d);\n", arg_temps[0]);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                    return t;
                }
                MixType *atype = arg->resolved_type;
                if (atype && atype->kind == TYPE_STR) {
                    ind(emit); fprintf(emit->out, "mix_print_str((const char *)t%d);\n", arg_temps[0]);
                } else if (atype && type_is_float(atype)) {
                    if (atype->kind == TYPE_FLOAT32) {
                        ind(emit); fprintf(emit->out, "mix_print_float((double)t%d);\n", arg_temps[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "mix_print_float(t%d);\n", arg_temps[0]);
                    }
                } else if (atype && atype->kind == TYPE_BOOL) {
                    ind(emit); fprintf(emit->out, "mix_print_bool((int)t%d);\n", arg_temps[0]);
                } else if (atype && atype->kind == TYPE_LIST) {
                    MixType *elem = atype->list.elem_type;
                    if (elem && elem->kind == TYPE_STR) {
                        ind(emit); fprintf(emit->out, "mix_print_list_str(t%d);\n", arg_temps[0]);
                    } else if (elem && type_is_float(elem)) {
                        ind(emit); fprintf(emit->out, "mix_print_list_float(t%d);\n", arg_temps[0]);
                    } else if (elem && elem->kind == TYPE_BOOL) {
                        ind(emit); fprintf(emit->out, "mix_print_list_bool(t%d);\n", arg_temps[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "mix_print_list_int(t%d);\n", arg_temps[0]);
                    }
                } else if (atype && atype->kind == TYPE_MAP) {
                    MixType *val_elem = atype->map.val_type;
                    if (val_elem && val_elem->kind == TYPE_STR) {
                        ind(emit); fprintf(emit->out, "mix_print_map_str(t%d);\n", arg_temps[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "mix_print_map(t%d);\n", arg_temps[0]);
                    }
                } else if (atype && atype->kind == TYPE_SET) {
                    MixType *selem = atype->set.elem_type;
                    if (selem && type_is_integer(selem)) {
                        ind(emit); fprintf(emit->out, "mix_print_set_int(t%d);\n", arg_temps[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "mix_print_set(t%d);\n", arg_temps[0]);
                    }
                } else {
                    ind(emit); fprintf(emit->out, "mix_print_int((int64_t)t%d);\n", arg_temps[0]);
                }
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }

            /* File I/O builtins */
            if (strcmp(expr->call.name, "file_open") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_file_open((const char *)t%d, (const char *)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "file_read") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_file_read((int64_t)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_write") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "mix_file_write((int64_t)t%d, (const char *)t%d);\n",
                        arg_temps[0], arg_temps[1]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "file_close") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "mix_file_close((int64_t)t%d);\n", arg_temps[0]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "file_read_all") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_file_read_all((const char *)t%d);\n",
                        t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_write_all") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "int32_t t%d = mix_file_write_all((const char *)t%d, (const char *)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }

            /* OS builtins */
            if (strcmp(expr->call.name, "shell") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_shell((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "shell_output") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_shell_output((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "file_exists") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_file_exists((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "list_dir") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_list_dir((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "env") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_env((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "exit") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "mix_exit((int64_t)t%d);\n", arg_temps[0]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "getcwd") == 0 && expr->call.arg_count == 0) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_getcwd();\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "mkdir") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_mkdir((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "args") == 0 && expr->call.arg_count == 0) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_args();\n", t);
                return t;
            }

            /* Math builtins (1 arg) */
            {
                const char *math1[] = {"sqrt","abs","sin","cos","tan","log","floor","ceil","round"};
                const char *rt1[]   = {"mix_math_sqrt","mix_math_abs","mix_math_sin","mix_math_cos",
                                       "mix_math_tan","mix_math_log","mix_math_floor","mix_math_ceil",
                                       "mix_math_round"};
                for (int mi = 0; mi < 9; mi++) {
                    if (strcmp(expr->call.name, math1[mi]) == 0 && expr->call.arg_count == 1) {
                        MixType *atype = expr->call.args[0]->resolved_type;
                        int arg = arg_temps[0];
                        if (atype && type_is_integer(atype)) {
                            int conv = next_temp(emit);
                            ind(emit); fprintf(emit->out, "double t%d = (double)t%d;\n", conv, arg);
                            arg = conv;
                        }
                        ind(emit); fprintf(emit->out, "double t%d = %s(t%d);\n", t, rt1[mi], arg);
                        return t;
                    }
                }
            }
            /* Math builtins (2 args) */
            {
                const char *math2[] = {"pow","min","max"};
                const char *rt2[]   = {"mix_math_pow","mix_math_min","mix_math_max"};
                for (int mi = 0; mi < 3; mi++) {
                    if (strcmp(expr->call.name, math2[mi]) == 0 && expr->call.arg_count == 2) {
                        MixType *at0 = expr->call.args[0]->resolved_type;
                        MixType *at1 = expr->call.args[1]->resolved_type;
                        int a0 = arg_temps[0], a1 = arg_temps[1];
                        if (at0 && type_is_integer(at0)) {
                            int conv = next_temp(emit);
                            ind(emit); fprintf(emit->out, "double t%d = (double)t%d;\n", conv, a0);
                            a0 = conv;
                        }
                        if (at1 && type_is_integer(at1)) {
                            int conv = next_temp(emit);
                            ind(emit); fprintf(emit->out, "double t%d = (double)t%d;\n", conv, a1);
                            a1 = conv;
                        }
                        ind(emit); fprintf(emit->out, "double t%d = %s(t%d, t%d);\n", t, rt2[mi], a0, a1);
                        return t;
                    }
                }
            }

            /* to_string */
            if (strcmp(expr->call.name, "to_string") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                if (atype && type_is_float(atype)) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_float(t%d);\n", t, arg_temps[0]);
                } else {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_to_string_int((int64_t)t%d);\n", t, arg_temps[0]);
                }
                return t;
            }
            /* str_reverse, str_count */
            if (strcmp(expr->call.name, "str_reverse") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_str_reverse((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "str_count") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_str_count((const char *)t%d, (const char *)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }

            /* to_int */
            if (strcmp(expr->call.name, "to_int") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                if (atype && type_is_float(atype)) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = (int64_t)t%d;\n", t, arg_temps[0]);
                } else if (atype && atype->kind == TYPE_STR) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_parse_int((const char *)t%d);\n", t, arg_temps[0]);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = (int64_t)t%d;\n", t, arg_temps[0]);
                }
                return t;
            }

            /* to_float */
            if (strcmp(expr->call.name, "to_float") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                if (atype && type_is_integer(atype)) {
                    ind(emit); fprintf(emit->out, "double t%d = (double)t%d;\n", t, arg_temps[0]);
                } else if (atype && atype->kind == TYPE_STR) {
                    ind(emit); fprintf(emit->out, "double t%d = mix_parse_float((const char *)t%d);\n", t, arg_temps[0]);
                } else {
                    ind(emit); fprintf(emit->out, "double t%d = t%d;\n", t, arg_temps[0]);
                }
                return t;
            }

            /* to_set */
            if (strcmp(expr->call.name, "to_set") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                MixType *elem = (atype && atype->kind == TYPE_LIST) ? atype->list.elem_type : NULL;
                if (elem && elem->kind == TYPE_STR) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_from_list(t%d);\n", t, arg_temps[0]);
                } else {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_from_list_int(t%d);\n", t, arg_temps[0]);
                }
                return t;
            }

            /* ord(s) -> int */
            if (strcmp(expr->call.name, "ord") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_ord((const char *)t%d);\n", t, arg_temps[0]);
                return t;
            }

            /* chr(n) -> str */
            if (strcmp(expr->call.name, "chr") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_chr((int64_t)t%d);\n", t, arg_temps[0]);
                return t;
            }

            /* panic(msg) — abort */
            if (strcmp(expr->call.name, "panic") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "mix_panic((const char *)t%d);\n", arg_temps[0]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }

            /* assert(cond, msg) — abort if cond false */
            if (strcmp(expr->call.name, "assert") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "mix_assert((int32_t)t%d, (const char *)t%d);\n",
                                  arg_temps[0], arg_temps[1]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }

            /* len(x) -> int  polymorphic */
            if (strcmp(expr->call.name, "len") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                const char *fn = "mix_list_len";
                if (atype && atype->kind == TYPE_STR) fn = "mix_str_len";
                else if (atype && atype->kind == TYPE_MAP) fn = "mix_map_len";
                else if (atype && atype->kind == TYPE_SET) fn = "mix_set_len";
                ind(emit); fprintf(emit->out, "int64_t t%d = %s((const void *)t%d);\n",
                                  t, fn, arg_temps[0]);
                return t;
            }

            /* type_of(x) -> str  (compile-time) */
            if (strcmp(expr->call.name, "type_of") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                const char *tname = atype ? type_kind_name(atype->kind) : "unknown";
                ind(emit); fprintf(emit->out, "const char *t%d = \"%s\";\n", t, tname);
                return t;
            }

            /* sizeof(x) -> int  (compile-time) */
            if (strcmp(expr->call.name, "sizeof") == 0 && expr->call.arg_count == 1) {
                MixType *atype = expr->call.args[0]->resolved_type;
                int sz = atype ? type_size(atype) : 8;
                ind(emit); fprintf(emit->out, "int64_t t%d = %d;\n", t, sz);
                return t;
            }

            /* Memory builtins */
            if (strcmp(expr->call.name, "alloc") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_alloc((int64_t)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "bytes") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_bytes((int64_t)t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "peek_u32") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_peek_u32_at(t%d, (int64_t)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "peek_byte") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_peek_byte(t%d, (int64_t)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "peek_f32") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "double t%d = mix_peek_f32(t%d, (int64_t)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "memcpy") == 0 && expr->call.arg_count == 3) {
                ind(emit); fprintf(emit->out, "mix_memcpy(t%d, t%d, (int64_t)t%d);\n",
                        arg_temps[0], arg_temps[1], arg_temps[2]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "poke_f32") == 0 && expr->call.arg_count == 3) {
                ind(emit); fprintf(emit->out, "mix_poke_f32(t%d, (int64_t)t%d, (double)t%d);\n",
                        arg_temps[0], arg_temps[1], arg_temps[2]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "poke_u32") == 0 && expr->call.arg_count == 3) {
                ind(emit); fprintf(emit->out, "mix_poke_u32(t%d, (int64_t)t%d, (int64_t)t%d);\n",
                        arg_temps[0], arg_temps[1], arg_temps[2]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "poke_ptr") == 0 && expr->call.arg_count == 3) {
                ind(emit); fprintf(emit->out, "mix_poke_ptr(t%d, (int64_t)t%d, (int64_t)t%d);\n",
                        arg_temps[0], arg_temps[1], arg_temps[2]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "peek_ptr") == 0 && expr->call.arg_count == 2) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_peek_ptr(t%d, (int64_t)t%d);\n",
                        t, arg_temps[0], arg_temps[1]);
                return t;
            }
            if (strcmp(expr->call.name, "pack2") == 0 && expr->call.arg_count == 3) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_pack2(t%d, t%d, (int64_t)t%d);\n",
                        t, arg_temps[0], arg_temps[1], arg_temps[2]);
                return t;
            }
            if (strcmp(expr->call.name, "pack3") == 0 && expr->call.arg_count == 4) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_pack3(t%d, t%d, t%d, (int64_t)t%d);\n",
                        t, arg_temps[0], arg_temps[1], arg_temps[2], arg_temps[3]);
                return t;
            }
            if (strcmp(expr->call.name, "list_to_f32") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "void *t%d = mix_list_to_f32(t%d);\n", t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "free_mem") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "mix_free(t%d);\n", arg_temps[0]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "random_seed") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "mix_random_seed((int64_t)t%d);\n", arg_temps[0]);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "random_int") == 0 && expr->call.arg_count == 0) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_random_int();\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "random_float") == 0 && expr->call.arg_count == 0) {
                ind(emit); fprintf(emit->out, "double t%d = mix_random_float();\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "time_now_ms") == 0 && expr->call.arg_count == 0) {
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_time_now_ms();\n", t);
                return t;
            }
            if (strcmp(expr->call.name, "int_to_hex") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_int_to_hex((int64_t)t%d);\n",
                        t, arg_temps[0]);
                return t;
            }
            if (strcmp(expr->call.name, "int_to_bin") == 0 && expr->call.arg_count == 1) {
                ind(emit); fprintf(emit->out, "const char *t%d = mix_int_to_bin((int64_t)t%d);\n",
                        t, arg_temps[0]);
                return t;
            }

            /* Regular / indirect function call */
            Symbol *fn_sym = symtab_lookup(emit->symtab, expr->call.name);
            MixType *fn_type = (fn_sym && fn_sym->type && fn_sym->type->kind == TYPE_FUNC)
                ? fn_sym->type : NULL;

            bool is_lambda_var = false;
            for (int fv = 0; fv < emit->fn_ptr_var_count; fv++) {
                if (strcmp(emit->fn_ptr_vars[fv], expr->call.name) == 0) {
                    is_lambda_var = true; break;
                }
            }
            if (!is_lambda_var) {
                if (!fn_sym) is_lambda_var = true;
                else if (fn_sym->type && fn_sym->type->kind != TYPE_FUNC)
                    is_lambda_var = true;
            }

            const char *ret_ty = c_type(expr->resolved_type);
            if (expr->resolved_type && expr->resolved_type->kind == TYPE_SHAPE)
                ret_ty = "void *";
            bool has_return = !(expr->resolved_type && expr->resolved_type->kind == TYPE_VOID);

            if (is_lambda_var) {
                /* Build function pointer type and call */
                ind(emit);
                if (has_return) fprintf(emit->out, "%s t%d = ", ret_ty, t);
                fprintf(emit->out, "((");
                /* Return type */
                if (has_return) fprintf(emit->out, "%s", ret_ty);
                else fprintf(emit->out, "void");
                fprintf(emit->out, " (*)(");
                for (int i = 0; i < expr->call.arg_count; i++) {
                    if (i > 0) fprintf(emit->out, ", ");
                    fprintf(emit->out, "int64_t");
                }
                if (expr->call.arg_count == 0) fprintf(emit->out, "void");
                fprintf(emit->out, "))v_%s)(", cname_ref(emit, expr->call.name));
                for (int i = 0; i < expr->call.arg_count; i++) {
                    if (i > 0) fprintf(emit->out, ", ");
                    fprintf(emit->out, "(int64_t)t%d", arg_temps[i]);
                }
                fprintf(emit->out, ");\n");
                if (!has_return) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
            } else {
                ind(emit);
                const char *call_name = (fn_sym && fn_sym->c_name) ? fn_sym->c_name : expr->call.name;
                if (has_return) {
                    fprintf(emit->out, "%s t%d = %s(", ret_ty, t, call_name);
                } else {
                    fprintf(emit->out, "%s(", call_name);
                }
                for (int i = 0; i < expr->call.arg_count; i++) {
                    if (i > 0) fprintf(emit->out, ", ");
                    MixType *pt = (fn_type && i < fn_type->func.param_count)
                        ? fn_type->func.param_types[i]
                        : expr->call.args[i]->resolved_type;
                    /* Cast arg to expected param type */
                    const char *cast_ty = c_type(pt);
                    if (pt && pt->kind == TYPE_SHAPE)
                        fprintf(emit->out, "(void *)t%d", arg_temps[i]);
                    else if (pt && type_is_float(pt))
                        fprintf(emit->out, "(double)t%d", arg_temps[i]);
                    else
                        fprintf(emit->out, "(%s)t%d", cast_ty, arg_temps[i]);
                }
                fprintf(emit->out, ");\n");
                if (!has_return) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
            }
            return t;
        }
        case NODE_METHOD_CALL: {
            int obj_temp = emit_expr(emit, expr->method_call.object);
            int arg_temps2[64];
            for (int i = 0; i < expr->method_call.arg_count; i++)
                arg_temps2[i] = emit_expr(emit, expr->method_call.args[i]);

            MixType *obj_type = expr->method_call.object->resolved_type;

            /* Shared methods */
            if (obj_type && obj_type->kind == TYPE_SHARED) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "read") == 0) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_shared_read(t%d);\n", t, obj_temp);
                } else if (strcmp(m, "update") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "mix_shared_update(t%d, t%d);\n", obj_temp, arg_temps2[0]);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }

            /* List methods */
            if (obj_type && obj_type->kind == TYPE_LIST) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "push") == 0 && expr->method_call.arg_count == 1) {
                    MixType *elem = obj_type->list.elem_type;
                    if (elem && type_is_float(elem))
                        { ind(emit); fprintf(emit->out, "mix_list_push(t%d, mix_double_to_bits(t%d));\n", obj_temp, arg_temps2[0]); }
                    else
                        { ind(emit); fprintf(emit->out, "mix_list_push(t%d, (int64_t)t%d);\n", obj_temp, arg_temps2[0]); }
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "pop") == 0) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_pop(t%d);\n", t, obj_temp);
                } else if (strcmp(m, "remove") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "mix_list_remove(t%d, (int64_t)t%d);\n", obj_temp, arg_temps2[0]);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "insert") == 0 && expr->method_call.arg_count == 2) {
                    ind(emit); fprintf(emit->out, "mix_list_insert(t%d, (int64_t)t%d, (int64_t)t%d);\n",
                            obj_temp, arg_temps2[0], arg_temps2[1]);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "sort") == 0) {
                    MixType *elem = obj_type->list.elem_type;
                    if (elem && elem->kind == TYPE_STR)
                        { ind(emit); fprintf(emit->out, "mix_list_sort_str(t%d);\n", obj_temp); }
                    else if (elem && type_is_float(elem))
                        { ind(emit); fprintf(emit->out, "mix_list_sort_float(t%d);\n", obj_temp); }
                    else
                        { ind(emit); fprintf(emit->out, "mix_list_sort(t%d);\n", obj_temp); }
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "reverse") == 0) {
                    ind(emit); fprintf(emit->out, "mix_list_reverse(t%d);\n", obj_temp);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "contains") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int32_t t%d = mix_list_contains(t%d, (int64_t)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "index_of") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_index_of(t%d, (int64_t)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "join") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_join(t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }

            /* String methods */
            if (obj_type && obj_type->kind == TYPE_STR) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "upper") == 0) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_upper((const char *)t%d);\n", t, obj_temp);
                } else if (strcmp(m, "lower") == 0) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_lower((const char *)t%d);\n", t, obj_temp);
                } else if (strcmp(m, "trim") == 0) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_trim((const char *)t%d);\n", t, obj_temp);
                } else if (strcmp(m, "split") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_str_split((const char *)t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "contains") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int32_t t%d = mix_str_contains((const char *)t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "starts_with") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int32_t t%d = mix_str_starts_with((const char *)t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "ends_with") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int32_t t%d = mix_str_ends_with((const char *)t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "replace") == 0 && expr->method_call.arg_count == 2) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_replace((const char *)t%d, (const char *)t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0], arg_temps2[1]);
                } else if (strcmp(m, "char_at") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_char_at((const char *)t%d, (int64_t)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "slice") == 0 && expr->method_call.arg_count == 2) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_slice((const char *)t%d, (int64_t)t%d, (int64_t)t%d);\n",
                            t, obj_temp, arg_temps2[0], arg_temps2[1]);
                } else if (strcmp(m, "repeat") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_repeat((const char *)t%d, (int64_t)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "reverse") == 0) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_reverse((const char *)t%d);\n", t, obj_temp);
                } else if (strcmp(m, "index_of") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_str_index_of((const char *)t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "code") == 0) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_ord((const char *)t%d);\n", t, obj_temp);
                } else if (strcmp(m, "sort") == 0) {
                    ind(emit); fprintf(emit->out, "const char *t%d = mix_str_sort((const char *)t%d);\n", t, obj_temp);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }

            /* Map methods */
            if (obj_type && obj_type->kind == TYPE_MAP) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                if (strcmp(m, "has") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "int32_t t%d = mix_map_has(t%d, (const char *)t%d);\n",
                            t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "remove") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "mix_map_remove(t%d, (const char *)t%d);\n", obj_temp, arg_temps2[0]);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }

            /* Set methods */
            if (obj_type && obj_type->kind == TYPE_SET) {
                const char *m = expr->method_call.method_name;
                int t = next_temp(emit);
                MixType *selem = obj_type->set.elem_type;
                bool is_int_set = selem && type_is_integer(selem);
                if (strcmp(m, "has") == 0 && expr->method_call.arg_count == 1) {
                    if (is_int_set) {
                        ind(emit); fprintf(emit->out, "int32_t t%d = mix_set_has_int(t%d, (int64_t)t%d);\n",
                                t, obj_temp, arg_temps2[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "int32_t t%d = mix_set_has(t%d, (const char *)t%d);\n",
                                t, obj_temp, arg_temps2[0]);
                    }
                } else if (strcmp(m, "add") == 0 && expr->method_call.arg_count == 1) {
                    if (is_int_set) {
                        ind(emit); fprintf(emit->out, "mix_set_add_int(t%d, (int64_t)t%d);\n", obj_temp, arg_temps2[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "mix_set_add(t%d, (const char *)t%d);\n", obj_temp, arg_temps2[0]);
                    }
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "remove") == 0 && expr->method_call.arg_count == 1) {
                    if (is_int_set) {
                        ind(emit); fprintf(emit->out, "mix_set_remove_int(t%d, (int64_t)t%d);\n", obj_temp, arg_temps2[0]);
                    } else {
                        ind(emit); fprintf(emit->out, "mix_set_remove(t%d, (const char *)t%d);\n", obj_temp, arg_temps2[0]);
                    }
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                } else if (strcmp(m, "union") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_union(t%d, t%d);\n", t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "intersect") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_intersect(t%d, t%d);\n", t, obj_temp, arg_temps2[0]);
                } else if (strcmp(m, "diff") == 0 && expr->method_call.arg_count == 1) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_diff(t%d, t%d);\n", t, obj_temp, arg_temps2[0]);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }

            /* Project.build() */
            if (obj_type && obj_type->kind == TYPE_SHAPE &&
                strcmp(obj_type->shape.name, "Project") == 0 &&
                strcmp(expr->method_call.method_name, "build") == 0) {
                int t = next_temp(emit);
                ind(emit); fprintf(emit->out, "mix_project_build(t%d);\n", obj_temp);
                ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                return t;
            }

            /* User-defined shape methods */
            {
                const char *shape_name = (obj_type && obj_type->kind == TYPE_SHAPE)
                    ? obj_type->shape.name : "Unknown";
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s", shape_name, expr->method_call.method_name);

                int t = next_temp(emit);
                const char *ret_ty = c_type(expr->resolved_type);
                if (expr->resolved_type && expr->resolved_type->kind == TYPE_SHAPE)
                    ret_ty = "void *";
                bool has_return = !(expr->resolved_type && expr->resolved_type->kind == TYPE_VOID);

                Symbol *msym = symtab_lookup(emit->symtab, mangled);
                MixType *mtype = (msym && msym->type && msym->type->kind == TYPE_FUNC)
                    ? msym->type : NULL;

                ind(emit);
                if (has_return) fprintf(emit->out, "%s t%d = ", ret_ty, t);
                fprintf(emit->out, "%s(", mangled);

                /* First arg: self */
                if (obj_type && obj_type->kind == TYPE_SHAPE)
                    fprintf(emit->out, "(%s *)t%d", shape_name, obj_temp);
                else
                    fprintf(emit->out, "t%d", obj_temp);

                /* Remaining args */
                for (int i = 0; i < expr->method_call.arg_count; i++) {
                    fprintf(emit->out, ", ");
                    MixType *pt = (mtype && i + 1 < mtype->func.param_count)
                        ? mtype->func.param_types[i + 1]
                        : expr->method_call.args[i]->resolved_type;
                    if (pt && pt->kind == TYPE_SHAPE)
                        fprintf(emit->out, "(%s *)t%d", pt->shape.name, arg_temps2[i]);
                    else if (pt && type_is_float(pt))
                        fprintf(emit->out, "(double)t%d", arg_temps2[i]);
                    else
                        fprintf(emit->out, "t%d", arg_temps2[i]);
                }
                fprintf(emit->out, ");\n");
                if (!has_return) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }
        }
        case NODE_SHAPE_LIT: {
            MixType *stype = expr->resolved_type;
            int t = next_temp(emit);

            if (stype && stype->kind == TYPE_SHAPE && stype->shape.is_tagged_union) {
                /* Tagged union construction */
                ShapeVariant *sv = type_find_variant(stype, expr->shape_lit.shape_name);
                int size = stype->shape.total_size > 0 ? stype->shape.total_size : 16;
                ind(emit); fprintf(emit->out, "uint8_t _tu%d[%d];\n", t, size);
                ind(emit); fprintf(emit->out, "memset(_tu%d, 0, %d);\n", t, size);
                if (sv) {
                    ind(emit); fprintf(emit->out, "*(int64_t *)_tu%d = %d;\n", t, sv->tag);
                    for (int i = 0; i < expr->shape_lit.field_count && i < sv->field_count; i++) {
                        int val = emit_expr(emit, expr->shape_lit.field_values[i]);
                        int foff = 8 + sv->fields[i].offset;
                        const char *fty = c_type(sv->fields[i].type);
                        ind(emit); fprintf(emit->out, "*(%s *)(_tu%d + %d) = (%s)t%d;\n",
                                fty, t, foff, fty, val);
                    }
                }
                ind(emit); fprintf(emit->out, "void *t%d = _tu%d;\n", t, t);
            } else if (stype && stype->kind == TYPE_SHAPE) {
                /* Regular struct construction — heap-allocate so pointer survives returns */
                ind(emit); fprintf(emit->out, "%s *t%d = (%s *)mix_alloc(sizeof(%s));\n",
                        stype->shape.name, t, stype->shape.name, stype->shape.name);
                ind(emit); fprintf(emit->out, "memset(t%d, 0, sizeof(%s));\n", t, stype->shape.name);
                for (int i = 0; i < expr->shape_lit.field_count; i++) {
                    ShapeFieldInfo *fi = type_find_field(stype, expr->shape_lit.field_names[i]);
                    if (!fi) continue;
                    int val = emit_expr(emit, expr->shape_lit.field_values[i]);
                    // Sub-shape stored inline — memcpy from the value pointer.
                    if (fi->type && fi->type->kind == TYPE_SHAPE) {
                        ind(emit); fprintf(emit->out, "memcpy(&t%d->%s, t%d, sizeof(%s));\n",
                                t, fi->name, val, fi->type->shape.name);
                    } else {
                        ind(emit); fprintf(emit->out, "t%d->%s = (%s)t%d;\n",
                                t, fi->name, c_type(fi->type), val);
                    }
                }
            } else {
                ind(emit); fprintf(emit->out, "void *t%d = 0;\n", t);
            }
            return t;
        }
        case NODE_FIELD_EXPR: {
            int obj = emit_expr(emit, expr->field_expr.object);
            MixType *obj_type = expr->field_expr.object->resolved_type;

            /* List .len */
            if (obj_type && obj_type->kind == TYPE_LIST) {
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    int t = next_temp(emit);
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_len(t%d);\n", t, obj);
                    return t;
                }
            }
            /* String .len */
            if (obj_type && obj_type->kind == TYPE_STR) {
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    int t = next_temp(emit);
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_str_len((const char *)t%d);\n", t, obj);
                    return t;
                }
            }
            /* Map fields */
            if (obj_type && obj_type->kind == TYPE_MAP) {
                const char *fn = expr->field_expr.field_name;
                int t = next_temp(emit);
                if (strcmp(fn, "len") == 0) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_map_len(t%d);\n", t, obj);
                } else if (strcmp(fn, "keys") == 0) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_map_keys(t%d);\n", t, obj);
                } else if (strcmp(fn, "values") == 0) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_map_values(t%d);\n", t, obj);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }
            /* Set fields */
            if (obj_type && obj_type->kind == TYPE_SET) {
                const char *fn = expr->field_expr.field_name;
                int t = next_temp(emit);
                MixType *selem = obj_type->set.elem_type;
                bool is_int_set = selem && type_is_integer(selem);
                if (strcmp(fn, "len") == 0) {
                    ind(emit); fprintf(emit->out, "int64_t t%d = mix_set_len(t%d);\n", t, obj);
                } else if (strcmp(fn, "values") == 0) {
                    if (is_int_set) {
                        ind(emit); fprintf(emit->out, "void *t%d = mix_set_values_int(t%d);\n", t, obj);
                    } else {
                        ind(emit); fprintf(emit->out, "void *t%d = mix_set_values(t%d);\n", t, obj);
                    }
                } else {
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
                }
                return t;
            }
            /* Shape field */
            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi = type_find_field(obj_type, expr->field_expr.field_name);
                if (fi) {
                    int t = next_temp(emit);
                    if (fi->type && fi->type->kind == TYPE_SHAPE) {
                        // Shape-typed field: return address of inline data
                        ind(emit); fprintf(emit->out, "void *t%d = &((%s *)t%d)->%s;\n",
                                t, obj_type->shape.name, obj, fi->name);
                    } else {
                        const char *fty = c_type(fi->type);
                        ind(emit); fprintf(emit->out, "%s t%d = ((%s *)t%d)->%s;\n",
                                fty, t, obj_type->shape.name, obj, fi->name);
                    }
                    return t;
                }
                /* Computed field (zero-param method) */
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                         obj_type->shape.name, expr->field_expr.field_name);
                Symbol *msym = symtab_lookup(emit->symtab, mangled);
                if (msym) {
                    int t = next_temp(emit);
                    const char *rty = c_type(expr->resolved_type);
                    ind(emit); fprintf(emit->out, "%s t%d = %s((%s *)t%d);\n",
                            rty, t, mangled, obj_type->shape.name, obj);
                    return t;
                }
            }
            /* Fallback */
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", t);
            return t;
        }
        case NODE_SHARED_EXPR: {
            int init_val = emit_expr(emit, expr->shared_expr.init_expr);
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_shared_new((int64_t)t%d);\n", t, init_val);
            return t;
        }
        case NODE_GO_EXPR: {
            AstNode *call = expr->go_expr.call_expr;
            if (call->kind != NODE_CALL_EXPR) {
                int t = next_temp(emit);
                ind(emit); fprintf(emit->out, "void *t%d = 0;\n", t);
                return t;
            }
            int arg_count = call->call.arg_count;
            int args_array;
            if (arg_count > 0) {
                args_array = next_temp(emit);
                ind(emit); fprintf(emit->out, "void *t%d = mix_alloc(%d);\n", args_array, arg_count * 8);
                for (int i = 0; i < arg_count; i++) {
                    int av = emit_expr(emit, call->call.args[i]);
                    ind(emit); fprintf(emit->out, "((int64_t *)t%d)[%d] = (int64_t)t%d;\n", args_array, i, av);
                }
            } else {
                args_array = next_temp(emit);
                ind(emit); fprintf(emit->out, "void *t%d = 0;\n", args_array);
            }
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "void *t%d = mix_task_spawn((void *)%s, t%d, %d);\n",
                    t, call->call.name, args_array, arg_count);
            return t;
        }
        case NODE_WAIT_EXPR: {
            int handle = emit_expr(emit, expr->wait_expr.handle_expr);
            int t = next_temp(emit);
            ind(emit); fprintf(emit->out, "int64_t t%d = mix_task_wait(t%d);\n", t, handle);
            return t;
        }
        default:
            mix_error(expr->loc, "unsupported expression node (kind %d) in C codegen", expr->kind);
            return next_temp(emit);
    }
}

/* --- Statement emission --- */
static void emit_stmt(CEmitter *emit, AstNode *stmt);

static void emit_block(CEmitter *emit, AstNode *block) {
    if (!block || block->kind != NODE_BLOCK) return;
    for (int i = 0; i < block->block.stmt_count; i++)
        emit_stmt(emit, block->block.stmts[i]);
}

static void emit_stmt(CEmitter *emit, AstNode *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case NODE_VAR_DECL: {
            if (stmt->var_decl.init_expr && stmt->var_decl.init_expr->kind == NODE_LAMBDA) {
                if (emit->fn_ptr_var_count < 128)
                    emit->fn_ptr_vars[emit->fn_ptr_var_count++] =
                        arena_strdup(emit->arena, stmt->var_decl.name);
            }
            MixType *vtype = stmt->resolved_type;
            /* If var type is void/null, try init expression type, then default to int64_t */
            if ((!vtype || vtype->kind == TYPE_VOID) && stmt->var_decl.init_expr)
                vtype = stmt->var_decl.init_expr->resolved_type;
            /* Final guard: never declare void variables */
            bool vtype_is_void = (!vtype || vtype->kind == TYPE_VOID);

            if (vtype && vtype->kind == TYPE_SHAPE) {
                int init = emit_expr(emit, stmt->var_decl.init_expr);
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "%s *v_%s = (%s *)t%d;\n",
                        vtype->shape.name, cn, vtype->shape.name, init);
            } else if (stmt->var_decl.is_mutable) {
                int init = emit_expr(emit, stmt->var_decl.init_expr);
                const char *ty = vtype_is_void ? "int64_t" : c_type(vtype);
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "%s v_%s = (%s)t%d;\n", ty, cn, ty, init);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_INT_LIT) {
                const char *ty = c_type(vtype);
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "%s v_%s = %" PRId64 ";\n",
                        ty, cn, stmt->var_decl.init_expr->int_lit.value);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_FLOAT_LIT) {
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "double v_%s = %.17g;\n",
                        cn, stmt->var_decl.init_expr->float_lit.value);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_BOOL_LIT) {
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "int32_t v_%s = %d;\n",
                        cn, stmt->var_decl.init_expr->bool_lit.value ? 1 : 0);
            } else if (stmt->var_decl.init_expr &&
                       stmt->var_decl.init_expr->kind == NODE_STRING_LIT) {
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "const char *v_%s = \"", cn);
                c_escape_string(emit->out, stmt->var_decl.init_expr->string_lit.value,
                                stmt->var_decl.init_expr->string_lit.length);
                fprintf(emit->out, "\";\n");
            } else {
                int init = emit_expr(emit, stmt->var_decl.init_expr);
                const char *ty = vtype_is_void ? "int64_t" : c_type(vtype);
                const char *cn = cname_decl(emit, stmt->var_decl.name);
                ind(emit); fprintf(emit->out, "%s v_%s = (%s)t%d;\n", ty, cn, ty, init);
            }
            break;
        }
        case NODE_ASSIGN: {
            int val = emit_expr(emit, stmt->assign.value);
            const char *cn = cname_ref(emit, stmt->assign.name);
            if (stmt->assign.op == TOK_EQ) {
                ind(emit); fprintf(emit->out, "v_%s = t%d;\n", cn, val);
            } else {
                const char *op;
                switch (stmt->assign.op) {
                    case TOK_PLUS_EQ:  op = "+="; break;
                    case TOK_MINUS_EQ: op = "-="; break;
                    case TOK_STAR_EQ:  op = "*="; break;
                    case TOK_SLASH_EQ: op = "/="; break;
                    default: op = "+="; break;
                }
                ind(emit); fprintf(emit->out, "v_%s %s t%d;\n", cn, op, val);
            }
            break;
        }
        case NODE_IF_STMT: {
            int cond = emit_expr(emit, stmt->if_stmt.condition);
            ind(emit); fprintf(emit->out, "if (t%d) {\n", cond);
            emit->indent++;
            emit_block(emit, stmt->if_stmt.then_block);
            emit->indent--;
            if (stmt->if_stmt.else_block) {
                ind(emit); fprintf(emit->out, "} else {\n");
                emit->indent++;
                if (stmt->if_stmt.else_block->kind == NODE_BLOCK) {
                    emit_block(emit, stmt->if_stmt.else_block);
                } else {
                    emit_stmt(emit, stmt->if_stmt.else_block);
                }
                emit->indent--;
                ind(emit); fprintf(emit->out, "}\n");
            } else {
                ind(emit); fprintf(emit->out, "}\n");
            }
            break;
        }
        case NODE_WHILE_STMT: {
            ind(emit); fprintf(emit->out, "while (1) {\n");
            emit->indent++;
            int cond = emit_expr(emit, stmt->while_stmt.condition);
            ind(emit); fprintf(emit->out, "if (!t%d) break;\n", cond);
            emit_block(emit, stmt->while_stmt.body);
            emit->indent--;
            ind(emit); fprintf(emit->out, "}\n");
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
                int map_ptr = emit_expr(emit, iter);
                int keys_list = next_temp(emit);
                ind(emit); fprintf(emit->out, "void *t%d = mix_map_keys(t%d);\n", keys_list, map_ptr);
                int len_t = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_len(t%d);\n", len_t, keys_list);
                int idx_id = next_label(emit);
                ind(emit); fprintf(emit->out, "for (int64_t _midx_%d = 0; _midx_%d < t%d; _midx_%d++) {\n",
                        idx_id, idx_id, len_t, idx_id);
                emit->indent++;
                ind(emit); fprintf(emit->out, "int64_t _key_%d = mix_list_get(t%d, _midx_%d);\n",
                        idx_id, keys_list, idx_id);
                if (stmt->for_stmt.index_name) {
                    ind(emit); fprintf(emit->out, "int64_t v_%s = _key_%d;\n",
                            cname_decl(emit, stmt->for_stmt.index_name), idx_id);
                }
                ind(emit); fprintf(emit->out, "int64_t v_%s = mix_map_get(t%d, (const char *)_key_%d);\n",
                        cname_decl(emit, stmt->for_stmt.var_name), map_ptr, idx_id);
                emit_block(emit, stmt->for_stmt.body);
                emit->indent--;
                ind(emit); fprintf(emit->out, "}\n");
            } else if (is_list) {
                int list_ptr = emit_expr(emit, iter);
                int len_t = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_len(t%d);\n", len_t, list_ptr);
                int idx_id = next_label(emit);
                ind(emit); fprintf(emit->out, "for (int64_t _idx_%d = 0; _idx_%d < t%d; _idx_%d++) {\n",
                        idx_id, idx_id, len_t, idx_id);
                emit->indent++;
                MixType *elem = iter_type->list.elem_type;
                const char *vn = cname_decl(emit, stmt->for_stmt.var_name);
                if (elem && type_is_float(elem)) {
                    ind(emit); fprintf(emit->out, "double v_%s = mix_bits_to_double(mix_list_get(t%d, _idx_%d));\n",
                            vn, list_ptr, idx_id);
                } else {
                    ind(emit); fprintf(emit->out, "int64_t v_%s = mix_list_get(t%d, _idx_%d);\n",
                            vn, list_ptr, idx_id);
                }
                if (stmt->for_stmt.index_name) {
                    ind(emit); fprintf(emit->out, "int64_t v_%s = _idx_%d;\n",
                            cname_decl(emit, stmt->for_stmt.index_name), idx_id);
                }
                emit_block(emit, stmt->for_stmt.body);
                emit->indent--;
                ind(emit); fprintf(emit->out, "}\n");
            } else if (is_set) {
                int set_ptr = emit_expr(emit, iter);
                int list_ptr = next_temp(emit);
                MixType *selem = iter_type->set.elem_type;
                bool is_int_set = selem && type_is_integer(selem);
                if (is_int_set) {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_values_int(t%d);\n", list_ptr, set_ptr);
                } else {
                    ind(emit); fprintf(emit->out, "void *t%d = mix_set_values(t%d);\n", list_ptr, set_ptr);
                }
                int len_t = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_list_len(t%d);\n", len_t, list_ptr);
                int idx_id = next_label(emit);
                ind(emit); fprintf(emit->out, "for (int64_t _sidx_%d = 0; _sidx_%d < t%d; _sidx_%d++) {\n",
                        idx_id, idx_id, len_t, idx_id);
                emit->indent++;
                ind(emit); fprintf(emit->out, "int64_t v_%s = mix_list_get(t%d, _sidx_%d);\n",
                        cname_decl(emit, stmt->for_stmt.var_name), list_ptr, idx_id);
                emit_block(emit, stmt->for_stmt.body);
                emit->indent--;
                ind(emit); fprintf(emit->out, "}\n");
            } else {
                /* For-range */
                int start_val, end_val;
                bool inclusive = false;
                if (is_range) {
                    start_val = emit_expr(emit, iter->binary.left);
                    end_val = emit_expr(emit, iter->binary.right);
                    inclusive = (iter->binary.op == TOK_DOTDOT_EQ);
                } else {
                    start_val = emit_expr(emit, iter);
                    end_val = next_temp(emit);
                    ind(emit); fprintf(emit->out, "int64_t t%d = 0;\n", end_val);
                }
                const char *cmp_op = inclusive ? "<=" : "<";
                const char *vn = cname_decl(emit, stmt->for_stmt.var_name);
                ind(emit); fprintf(emit->out, "for (int64_t v_%s = t%d; v_%s %s t%d; v_%s++) {\n",
                        vn, start_val, vn, cmp_op, end_val, vn);
                emit->indent++;
                emit_block(emit, stmt->for_stmt.body);
                emit->indent--;
                ind(emit); fprintf(emit->out, "}\n");
            }
            break;
        }
        case NODE_MATCH_STMT: {
            int subject = emit_expr(emit, stmt->match_stmt.subject);
            MixType *subj_type = stmt->match_stmt.subject->resolved_type;
            bool is_tagged = subj_type && subj_type->kind == TYPE_SHAPE && subj_type->shape.is_tagged_union;
            bool is_optional = subj_type && subj_type->kind == TYPE_OPTIONAL;
            bool is_result = subj_type && subj_type->kind == TYPE_RESULT;

            MixType *match_type = stmt->resolved_type;
            bool has_result = match_type && match_type->kind != TYPE_VOID;
            int match_result = -1;
            if (has_result) {
                match_result = next_temp(emit);
                const char *rty = c_type(match_type);
                ind(emit); fprintf(emit->out, "%s t%d = 0;\n", rty, match_result);
            }
            emit->last_match_temp = match_result;

            int tag_val = -1;
            if (is_tagged) {
                tag_val = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = *(int64_t *)t%d;\n", tag_val, subject);
            }
            int has_val = -1;
            if (is_optional) {
                has_val = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_optional_has(t%d);\n",
                        has_val, subject);
            } else if (is_result) {
                has_val = next_temp(emit);
                ind(emit); fprintf(emit->out, "int64_t t%d = mix_result_is_ok(t%d);\n",
                        has_val, subject);
            }

            /* Pre-evaluate all pattern expressions so temps are in scope */
            int pat_temps[64];
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                struct MatchArm *arm = &stmt->match_stmt.arms[i];
                bool is_capture_pat = arm->pattern && (
                    (is_optional && (arm->pattern->kind == NODE_NONE_LIT ||
                                     (arm->pattern->kind == NODE_CALL_EXPR &&
                                      strcmp(arm->pattern->call.name, "some") == 0))) ||
                    (is_result && arm->pattern->kind == NODE_CALL_EXPR &&
                     (strcmp(arm->pattern->call.name, "ok") == 0 ||
                      strcmp(arm->pattern->call.name, "err") == 0)));
                if (!arm->is_wildcard && !is_capture_pat &&
                    !(is_tagged && arm->pattern && arm->pattern->kind == NODE_CALL_EXPR)) {
                    pat_temps[i] = emit_expr(emit, arm->pattern);
                } else {
                    pat_temps[i] = -1;
                }
            }

            bool first_arm = true;
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                struct MatchArm *arm = &stmt->match_stmt.arms[i];

                if (arm->is_wildcard) {
                    if (!first_arm) {
                        ind(emit); fprintf(emit->out, "} else {\n");
                    } else {
                        ind(emit); fprintf(emit->out, "{\n");
                    }
                    emit->indent++;
                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK)
                            emit_block(emit, arm->body);
                        else {
                            int val = emit_expr(emit, arm->body);
                            if (has_result) {
                                ind(emit); fprintf(emit->out, "t%d = t%d;\n", match_result, val);
                            }
                        }
                    }
                    emit->indent--;
                    ind(emit); fprintf(emit->out, "}\n");
                    first_arm = false;
                } else if ((is_optional || is_result) && arm->pattern) {
                    AstNode *pat = arm->pattern;
                    bool match_truthy;
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
                            continue;
                        }
                        if (pat->call.arg_count > 0 &&
                            pat->call.args[0]->kind == NODE_IDENT)
                            bind_name = pat->call.args[0]->ident.name;
                    } else {
                        continue;
                    }

                    if (!first_arm) {
                        ind(emit); fprintf(emit->out, "} else ");
                    } else {
                        ind(emit);
                    }
                    fprintf(emit->out, "if (t%d %s 0) {\n",
                            has_val, match_truthy ? "!=" : "==");
                    emit->indent++;
                    if (bind_name && accessor) {
                        const char *cn = cname_decl(emit, bind_name);
                        ind(emit); fprintf(emit->out, "int64_t v_%s = %s(t%d);\n",
                                cn, accessor, subject);
                    }
                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK)
                            emit_block(emit, arm->body);
                        else {
                            int val = emit_expr(emit, arm->body);
                            if (has_result) {
                                ind(emit); fprintf(emit->out, "t%d = t%d;\n", match_result, val);
                            }
                        }
                    }
                    emit->indent--;
                    first_arm = false;
                } else if (is_tagged && arm->pattern && arm->pattern->kind == NODE_CALL_EXPR) {
                    const char *var_name = arm->pattern->call.name;
                    ShapeVariant *sv = type_find_variant(subj_type, var_name);

                    if (!first_arm) {
                        ind(emit); fprintf(emit->out, "} else ");
                    }
                    if (sv) {
                        ind(emit); fprintf(emit->out, "if (t%d == %d) {\n", tag_val, sv->tag);
                        emit->indent++;
                        for (int k = 0; k < arm->pattern->call.arg_count && k < sv->field_count; k++) {
                            AstNode *binding = arm->pattern->call.args[k];
                            if (binding->kind == NODE_IDENT) {
                                int foff = 8 + sv->fields[k].offset;
                                const char *fty = c_type(sv->fields[k].type);
                                ind(emit); fprintf(emit->out, "%s v_%s = *(%s *)((char *)t%d + %d);\n",
                                        fty, cname_decl(emit, binding->ident.name),
                                        fty, subject, foff);
                            }
                        }
                        if (arm->body) {
                            if (arm->body->kind == NODE_BLOCK)
                                emit_block(emit, arm->body);
                            else {
                                int val = emit_expr(emit, arm->body);
                                if (has_result) {
                                    ind(emit); fprintf(emit->out, "t%d = t%d;\n", match_result, val);
                                }
                            }
                        }
                        emit->indent--;
                    }
                    first_arm = false;
                } else {
                    int pat = pat_temps[i];
                    if (!first_arm) {
                        ind(emit); fprintf(emit->out, "} else ");
                    }
                    ind(emit); fprintf(emit->out, "if (t%d == t%d) {\n", subject, pat);
                    emit->indent++;
                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK)
                            emit_block(emit, arm->body);
                        else {
                            int val = emit_expr(emit, arm->body);
                            if (has_result) {
                                ind(emit); fprintf(emit->out, "t%d = t%d;\n", match_result, val);
                            }
                        }
                    }
                    emit->indent--;
                    first_arm = false;
                }
            }
            if (!first_arm && !stmt->match_stmt.arms[stmt->match_stmt.arm_count - 1].is_wildcard) {
                ind(emit); fprintf(emit->out, "}\n");
            }
            break;
        }
        case NODE_DONE_STMT: {
            for (int i = emit->defer_count - 1; i >= 0; i--)
                emit_stmt(emit, emit->deferred[i]);
            if (stmt->done_stmt.value) {
                int val = emit_expr(emit, stmt->done_stmt.value);
                if (emit->current_return_type &&
                    emit->current_return_type->kind == TYPE_RESULT &&
                    stmt->done_stmt.value->kind != NODE_NONE_LIT) {
                    MixType *ok_type = emit->current_return_type->result.ok_type;
                    if (ok_type && type_is_float(ok_type)) {
                        int bits = next_temp(emit);
                        ind(emit); fprintf(emit->out, "int64_t t%d; memcpy(&t%d, &t%d, sizeof(double));\n",
                                bits, bits, val);
                        ind(emit); fprintf(emit->out, "return mix_result_ok(t%d);\n", bits);
                    } else {
                        ind(emit); fprintf(emit->out, "return mix_result_ok((int64_t)t%d);\n", val);
                    }
                } else if (emit->current_return_type &&
                           emit->current_return_type->kind == TYPE_OPTIONAL &&
                           stmt->done_stmt.value->kind != NODE_NONE_LIT) {
                    ind(emit); fprintf(emit->out, "return mix_optional_some((int64_t)t%d);\n", val);
                } else {
                    ind(emit); fprintf(emit->out, "return t%d;\n", val);
                }
            } else {
                ind(emit); fprintf(emit->out, "return;\n");
            }
            break;
        }
        case NODE_DEFER_STMT: {
            if (emit->defer_count < 64)
                emit->deferred[emit->defer_count++] = stmt->defer_stmt.stmt;
            break;
        }
        case NODE_EXPR_STMT:
            emit_expr(emit, stmt->expr_stmt.expr);
            break;
        case NODE_UNSAFE_BLOCK:
            emit_block(emit, stmt->unsafe_block.body);
            break;
        case NODE_ZONE_STMT: {
            ind(emit); fprintf(emit->out, "mix_zone_enter();\n");
            emit_block(emit, stmt->zone_stmt.body);
            ind(emit); fprintf(emit->out, "mix_zone_exit();\n");
            break;
        }
        case NODE_DEREF_ASSIGN: {
            int ptr = emit_expr(emit, stmt->deref_assign.ptr_expr);
            int val = emit_expr(emit, stmt->deref_assign.value);
            MixType *val_type = stmt->deref_assign.value->resolved_type;
            const char *ty = c_type(val_type);
            ind(emit); fprintf(emit->out, "*(%s *)t%d = (%s)t%d;\n", ty, ptr, ty, val);
            break;
        }
        case NODE_INDEX_ASSIGN: {
            int obj = emit_expr(emit, stmt->index_assign.object);
            int idx = emit_expr(emit, stmt->index_assign.index);
            int val = emit_expr(emit, stmt->index_assign.value);
            MixType *obj_type = stmt->index_assign.object->resolved_type;
            if (obj_type && obj_type->kind == TYPE_MAP) {
                ind(emit); fprintf(emit->out, "mix_map_set(t%d, (const char *)t%d, (int64_t)t%d);\n",
                        obj, idx, val);
            } else {
                MixType *elem = (obj_type && obj_type->kind == TYPE_LIST) ? obj_type->list.elem_type : NULL;
                if (elem && type_is_float(elem))
                    { ind(emit); fprintf(emit->out, "mix_list_set(t%d, (int64_t)t%d, mix_double_to_bits(t%d));\n", obj, idx, val); }
                else
                    { ind(emit); fprintf(emit->out, "mix_list_set(t%d, (int64_t)t%d, (int64_t)t%d);\n", obj, idx, val); }
            }
            break;
        }
        case NODE_FIELD_ASSIGN: {
            int obj = emit_expr(emit, stmt->field_assign.object);
            int val = emit_expr(emit, stmt->field_assign.value);
            MixType *obj_type = stmt->field_assign.object->resolved_type;
            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi_dst =
                    type_find_field(obj_type, stmt->field_assign.field_name);
                // Whole-shape replacement: `s.pos = Vec2(...)`. The field is
                // stored inline; the value is a pointer to the source shape.
                // Scalar assignment would only copy 8 bytes — memcpy the rest.
                if (fi_dst && fi_dst->type && fi_dst->type->kind == TYPE_SHAPE &&
                    stmt->field_assign.op == TOK_EQ) {
                    ind(emit); fprintf(emit->out,
                            "memcpy(&((%s *)t%d)->%s, t%d, sizeof(%s));\n",
                            obj_type->shape.name, obj, stmt->field_assign.field_name,
                            val, fi_dst->type->shape.name);
                    break;
                }
                if (stmt->field_assign.op == TOK_EQ) {
                    ind(emit); fprintf(emit->out, "((%s *)t%d)->%s = t%d;\n",
                            obj_type->shape.name, obj, stmt->field_assign.field_name, val);
                } else {
                    const char *op;
                    switch (stmt->field_assign.op) {
                        case TOK_PLUS_EQ:  op = "+="; break;
                        case TOK_MINUS_EQ: op = "-="; break;
                        case TOK_STAR_EQ:  op = "*="; break;
                        case TOK_SLASH_EQ: op = "/="; break;
                        default: op = "+="; break;
                    }
                    ind(emit); fprintf(emit->out, "((%s *)t%d)->%s %s t%d;\n",
                            obj_type->shape.name, obj, stmt->field_assign.field_name, op, val);
                }
            }
            break;
        }
        case NODE_FAIL_STMT: {
            int fval = emit_expr(emit, stmt->fail_stmt.value);
            if (emit->current_return_type &&
                emit->current_return_type->kind == TYPE_RESULT) {
                for (int i = emit->defer_count - 1; i >= 0; i--)
                    emit_stmt(emit, emit->deferred[i]);
                ind(emit); fprintf(emit->out, "return mix_result_err((int64_t)t%d);\n", fval);
            } else {
                MixType *vtype = stmt->fail_stmt.value->resolved_type;
                if (vtype && vtype->kind == TYPE_STR) {
                    ind(emit); fprintf(emit->out, "mix_panic((const char *)t%d);\n", fval);
                } else {
                    ind(emit); fprintf(emit->out, "mix_panic(\"fail\");\n");
                }
            }
            break;
        }
        case NODE_BREAK_STMT:
            ind(emit); fprintf(emit->out, "break;\n");
            break;
        case NODE_CONTINUE_STMT:
            ind(emit); fprintf(emit->out, "continue;\n");
            break;
        default:
            break;
    }
}

/* --- Function emission --- */

static void emit_fn_decl(CEmitter *emit, AstNode *fn) {
    bool is_main = strcmp(fn->fn_decl.name, "main") == 0;

    if (is_main) {
        fprintf(emit->out, "int main(int argc, char **argv) {\n");
        emit->indent = 1;
        ind(emit); fprintf(emit->out, "mix_set_args((int32_t)argc, argv);\n");
    } else {
        MixType *ret_type = fn->fn_decl.return_type
            ? fn->fn_decl.return_type->resolved_type : NULL;
        Symbol *fn_sig_sym = symtab_lookup(emit->symtab, fn->fn_decl.name);
        if (fn_sig_sym && fn_sig_sym->type && fn_sig_sym->type->kind == TYPE_FUNC &&
            fn_sig_sym->type->func.return_type &&
            fn_sig_sym->type->func.return_type->kind == TYPE_RESULT) {
            ret_type = fn_sig_sym->type->func.return_type;
        }

        const char *ret_str = ret_type ? c_type(ret_type) : "void";
        /* Result/optional return as void* */
        if (ret_type && (ret_type->kind == TYPE_RESULT || ret_type->kind == TYPE_OPTIONAL))
            ret_str = "void *";
        else if (ret_type && ret_type->kind == TYPE_SHAPE)
            ret_str = "void *";

        const char *static_kw = fn->fn_decl.is_pub ? "" : "static ";
        fprintf(emit->out, "%s%s %s(", static_kw, ret_str, fn->fn_decl.name);

        for (int i = 0; i < fn->fn_decl.param_count; i++) {
            if (i > 0) fprintf(emit->out, ", ");
            Param *param = &fn->fn_decl.params[i];
            MixType *ptype = param->type ? param->type->resolved_type : NULL;
            if (ptype && ptype->kind == TYPE_SHAPE)
                fprintf(emit->out, "%s *p_%s", ptype->shape.name, param->name);
            else if (ptype && type_is_float(ptype))
                fprintf(emit->out, "double p_%s", param->name);
            else {
                const char *pty = c_type(ptype);
                fprintf(emit->out, "%s p_%s", pty, param->name);
            }
        }
        if (fn->fn_decl.param_count == 0) fprintf(emit->out, "void");
        fprintf(emit->out, ") {\n");
        emit->indent = 1;
    }

    /* Copy parameters to v_ variables */
    for (int i = 0; i < fn->fn_decl.param_count; i++) {
        Param *param = &fn->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        if (ptype && ptype->kind == TYPE_SHAPE) {
            ind(emit); fprintf(emit->out, "%s *v_%s = p_%s;\n",
                    ptype->shape.name, param->name, param->name);
        } else {
            const char *ty = c_type(ptype);
            if (type_is_float(ptype))
                ty = "double";
            ind(emit); fprintf(emit->out, "%s v_%s = p_%s;\n", ty, param->name, param->name);
        }
    }

    /* Reset per-function state */
    emit->defer_count = 0;
    emit->fn_ptr_var_count = 0;
    for (int ci = 0; ci < emit->const_count; ci++)
        emit->constants[ci].cached_temp = -1;

    /* Track return type */
    MixType *ret_type_resolved = fn->fn_decl.return_type
        ? fn->fn_decl.return_type->resolved_type : NULL;
    Symbol *fn_sym_lookup = symtab_lookup(emit->symtab, fn->fn_decl.name);
    if (fn_sym_lookup && fn_sym_lookup->type && fn_sym_lookup->type->kind == TYPE_FUNC)
        emit->current_return_type = fn_sym_lookup->type->func.return_type;
    else
        emit->current_return_type = ret_type_resolved;

    /* Register function-typed params as indirect call targets */
    for (int i = 0; i < fn->fn_decl.param_count; i++) {
        Param *param = &fn->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        if (ptype && ptype->kind == TYPE_FUNC && emit->fn_ptr_var_count < 128)
            emit->fn_ptr_vars[emit->fn_ptr_var_count++] =
                arena_strdup(emit->arena, param->name);
    }

    cname_reset(emit);

    /* Emit body with implicit return handling */
    if (fn->fn_decl.body) {
        AstNode *body = fn->fn_decl.body;
        int stmt_count = body->block.stmt_count;

        for (int i = 0; i < stmt_count - 1; i++)
            emit_stmt(emit, body->block.stmts[i]);

        if (stmt_count > 0) {
            AstNode *last = body->block.stmts[stmt_count - 1];
            if (!is_main && fn->fn_decl.return_type && last->kind == NODE_EXPR_STMT) {
                for (int i = emit->defer_count - 1; i >= 0; i--)
                    emit_stmt(emit, emit->deferred[i]);
                int val = emit_expr(emit, last->expr_stmt.expr);
                if (ret_type_resolved && ret_type_resolved->kind == TYPE_OPTIONAL &&
                    last->expr_stmt.expr->kind != NODE_NONE_LIT) {
                    ind(emit); fprintf(emit->out, "return mix_optional_some((int64_t)t%d);\n", val);
                } else if (emit->current_return_type &&
                           emit->current_return_type->kind == TYPE_RESULT &&
                           last->expr_stmt.expr->kind != NODE_NONE_LIT) {
                    MixType *ok_type = emit->current_return_type->result.ok_type;
                    if (ok_type && type_is_float(ok_type)) {
                        int bits = next_temp(emit);
                        ind(emit); fprintf(emit->out, "int64_t t%d; memcpy(&t%d, &t%d, sizeof(double));\n",
                                bits, bits, val);
                        ind(emit); fprintf(emit->out, "return mix_result_ok(t%d);\n", bits);
                    } else {
                        ind(emit); fprintf(emit->out, "return mix_result_ok((int64_t)t%d);\n", val);
                    }
                } else {
                    ind(emit); fprintf(emit->out, "return t%d;\n", val);
                }
                fprintf(emit->out, "}\n\n");
                return;
            } else if (!is_main && fn->fn_decl.return_type && last->kind == NODE_MATCH_STMT) {
                emit->last_match_temp = -1;
                emit_stmt(emit, last);
                if (emit->last_match_temp >= 0) {
                    for (int i = emit->defer_count - 1; i >= 0; i--)
                        emit_stmt(emit, emit->deferred[i]);
                    int val = emit->last_match_temp;
                    if (ret_type_resolved && ret_type_resolved->kind == TYPE_OPTIONAL) {
                        ind(emit); fprintf(emit->out, "return mix_optional_some((int64_t)t%d);\n", val);
                    } else if (emit->current_return_type && emit->current_return_type->kind == TYPE_RESULT) {
                        ind(emit); fprintf(emit->out, "return mix_result_ok((int64_t)t%d);\n", val);
                    } else {
                        ind(emit); fprintf(emit->out, "return t%d;\n", val);
                    }
                    fprintf(emit->out, "}\n\n");
                    return;
                }
            } else {
                emit_stmt(emit, last);
            }
        }
    }

    for (int i = emit->defer_count - 1; i >= 0; i--)
        emit_stmt(emit, emit->deferred[i]);

    if (is_main) {
        ind(emit); fprintf(emit->out, "return 0;\n");
    } else if (!fn->fn_decl.return_type) {
        /* void return */
    } else {
        ind(emit); fprintf(emit->out, "return 0;\n");
    }
    fprintf(emit->out, "}\n\n");
}

/* Emit shape method */
static void emit_method(CEmitter *emit, AstNode *method, const char *shape_name,
                         MixType *shape_type, bool is_pub) {
    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s_%s", shape_name, method->fn_decl.name);

    MixType *ret_type = method->fn_decl.return_type
        ? method->fn_decl.return_type->resolved_type : NULL;
    const char *ret_str = ret_type ? c_type(ret_type) : "void";
    if (ret_type && (ret_type->kind == TYPE_RESULT || ret_type->kind == TYPE_OPTIONAL))
        ret_str = "void *";
    else if (ret_type && ret_type->kind == TYPE_SHAPE)
        ret_str = "void *";

    const char *static_kw = is_pub ? "" : "static ";
    fprintf(emit->out, "%s%s %s(%s *p_self", static_kw, ret_str, mangled, shape_name);

    for (int i = 0; i < method->fn_decl.param_count; i++) {
        fprintf(emit->out, ", ");
        Param *param = &method->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        if (ptype && ptype->kind == TYPE_SHAPE)
            fprintf(emit->out, "%s *p_%s", ptype->shape.name, param->name);
        else if (ptype && type_is_float(ptype))
            fprintf(emit->out, "double p_%s", param->name);
        else {
            const char *pty = c_type(ptype);
            fprintf(emit->out, "%s p_%s", pty, param->name);
        }
    }
    fprintf(emit->out, ") {\n");
    emit->indent = 1;

    /* Alias self to v_self so NODE_IDENT lookups (which generate v_*) resolve. */
    ind(emit); fprintf(emit->out, "%s *v_self = p_self;\n", shape_name);

    /* Copy user params */
    for (int i = 0; i < method->fn_decl.param_count; i++) {
        Param *param = &method->fn_decl.params[i];
        MixType *ptype = param->type ? param->type->resolved_type : NULL;
        if (ptype && ptype->kind == TYPE_SHAPE) {
            ind(emit); fprintf(emit->out, "%s *v_%s = p_%s;\n",
                    ptype->shape.name, param->name, param->name);
        } else {
            const char *ty = type_is_float(ptype) ? "double" : c_type(ptype);
            ind(emit); fprintf(emit->out, "%s v_%s = p_%s;\n", ty, param->name, param->name);
        }
    }

    emit->current_shape = shape_type;
    emit->defer_count = 0;
    emit->fn_ptr_var_count = 0;
    for (int ci = 0; ci < emit->const_count; ci++)
        emit->constants[ci].cached_temp = -1;

    cname_reset(emit);

    /* Emit body with implicit return */
    if (method->fn_decl.body) {
        AstNode *body = method->fn_decl.body;
        int stmt_count = body->block.stmt_count;

        for (int i = 0; i < stmt_count - 1; i++)
            emit_stmt(emit, body->block.stmts[i]);

        if (stmt_count > 0) {
            AstNode *last = body->block.stmts[stmt_count - 1];
            if (ret_type && last->kind == NODE_EXPR_STMT) {
                for (int i = emit->defer_count - 1; i >= 0; i--)
                    emit_stmt(emit, emit->deferred[i]);
                int val = emit_expr(emit, last->expr_stmt.expr);
                ind(emit); fprintf(emit->out, "return t%d;\n", val);
                emit->current_shape = NULL;
                fprintf(emit->out, "}\n\n");
                return;
            } else {
                emit_stmt(emit, last);
            }
        }
    }

    for (int i = emit->defer_count - 1; i >= 0; i--)
        emit_stmt(emit, emit->deferred[i]);

    if (ret_type)
        { ind(emit); fprintf(emit->out, "return 0;\n"); }

    emit->current_shape = NULL;
    fprintf(emit->out, "}\n\n");
}

/* --- Program emission --- */

void c_emit_program(CEmitter *emit, AstNode *program) {
    if (!program || program->kind != NODE_PROGRAM) return;

    /* Header */
    fprintf(emit->out,
        "/* Generated by mix --backend c */\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#include <stdbool.h>\n\n"
    );

    emit_runtime_decls(emit);

    /* Register constants */
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_CONST_DECL && emit->const_count < 8192) {
            emit->constants[emit->const_count].name = decl->const_decl.name;
            emit->constants[emit->const_count].value = decl->const_decl.value;
            emit->constants[emit->const_count].cached_temp = -1;
            emit->const_count++;
        }
    }

    /* Emit shape struct typedefs */
    fprintf(emit->out, "/* Shapes */\n");
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_SHAPE_DECL) {
            // Skip generic shape templates — instantiations are emitted
            // as their own decls.
            if (decl->shape_decl.type_param_count > 0) continue;
            Symbol *shape_sym = symtab_lookup(emit->symtab, decl->shape_decl.name);
            MixType *st = (shape_sym && shape_sym->type) ? shape_sym->type : NULL;

            if (st && st->shape.is_tagged_union) {
                int data_bytes = st->shape.total_size - 8;
                if (data_bytes < 8) data_bytes = 8;
                fprintf(emit->out, "typedef struct { int64_t _tag; uint8_t _data[%d]; } %s;\n",
                        data_bytes, decl->shape_decl.name);
            } else if (st && st->shape.is_union) {
                fprintf(emit->out, "typedef union {\n");
                for (int j = 0; j < decl->shape_decl.field_count; j++) {
                    ShapeField *sf = &decl->shape_decl.fields[j];
                    MixType *ftype = sf->type ? sf->type->resolved_type : NULL;
                    if (ftype && ftype->kind == TYPE_SHAPE)
                        fprintf(emit->out, "    %s %s;\n", ftype->shape.name, sf->name);
                    else
                        fprintf(emit->out, "    %s %s;\n", c_type(ftype), sf->name);
                }
                fprintf(emit->out, "} %s;\n", decl->shape_decl.name);
            } else {
                fprintf(emit->out, "typedef struct {\n");
                for (int j = 0; j < decl->shape_decl.field_count; j++) {
                    ShapeField *sf = &decl->shape_decl.fields[j];
                    MixType *ftype = sf->type ? sf->type->resolved_type : NULL;
                    // Inline sub-shapes (no pointer): matches the QBE backend's
                    // memory layout and lets `s.pos.x = v` write into the parent.
                    if (ftype && ftype->kind == TYPE_SHAPE)
                        fprintf(emit->out, "    %s %s;\n", ftype->shape.name, sf->name);
                    else
                        fprintf(emit->out, "    %s %s;\n", c_type(ftype), sf->name);
                }
                fprintf(emit->out, "} %s;\n", decl->shape_decl.name);
            }
        }
    }
    /* Imported shapes from symbol table */
    {
        char *emitted_shapes[128];
        int emitted_count = 0;
        for (int i = 0; i < program->program.decl_count; i++) {
            if (program->program.decls[i]->kind == NODE_SHAPE_DECL && emitted_count < 128)
                emitted_shapes[emitted_count++] = program->program.decls[i]->shape_decl.name;
        }
        for (Scope *scope = emit->symtab->current; scope; scope = scope->parent) {
            for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
                if (sym->type && sym->type->kind == TYPE_SHAPE) {
                    bool already = false;
                    for (int j = 0; j < emitted_count; j++) {
                        if (strcmp(emitted_shapes[j], sym->type->shape.name) == 0) {
                            already = true; break;
                        }
                    }
                    if (!already && emitted_count < 128) {
                        const char *kw = sym->type->shape.is_union ? "union" : "struct";
                        fprintf(emit->out, "typedef %s {\n", kw);
                        for (int j = 0; j < sym->type->shape.field_count; j++) {
                            MixType *fty = sym->type->shape.fields[j].type;
                            // Inline sub-shapes (matches QBE layout).
                            if (fty && fty->kind == TYPE_SHAPE)
                                fprintf(emit->out, "    %s %s;\n",
                                        fty->shape.name,
                                        sym->type->shape.fields[j].name);
                            else
                                fprintf(emit->out, "    %s %s;\n",
                                        c_type(fty),
                                        sym->type->shape.fields[j].name);
                        }
                        fprintf(emit->out, "} %s;\n", sym->type->shape.name);
                        emitted_shapes[emitted_count++] = sym->type->shape.name;
                    }
                }
            }
        }
    }
    fprintf(emit->out, "\n");

    /* Extern declarations for imported module functions (from symtab) */
    for (Scope *scope = emit->symtab->current; scope; scope = scope->parent) {
        for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
            if (sym->type && sym->type->kind == TYPE_FUNC) {
                /* Skip if this function is defined locally */
                bool is_local = false;
                for (int i = 0; i < program->program.decl_count; i++) {
                    AstNode *d = program->program.decls[i];
                    if (d->kind == NODE_FN_DECL && strcmp(d->fn_decl.name, sym->name) == 0)
                        { is_local = true; break; }
                    if (d->kind == NODE_SHAPE_DECL) {
                        char mangled[256];
                        for (int j = 0; j < d->shape_decl.method_count; j++) {
                            snprintf(mangled, sizeof(mangled), "%s_%s",
                                     d->shape_decl.name, d->shape_decl.methods[j]->fn_decl.name);
                            if (strcmp(mangled, sym->name) == 0) { is_local = true; break; }
                        }
                    }
                    if (d->kind == NODE_COND_DECL && d->cond_decl.active) {
                        for (int j = 0; j < d->cond_decl.decl_count; j++) {
                            if (d->cond_decl.decls[j]->kind == NODE_FN_DECL &&
                                strcmp(d->cond_decl.decls[j]->fn_decl.name, sym->name) == 0)
                                { is_local = true; break; }
                        }
                    }
                    if (d->kind == NODE_EXTERN_BLOCK) {
                        for (int j = 0; j < d->extern_block.decl_count; j++) {
                            if (strcmp(d->extern_block.decls[j]->extern_fn_decl.name, sym->name) == 0)
                                { is_local = true; break; }
                        }
                    }
                    if (is_local) break;
                }
                /* Skip builtins (mix_*) and runtime functions */
                if (strncmp(sym->name, "mix_", 4) == 0) is_local = true;
                /* Skip already-declared names (print, to_string, etc) */
                const char *builtins[] = {"print","sqrt","abs","sin","cos","tan","log",
                    "floor","ceil","round","pow","min","max","to_string","to_int",
                    "to_float","to_set","str_reverse","str_count","file_open","file_read",
                    "file_write","file_close","file_read_all","file_write_all","file_exists",
                    "list_dir","shell","shell_output","env","exit","getcwd","mkdir","args",
                    "ord","chr","alloc","bytes","peek_u32","peek_byte","peek_f32","peek_ptr","memcpy","poke_f32","poke_u32","poke_ptr","pack2","pack3","list_to_f32","free_mem",
                    "panic","assert","len","type_of","sizeof",
                    "random_seed","random_int","random_float","time_now_ms","int_to_hex","int_to_bin",NULL};
                for (int b = 0; builtins[b]; b++) {
                    if (strcmp(sym->name, builtins[b]) == 0) { is_local = true; break; }
                }
                if (is_local) continue;

                MixType *rt = sym->type->func.return_type;
                const char *rs = rt ? c_type(rt) : "void";
                if (rt && (rt->kind == TYPE_RESULT || rt->kind == TYPE_OPTIONAL)) rs = "void *";
                else if (rt && rt->kind == TYPE_SHAPE) rs = "void *";
                fprintf(emit->out, "extern %s %s(", rs, sym->name);
                for (int j = 0; j < sym->type->func.param_count; j++) {
                    if (j > 0) fprintf(emit->out, ", ");
                    MixType *pt = sym->type->func.param_types[j];
                    if (pt && pt->kind == TYPE_SHAPE) fprintf(emit->out, "void *");
                    else if (pt && type_is_float(pt)) fprintf(emit->out, "double");
                    else fprintf(emit->out, "%s", c_type(pt));
                }
                if (sym->type->func.param_count == 0) fprintf(emit->out, "void");
                fprintf(emit->out, ");\n");
            }
        }
    }

    /* Emit extern declarations for extern block functions (from use c or explicit extern) */
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *d = program->program.decls[i];
        if (d->kind == NODE_EXTERN_BLOCK) {
            for (int j = 0; j < d->extern_block.decl_count; j++) {
                AstNode *efn = d->extern_block.decls[j];
                Symbol *sym = symtab_lookup(emit->symtab, efn->extern_fn_decl.name);
                if (!sym || !sym->type || sym->type->kind != TYPE_FUNC) continue;
                MixType *rt = sym->type->func.return_type;
                const char *rs = rt ? c_type(rt) : "void";
                if (rt && (rt->kind == TYPE_RESULT || rt->kind == TYPE_OPTIONAL)) rs = "void *";
                else if (rt && rt->kind == TYPE_SHAPE) rs = "void *";
                if (efn->extern_fn_decl.c_name) {
                    // Function pointer global (e.g. GLAD): extern void (*glad_glClear)(unsigned int);
                    fprintf(emit->out, "extern %s (*%s)(", rs, efn->extern_fn_decl.c_name);
                } else {
                    fprintf(emit->out, "extern %s %s(", rs, efn->extern_fn_decl.name);
                }
                for (int k = 0; k < sym->type->func.param_count; k++) {
                    if (k > 0) fprintf(emit->out, ", ");
                    MixType *pt = sym->type->func.param_types[k];
                    if (pt && pt->kind == TYPE_SHAPE) fprintf(emit->out, "void *");
                    else if (pt && type_is_float(pt)) fprintf(emit->out, "double");
                    else fprintf(emit->out, "%s", c_type(pt));
                }
                if (sym->type->func.param_count == 0) fprintf(emit->out, "void");
                fprintf(emit->out, ");\n");
            }
        }
    }

    /* Imported pub globals from other modules — declare extern so the
     * linker resolves `v_<name>` to the defining module's storage. We
     * scan the symtab for is_global symbols not declared in the current
     * program AST. */
    {
        char *local_globals[256];
        int local_count = 0;
        for (int i = 0; i < program->program.decl_count && local_count < 256; i++) {
            AstNode *d = program->program.decls[i];
            if (d->kind == NODE_VAR_DECL && d->var_decl.is_global)
                local_globals[local_count++] = d->var_decl.name;
        }
        for (Scope *scope = emit->symtab->current; scope; scope = scope->parent) {
            for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
                if (!sym->is_global) continue;
                bool is_local = false;
                for (int j = 0; j < local_count; j++) {
                    if (strcmp(local_globals[j], sym->name) == 0) {
                        is_local = true;
                        break;
                    }
                }
                if (is_local) continue;
                MixType *t = sym->type;
                const char *ty;
                if (t && type_is_float(t)) ty = "double";
                else if (t && t->kind == TYPE_STR) ty = "const char *";
                else ty = "int64_t";
                fprintf(emit->out, "extern %s v_%s;\n", ty, sym->name);
            }
        }
    }

    /* Module-level mutables (`name! = literal` at module scope). Emitted
     * as file-scope statics so reads/writes through `v_<name>` resolve.
     * `pub` ones drop the `static` qualifier so other translation units
     * can link against them. */
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind != NODE_VAR_DECL || !decl->var_decl.is_global) continue;
        AstNode *init = decl->var_decl.init_expr;
        bool neg = false;
        if (init && init->kind == NODE_UNARY_EXPR && init->unary.op == TOK_MINUS) {
            neg = true;
            init = init->unary.operand;
        }
        const char *qualifier = decl->var_decl.is_pub ? "" : "static ";
        const char *name = decl->var_decl.name;
        if (init && init->kind == NODE_INT_LIT) {
            int64_t v = neg ? -init->int_lit.value : init->int_lit.value;
            fprintf(emit->out, "%sint64_t v_%s = %" PRId64 ";\n", qualifier, name, v);
        } else if (init && init->kind == NODE_FLOAT_LIT) {
            double v = neg ? -init->float_lit.value : init->float_lit.value;
            fprintf(emit->out, "%sdouble v_%s = %.17g;\n", qualifier, name, v);
        } else if (init && init->kind == NODE_BOOL_LIT) {
            fprintf(emit->out, "%sint64_t v_%s = %d;\n",
                    qualifier, name, init->bool_lit.value ? 1 : 0);
        } else if (init && init->kind == NODE_STRING_LIT) {
            // Quote with C escapes. The runtime treats strings as
            // const char *, so emit a string literal directly.
            fprintf(emit->out, "%sconst char * v_%s = ", qualifier, name);
            fputc('"', emit->out);
            for (int k = 0; k < init->string_lit.length; k++) {
                char c = init->string_lit.value[k];
                if (c == '\\' || c == '"') fprintf(emit->out, "\\%c", c);
                else if (c == '\n') fprintf(emit->out, "\\n");
                else if (c == '\t') fprintf(emit->out, "\\t");
                else if (c == '\r') fprintf(emit->out, "\\r");
                else if ((unsigned char)c < 32) fprintf(emit->out, "\\x%02x", c & 0xff);
                else fputc(c, emit->out);
            }
            fputs("\";\n", emit->out);
        }
    }

    /* Forward declarations for functions and methods */
    fprintf(emit->out, "/* Forward declarations */\n");
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            if (strcmp(decl->fn_decl.name, "main") == 0) continue;
            MixType *ret_type = decl->fn_decl.return_type
                ? decl->fn_decl.return_type->resolved_type : NULL;
            Symbol *fs = symtab_lookup(emit->symtab, decl->fn_decl.name);
            if (fs && fs->type && fs->type->kind == TYPE_FUNC &&
                fs->type->func.return_type &&
                fs->type->func.return_type->kind == TYPE_RESULT)
                ret_type = fs->type->func.return_type;

            const char *ret_str = ret_type ? c_type(ret_type) : "void";
            if (ret_type && (ret_type->kind == TYPE_RESULT || ret_type->kind == TYPE_OPTIONAL))
                ret_str = "void *";
            else if (ret_type && ret_type->kind == TYPE_SHAPE)
                ret_str = "void *";

            const char *static_kw = decl->fn_decl.is_pub ? "" : "static ";
            fprintf(emit->out, "%s%s %s(", static_kw, ret_str, decl->fn_decl.name);
            for (int j = 0; j < decl->fn_decl.param_count; j++) {
                if (j > 0) fprintf(emit->out, ", ");
                Param *p = &decl->fn_decl.params[j];
                MixType *pt = p->type ? p->type->resolved_type : NULL;
                if (pt && pt->kind == TYPE_SHAPE)
                    fprintf(emit->out, "%s *", pt->shape.name);
                else if (pt && type_is_float(pt))
                    fprintf(emit->out, "double");
                else
                    fprintf(emit->out, "%s", c_type(pt));
            }
            if (decl->fn_decl.param_count == 0) fprintf(emit->out, "void");
            fprintf(emit->out, ");\n");
        } else if (decl->kind == NODE_SHAPE_DECL) {
            if (decl->shape_decl.type_param_count > 0) continue;
            Symbol *shape_sym = symtab_lookup(emit->symtab, decl->shape_decl.name);
            MixType *shape_type = (shape_sym && shape_sym->type) ? shape_sym->type : NULL;
            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                AstNode *m = decl->shape_decl.methods[j];
                MixType *mret = m->fn_decl.return_type
                    ? m->fn_decl.return_type->resolved_type : NULL;
                const char *mret_str = mret ? c_type(mret) : "void";
                if (mret && (mret->kind == TYPE_RESULT || mret->kind == TYPE_OPTIONAL))
                    mret_str = "void *";
                else if (mret && mret->kind == TYPE_SHAPE)
                    mret_str = "void *";

                const char *skw = decl->shape_decl.is_pub ? "" : "static ";
                fprintf(emit->out, "%s%s %s_%s(%s *",
                        skw, mret_str, decl->shape_decl.name, m->fn_decl.name, decl->shape_decl.name);
                for (int k = 0; k < m->fn_decl.param_count; k++) {
                    fprintf(emit->out, ", ");
                    Param *p = &m->fn_decl.params[k];
                    MixType *pt = p->type ? p->type->resolved_type : NULL;
                    if (pt && pt->kind == TYPE_SHAPE)
                        fprintf(emit->out, "%s *", pt->shape.name);
                    else if (pt && type_is_float(pt))
                        fprintf(emit->out, "double");
                    else
                        fprintf(emit->out, "%s", c_type(pt));
                }
                fprintf(emit->out, ");\n");
            }
            (void)shape_type;
        } else if (decl->kind == NODE_COND_DECL && decl->cond_decl.active) {
            for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                AstNode *cd = decl->cond_decl.decls[j];
                if (cd->kind == NODE_FN_DECL && strcmp(cd->fn_decl.name, "main") != 0) {
                    MixType *rt = cd->fn_decl.return_type
                        ? cd->fn_decl.return_type->resolved_type : NULL;
                    const char *rs = rt ? c_type(rt) : "void";
                    if (rt && (rt->kind == TYPE_RESULT || rt->kind == TYPE_OPTIONAL)) rs = "void *";
                    fprintf(emit->out, "static %s %s(", rs, cd->fn_decl.name);
                    for (int k = 0; k < cd->fn_decl.param_count; k++) {
                        if (k > 0) fprintf(emit->out, ", ");
                        Param *p = &cd->fn_decl.params[k];
                        MixType *pt = p->type ? p->type->resolved_type : NULL;
                        fprintf(emit->out, "%s", c_type(pt));
                    }
                    if (cd->fn_decl.param_count == 0) fprintf(emit->out, "void");
                    fprintf(emit->out, ");\n");
                }
            }
        }
    }
    fprintf(emit->out, "\n");

    /* Pre-scan for lambdas and emit their forward declarations */
    prescan_lambdas(emit, program);
    for (int i = 0; i < emit->lambda_count; i++) {
        AstNode *lam = emit->lambdas[i];
        fprintf(emit->out, "static int64_t %s(", lam->lambda.generated_name);
        for (int j = 0; j < lam->lambda.param_count; j++) {
            if (j > 0) fprintf(emit->out, ", ");
            fprintf(emit->out, "int64_t");
        }
        if (lam->lambda.param_count == 0) fprintf(emit->out, "void");
        fprintf(emit->out, ");\n");
    }
    /* Reset lambda state for collection during emission (already named) */
    emit->lambda_count = 0;
    emit->lambda_counter = 0;
    fprintf(emit->out, "\n");

    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            emit_fn_decl(emit, decl);
        } else if (decl->kind == NODE_SHAPE_DECL) {
            // Skip generic shape templates — instantiations emit as their own decls.
            if (decl->shape_decl.type_param_count > 0) continue;
            Symbol *shape_sym = symtab_lookup(emit->symtab, decl->shape_decl.name);
            MixType *shape_type = (shape_sym && shape_sym->type) ? shape_sym->type : NULL;
            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                emit_method(emit, decl->shape_decl.methods[j],
                            decl->shape_decl.name, shape_type, decl->shape_decl.is_pub);
            }
        } else if (decl->kind == NODE_COND_DECL && decl->cond_decl.active) {
            for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                AstNode *cd = decl->cond_decl.decls[j];
                if (cd->kind == NODE_FN_DECL)
                    emit_fn_decl(emit, cd);
            }
        }
    }

    /* Emit lambdas */
    for (int i = 0; i < emit->lambda_count; i++) {
        AstNode *lam = emit->lambdas[i];
        const char *lname = lam->lambda.generated_name;
        MixType *body_type = lam->lambda.body ? lam->lambda.body->resolved_type : NULL;
        const char *ret = body_type ? c_type(body_type) : "int64_t";

        fprintf(emit->out, "static %s %s(", ret, lname);
        for (int j = 0; j < lam->lambda.param_count; j++) {
            if (j > 0) fprintf(emit->out, ", ");
            fprintf(emit->out, "int64_t p_%s", lam->lambda.param_names[j]);
        }
        if (lam->lambda.param_count == 0) fprintf(emit->out, "void");
        fprintf(emit->out, ") {\n");
        emit->indent = 1;

        cname_reset(emit);
        for (int j = 0; j < lam->lambda.param_count; j++) {
            ind(emit); fprintf(emit->out, "int64_t v_%s = p_%s;\n",
                    lam->lambda.param_names[j], lam->lambda.param_names[j]);
        }

        int val = emit_expr(emit, lam->lambda.body);
        ind(emit); fprintf(emit->out, "return t%d;\n", val);
        fprintf(emit->out, "}\n\n");
    }
}
