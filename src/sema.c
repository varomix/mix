#include "sema.h"
#include "errors.h"
#include "arena.h"

// --- Levenshtein distance for "did you mean?" suggestions ---

static int levenshtein(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    // Use two rows to save stack space
    int prev[128], curr[128];
    if (lb >= 128) return 99;
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j-1] + 1;
            int sub = prev[j-1] + cost;
            curr[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
        for (int j = 0; j <= lb; j++) prev[j] = curr[j];
    }
    return prev[lb];
}

// Search all scopes for the closest name within max_dist.
// Returns the best match name, or NULL if none found.
static const char *find_similar_name(SymTab *st, const char *name, int max_dist) {
    const char *best = NULL;
    int best_dist = max_dist + 1;
    for (Scope *scope = st->current; scope; scope = scope->parent) {
        for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
            int d = levenshtein(name, sym->name);
            if (d > 0 && d < best_dist) {
                best_dist = d;
                best = sym->name;
            }
        }
    }
    return best;
}

Sema sema_create(Arena *arena) {
    Sema sema = {0};
    sema.arena = arena;
    sema.symtab = symtab_create(arena);
    return sema;
}

static MixType *make_type(Arena *a, TypeKind kind) {
    MixType *t = arena_alloc(a, sizeof(MixType));
    memset(t, 0, sizeof(MixType));
    t->kind = kind;
    return t;
}

static MixType *make_ptr_type(Arena *a, MixType *base) {
    MixType *t = make_type(a, TYPE_PTR);
    t->ptr.base = base;
    return t;
}

static MixType *make_list_type(Arena *a, MixType *elem) {
    MixType *t = make_type(a, TYPE_LIST);
    t->list.elem_type = elem;
    return t;
}

static MixType *make_map_type(Arena *a, MixType *key, MixType *val) {
    MixType *t = make_type(a, TYPE_MAP);
    t->map.key_type = key;
    t->map.val_type = val;
    return t;
}

static MixType *make_set_type(Arena *a, MixType *elem) {
    MixType *t = make_type(a, TYPE_SET);
    t->set.elem_type = elem;
    return t;
}

/* Produce a readable type name: int, [int], [str], map[str,int], set[str], ShapeName */
static const char *type_name(Arena *a, MixType *t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TYPE_LIST: {
            const char *inner = type_name(a, t->list.elem_type);
            char buf[128];
            snprintf(buf, sizeof(buf), "[%s]", inner);
            return arena_strdup(a, buf);
        }
        case TYPE_MAP: {
            const char *k = type_name(a, t->map.key_type);
            const char *v = type_name(a, t->map.val_type);
            char buf[128];
            snprintf(buf, sizeof(buf), "map[%s, %s]", k, v);
            return arena_strdup(a, buf);
        }
        case TYPE_SET: {
            const char *inner = type_name(a, t->set.elem_type);
            char buf[128];
            snprintf(buf, sizeof(buf), "set[%s]", inner);
            return arena_strdup(a, buf);
        }
        case TYPE_OPTIONAL: {
            const char *inner = type_name(a, t->optional.inner);
            char buf[128];
            snprintf(buf, sizeof(buf), "%s?", inner);
            return arena_strdup(a, buf);
        }
        case TYPE_RESULT: {
            const char *inner = type_name(a, t->result.ok_type);
            char buf[128];
            snprintf(buf, sizeof(buf), "result[%s]", inner);
            return arena_strdup(a, buf);
        }
        case TYPE_SHAPE:
            return t->shape.name ? t->shape.name : "shape";
        case TYPE_FUNC:
            return "func";
        default:
            return type_kind_name(t->kind);
    }
}

/* Check if two types are compatible (same kind + matching inner types) */
static bool types_compatible(MixType *expected, MixType *actual) {
    if (!expected || !actual) return true;  /* unknown types — don't error */
    if (expected->kind == TYPE_INFER || actual->kind == TYPE_INFER) return true;
    if (expected->kind == TYPE_GENERIC || actual->kind == TYPE_GENERIC) return true;
    if (expected->kind == TYPE_NAMED || actual->kind == TYPE_NAMED) return true;
    if (expected->kind == TYPE_VOID || actual->kind == TYPE_VOID) return true;

    /* Numeric promotion: int types are compatible with each other */
    if (type_is_integer(expected) && type_is_integer(actual)) return true;
    /* Float types compatible with each other */
    if (type_is_float(expected) && type_is_float(actual)) return true;
    /* Int and float are compatible (implicit conversion) */
    if (type_is_numeric(expected) && type_is_numeric(actual)) return true;

    /* ptr and str are compatible (strings are pointers) */
    if ((expected->kind == TYPE_PTR && actual->kind == TYPE_STR) ||
        (expected->kind == TYPE_STR && actual->kind == TYPE_PTR)) return true;
    /* ptr is compatible with any pointer-like type */
    if (expected->kind == TYPE_PTR || actual->kind == TYPE_PTR) return true;
    /* func type is compatible with any func-like value */
    if (expected->kind == TYPE_FUNC || actual->kind == TYPE_FUNC) return true;

    if (expected->kind != actual->kind) return false;

    /* For parameterized types, check inner types */
    switch (expected->kind) {
        case TYPE_LIST:
            return types_compatible(expected->list.elem_type, actual->list.elem_type);
        case TYPE_MAP:
            return types_compatible(expected->map.key_type, actual->map.key_type) &&
                   types_compatible(expected->map.val_type, actual->map.val_type);
        case TYPE_SET:
            return types_compatible(expected->set.elem_type, actual->set.elem_type);
        case TYPE_OPTIONAL:
            return types_compatible(expected->optional.inner, actual->optional.inner);
        case TYPE_RESULT:
            return types_compatible(expected->result.ok_type, actual->result.ok_type);
        case TYPE_SHAPE:
            if (expected->shape.name && actual->shape.name)
                return strcmp(expected->shape.name, actual->shape.name) == 0;
            return true;
        default:
            return true;
    }
}

static MixType *resolve_type_node(Sema *sema, AstNode *type_node) {
    if (!type_node) return make_type(sema->arena, TYPE_VOID);

    if (type_node->kind == NODE_TYPE_PTR) {
        MixType *base = resolve_type_node(sema, type_node->type_ptr.base_type);
        return make_ptr_type(sema->arena, base);
    }

    if (type_node->kind == NODE_TYPE_OPTIONAL) {
        MixType *inner = resolve_type_node(sema, type_node->type_optional.inner_type);
        MixType *opt = make_type(sema->arena, TYPE_OPTIONAL);
        opt->optional.inner = inner;
        return opt;
    }

    if (type_node->kind == NODE_TYPE_NAME) {
        TokenKind tk = type_node->type_name.type_kind;

        // Check if it's a generic type parameter (T, K, etc.)
        if (tk == TOK_IDENT && sema->generic_param_count > 0) {
            for (int i = 0; i < sema->generic_param_count; i++) {
                if (strcmp(sema->generic_params[i], type_node->type_name.name) == 0) {
                    // Generic type param — passes any type check
                    return make_type(sema->arena, TYPE_GENERIC);
                }
            }
        }
        switch (tk) {
            case TOK_INT:     return make_type(sema->arena, TYPE_INT);
            case TOK_FLOAT:   return make_type(sema->arena, TYPE_FLOAT);
            case TOK_BOOL:    return make_type(sema->arena, TYPE_BOOL);
            case TOK_BYTE:    return make_type(sema->arena, TYPE_BYTE);
            case TOK_STR:     return make_type(sema->arena, TYPE_STR);
            case TOK_INT8:    return make_type(sema->arena, TYPE_INT8);
            case TOK_INT16:   return make_type(sema->arena, TYPE_INT16);
            case TOK_INT32:   return make_type(sema->arena, TYPE_INT32);
            case TOK_INT64:   return make_type(sema->arena, TYPE_INT64);
            case TOK_UINT8:   return make_type(sema->arena, TYPE_UINT8);
            case TOK_UINT16:  return make_type(sema->arena, TYPE_UINT16);
            case TOK_UINT32:  return make_type(sema->arena, TYPE_UINT32);
            case TOK_UINT64:  return make_type(sema->arena, TYPE_UINT64);
            case TOK_FLOAT32: return make_type(sema->arena, TYPE_FLOAT32);
            case TOK_FLOAT64: return make_type(sema->arena, TYPE_FLOAT64);
            case TOK_IDENT: {
                // Look up in symbol table — could be a shape type
                Symbol *sym = symtab_lookup(&sema->symtab, type_node->type_name.name);
                if (sym && sym->type && sym->type->kind == TYPE_SHAPE) {
                    return sym->type;
                }
                // Opaque named type (e.g., SDL_Window)
                MixType *t = make_type(sema->arena, TYPE_NAMED);
                t->named.name = type_node->type_name.name;
                return t;
            }
            case TOK_LBRACKET: {
                // List type: [T]
                MixType *elem = resolve_type_node(sema, type_node->type_ptr.base_type);
                return make_list_type(sema->arena, elem);
            }
            case TOK_SET: {
                // Set type: set[T]
                MixType *elem = resolve_type_node(sema, type_node->type_ptr.base_type);
                return make_set_type(sema->arena, elem);
            }
            default:
                mix_error(type_node->loc, "unknown type");
                return make_type(sema->arena, TYPE_INT);
        }
    }

    return make_type(sema->arena, TYPE_VOID);
}

static MixType *resolve_expr(Sema *sema, AstNode *expr);

static MixType *resolve_expr(Sema *sema, AstNode *expr) {
    if (!expr) return make_type(sema->arena, TYPE_VOID);

    switch (expr->kind) {
        case NODE_INT_LIT:
            expr->resolved_type = make_type(sema->arena, TYPE_INT);
            return expr->resolved_type;
        case NODE_FLOAT_LIT:
            expr->resolved_type = make_type(sema->arena, TYPE_FLOAT);
            return expr->resolved_type;
        case NODE_STRING_LIT:
            expr->resolved_type = make_type(sema->arena, TYPE_STR);
            return expr->resolved_type;
        case NODE_STRING_INTERP:
            // Resolve all embedded expressions
            for (int i2 = 0; i2 < expr->string_interp.expr_count; i2++) {
                resolve_expr(sema, expr->string_interp.exprs[i2]);
            }
            expr->resolved_type = make_type(sema->arena, TYPE_STR);
            return expr->resolved_type;
        case NODE_BOOL_LIT:
            expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
            return expr->resolved_type;
        case NODE_NONE_LIT:
            expr->resolved_type = make_type(sema->arena, TYPE_VOID);
            return expr->resolved_type;
        case NODE_IDENT: {
            Symbol *sym = symtab_lookup(&sema->symtab, expr->ident.name);
            if (!sym) {
                mix_error(expr->loc, "undefined variable '%s'", expr->ident.name);
                const char *suggestion = find_similar_name(&sema->symtab, expr->ident.name, 2);
                if (suggestion) mix_note(expr->loc, "did you mean '%s'?", suggestion);
                expr->resolved_type = make_type(sema->arena, TYPE_INT);
            } else {
                expr->resolved_type = sym->type;
                // Mark whether the variable was declared as mutable
                // (needed by emitter to know if load or copy)
                expr->ident.is_mutable = sym->is_mutable;
            }
            return expr->resolved_type;
        }
        case NODE_BINARY_EXPR: {
            MixType *left = resolve_expr(sema, expr->binary.left);
            resolve_expr(sema, expr->binary.right);

            // String concatenation: str + str -> str
            if (left && left->kind == TYPE_STR && expr->binary.op == TOK_PLUS) {
                expr->resolved_type = make_type(sema->arena, TYPE_STR);
                return expr->resolved_type;
            }

            // Check for operator overloading on shapes
            if (left && left->kind == TYPE_SHAPE) {
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
                    snprintf(mangled, sizeof(mangled), "%s_%s", left->shape.name, op_method);
                    Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                    if (msym && msym->type && msym->type->kind == TYPE_FUNC) {
                        expr->resolved_type = msym->type->func.return_type;
                        return expr->resolved_type;
                    }
                }
            }

            if (expr->binary.op == TOK_EQEQ || expr->binary.op == TOK_NEQ ||
                expr->binary.op == TOK_LT || expr->binary.op == TOK_GT ||
                expr->binary.op == TOK_LTE || expr->binary.op == TOK_GTE ||
                expr->binary.op == TOK_AND || expr->binary.op == TOK_OR) {
                expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
            } else {
                expr->resolved_type = left;
            }
            return expr->resolved_type;
        }
        case NODE_UNARY_EXPR: {
            MixType *operand = resolve_expr(sema, expr->unary.operand);
            if (expr->unary.op == TOK_NOT) {
                expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
            } else if (expr->unary.op == TOK_AMPERSAND) {
                expr->resolved_type = make_ptr_type(sema->arena, operand);
            } else if (expr->unary.op == TOK_STAR && operand->kind == TYPE_PTR) {
                expr->resolved_type = operand->ptr.base;
            } else {
                expr->resolved_type = operand;
            }
            return expr->resolved_type;
        }
        case NODE_ELSE_EXPR: {
            MixType *val_type = resolve_expr(sema, expr->else_expr.value);
            resolve_expr(sema, expr->else_expr.fallback);
            // Result type is the inner type of the optional or result
            if (val_type && val_type->kind == TYPE_OPTIONAL) {
                expr->resolved_type = val_type->optional.inner;
            } else if (val_type && val_type->kind == TYPE_RESULT) {
                expr->resolved_type = val_type->result.ok_type;
            } else {
                // If not optional/result, just use the value type directly
                expr->resolved_type = val_type;
            }
            return expr->resolved_type;
        }
        case NODE_LAMBDA: {
            // Resolve lambda body with params in scope
            symtab_push_scope(&sema->symtab);
            for (int i2 = 0; i2 < expr->lambda.param_count; i2++) {
                // Lambda params have inferred types for now
                symtab_insert(&sema->symtab, expr->lambda.param_names[i2],
                              make_type(sema->arena, TYPE_INFER), false);
            }
            MixType *body_type = resolve_expr(sema, expr->lambda.body);
            symtab_pop_scope(&sema->symtab);

            // Build a function type for the lambda
            MixType *func_type = make_type(sema->arena, TYPE_FUNC);
            func_type->func.return_type = body_type;
            func_type->func.param_count = expr->lambda.param_count;
            func_type->func.param_types = arena_alloc(sema->arena,
                sizeof(MixType*) * expr->lambda.param_count);
            for (int i2 = 0; i2 < expr->lambda.param_count; i2++) {
                func_type->func.param_types[i2] = make_type(sema->arena, TYPE_INFER);
            }
            expr->resolved_type = func_type;
            return expr->resolved_type;
        }
        case NODE_LIST_LIT: {
            // Resolve all element expressions and infer list type from first element
            MixType *elem_type = make_type(sema->arena, TYPE_INT); // default
            for (int i2 = 0; i2 < expr->list_lit.element_count; i2++) {
                MixType *et = resolve_expr(sema, expr->list_lit.elements[i2]);
                if (i2 == 0 && et) elem_type = et;
            }
            expr->resolved_type = make_list_type(sema->arena, elem_type);
            return expr->resolved_type;
        }
        case NODE_MAP_LIT: {
            MixType *key_type = make_type(sema->arena, TYPE_STR);
            MixType *val_type = make_type(sema->arena, TYPE_INT);
            for (int i2 = 0; i2 < expr->map_lit.entry_count; i2++) {
                MixType *kt = resolve_expr(sema, expr->map_lit.keys[i2]);
                MixType *vt = resolve_expr(sema, expr->map_lit.values[i2]);
                if (i2 == 0) { if (kt) key_type = kt; if (vt) val_type = vt; }
            }
            expr->resolved_type = make_map_type(sema->arena, key_type, val_type);
            return expr->resolved_type;
        }
        case NODE_SET_LIT: {
            MixType *elem_type = make_type(sema->arena, TYPE_STR); // default
            for (int i2 = 0; i2 < expr->set_lit.element_count; i2++) {
                MixType *et = resolve_expr(sema, expr->set_lit.elements[i2]);
                if (i2 == 0 && et) elem_type = et;
            }
            expr->resolved_type = make_set_type(sema->arena, elem_type);
            return expr->resolved_type;
        }
        case NODE_CAST_EXPR: {
            resolve_expr(sema, expr->cast_expr.value);
            TypeKind tk = TYPE_INT;
            switch (expr->cast_expr.target_type) {
                case TOK_INT:     tk = TYPE_INT; break;
                case TOK_FLOAT:   tk = TYPE_FLOAT; break;
                case TOK_BOOL:    tk = TYPE_BOOL; break;
                case TOK_BYTE:    tk = TYPE_BYTE; break;
                case TOK_INT8:    tk = TYPE_INT8; break;
                case TOK_INT16:   tk = TYPE_INT16; break;
                case TOK_INT32:   tk = TYPE_INT32; break;
                case TOK_INT64:   tk = TYPE_INT64; break;
                case TOK_UINT8:   tk = TYPE_UINT8; break;
                case TOK_UINT16:  tk = TYPE_UINT16; break;
                case TOK_UINT32:  tk = TYPE_UINT32; break;
                case TOK_UINT64:  tk = TYPE_UINT64; break;
                case TOK_FLOAT32: tk = TYPE_FLOAT32; break;
                case TOK_FLOAT64: tk = TYPE_FLOAT64; break;
                default: break;
            }
            expr->resolved_type = make_type(sema->arena, tk);
            return expr->resolved_type;
        }
        case NODE_INDEX_EXPR: {
            MixType *obj_type = resolve_expr(sema, expr->index_expr.object);
            resolve_expr(sema, expr->index_expr.index);
            if (obj_type && obj_type->kind == TYPE_LIST) {
                expr->resolved_type = obj_type->list.elem_type;
            } else if (obj_type && obj_type->kind == TYPE_MAP) {
                expr->resolved_type = obj_type->map.val_type;
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_INT);
            }
            return expr->resolved_type;
        }
        case NODE_SLICE_EXPR: {
            MixType *obj_type = resolve_expr(sema, expr->slice_expr.object);
            if (expr->slice_expr.start) resolve_expr(sema, expr->slice_expr.start);
            if (expr->slice_expr.end) resolve_expr(sema, expr->slice_expr.end);
            // Slice of a list returns same list type
            if (obj_type && obj_type->kind == TYPE_LIST) {
                expr->resolved_type = obj_type;
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_VOID);
            }
            return expr->resolved_type;
        }
        case NODE_LIST_COMP: {
            MixType *iter_type = resolve_expr(sema, expr->list_comp.iterable);
            symtab_push_scope(&sema->symtab);
            MixType *var_type = make_type(sema->arena, TYPE_INT);
            if (iter_type && iter_type->kind == TYPE_LIST && iter_type->list.elem_type) {
                var_type = iter_type->list.elem_type;
            }
            symtab_insert(&sema->symtab, expr->list_comp.var_name, var_type, true);
            MixType *elem_type = resolve_expr(sema, expr->list_comp.expr);
            if (expr->list_comp.condition) resolve_expr(sema, expr->list_comp.condition);
            symtab_pop_scope(&sema->symtab);
            expr->resolved_type = make_list_type(sema->arena, elem_type);
            return expr->resolved_type;
        }
        case NODE_CALL_EXPR: {
            Symbol *sym = symtab_lookup(&sema->symtab, expr->call.name);
            if (!sym) {
                mix_error(expr->loc, "undefined function '%s'", expr->call.name);
                const char *suggestion = find_similar_name(&sema->symtab, expr->call.name, 2);
                if (suggestion) mix_note(expr->loc, "did you mean '%s'?", suggestion);
                expr->resolved_type = make_type(sema->arena, TYPE_INT);
                for (int i = 0; i < expr->call.arg_count; i++) {
                    resolve_expr(sema, expr->call.args[i]);
                }
                return expr->resolved_type;
            } else if (sym->type && sym->type->kind == TYPE_SHAPE) {
                // Positional shape construction: Name(val, val, ...)
                // Resolve arguments before rewriting the node, since the
                // rewrite overwrites the call union fields.
                for (int i = 0; i < expr->call.arg_count; i++) {
                    resolve_expr(sema, expr->call.args[i]);
                }
                // Rewrite NODE_CALL_EXPR → NODE_SHAPE_LIT in-place
                char *shape_name = expr->call.name;
                AstNode **args = expr->call.args;
                int arg_count = expr->call.arg_count;

                expr->kind = NODE_SHAPE_LIT;
                expr->shape_lit.shape_name = shape_name;
                expr->shape_lit.field_names = NULL;  // positional — filled below
                expr->shape_lit.field_values = args;
                expr->shape_lit.field_count = arg_count;

                // Fall through to NODE_SHAPE_LIT handling
                goto handle_shape_lit;
            } else if (sym->type && sym->type->kind == TYPE_FUNC) {
                expr->resolved_type = sym->type->func.return_type;
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_INT);
            }
            for (int i = 0; i < expr->call.arg_count; i++) {
                resolve_expr(sema, expr->call.args[i]);
            }
            // Type-check arguments against parameter types
            if (sym->type && sym->type->kind == TYPE_FUNC) {
                MixType *ftype = sym->type;
                // Check argument count (skip variadic and builtins with overloads)
                if (!ftype->func.is_variadic && ftype->func.param_count > 0 &&
                    expr->call.arg_count != ftype->func.param_count) {
                    // Skip count check for overloaded builtins (print, min, max, etc.)
                    const char *n = expr->call.name;
                    bool is_overloaded = (strcmp(n, "print") == 0 || strcmp(n, "to_string") == 0 ||
                        strcmp(n, "to_int") == 0 || strcmp(n, "to_float") == 0 ||
                        strcmp(n, "to_set") == 0);
                    if (!is_overloaded) {
                        mix_error(expr->loc, "'%s' expects %d argument(s), got %d",
                                  expr->call.name, ftype->func.param_count, expr->call.arg_count);
                    }
                }
                // Check argument types
                for (int i = 0; i < expr->call.arg_count && i < ftype->func.param_count; i++) {
                    MixType *expected = ftype->func.param_types[i];
                    MixType *actual = expr->call.args[i]->resolved_type;
                    if (!types_compatible(expected, actual)) {
                        mix_error(expr->call.args[i]->loc,
                                  "argument %d of '%s': expected %s, got %s",
                                  i + 1, expr->call.name,
                                  type_name(sema->arena, expected),
                                  type_name(sema->arena, actual));
                    }
                }
            }
            // to_set: infer set element type from list argument
            if (strcmp(expr->call.name, "to_set") == 0 && expr->call.arg_count == 1) {
                MixType *arg_type = expr->call.args[0]->resolved_type;
                if (arg_type && arg_type->kind == TYPE_LIST && arg_type->list.elem_type) {
                    expr->resolved_type = make_set_type(sema->arena, arg_type->list.elem_type);
                }
            }
            return expr->resolved_type;
        }
        handle_shape_lit:
        case NODE_SHAPE_LIT: {
            // Look up the shape type — could be a direct shape or a variant constructor
            Symbol *sym = symtab_lookup(&sema->symtab, expr->shape_lit.shape_name);
            MixType *stype = NULL;
            if (sym && sym->type && sym->type->kind == TYPE_SHAPE) {
                stype = sym->type;
            } else if (sym && sym->type && sym->type->kind == TYPE_FUNC &&
                       sym->type->func.return_type && sym->type->func.return_type->kind == TYPE_SHAPE) {
                stype = sym->type->func.return_type;
            } else {
                mix_error(expr->loc, "unknown shape '%s'", expr->shape_lit.shape_name);
                expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                return expr->resolved_type;
            }
            expr->resolved_type = stype;

            // Fill in field names for positional construction
            if (expr->shape_lit.field_names == NULL && stype && !stype->shape.is_tagged_union) {
                int count = expr->shape_lit.field_count;
                char **names = arena_alloc(sema->arena, sizeof(char*) * count);
                for (int i = 0; i < count && i < stype->shape.field_count; i++) {
                    names[i] = stype->shape.fields[i].name;
                }
                expr->shape_lit.field_names = names;
            }
            // Fill in for tagged union variants
            if (expr->shape_lit.field_names == NULL && stype && stype->shape.is_tagged_union) {
                ShapeVariant *sv = type_find_variant(stype, expr->shape_lit.shape_name);
                if (sv) {
                    int count = expr->shape_lit.field_count;
                    char **names = arena_alloc(sema->arena, sizeof(char*) * count);
                    for (int i = 0; i < count && i < sv->field_count; i++) {
                        names[i] = sv->fields[i].name;
                    }
                    expr->shape_lit.field_names = names;
                }
            }
            // Fill in default values for built-in Project shape (missing fields)
            if (stype && !stype->shape.is_tagged_union &&
                strcmp(stype->shape.name, "Project") == 0 &&
                expr->shape_lit.field_count < stype->shape.field_count &&
                expr->shape_lit.field_names != NULL) {
                // Build new arrays with all fields, filling defaults for missing ones
                int total = stype->shape.field_count;
                char **new_names = arena_alloc(sema->arena, sizeof(char*) * total);
                AstNode **new_values = arena_alloc(sema->arena, sizeof(AstNode*) * total);
                int user_count = expr->shape_lit.field_count;

                for (int fi = 0; fi < total; fi++) {
                    const char *fname = stype->shape.fields[fi].name;
                    // Check if user provided this field
                    bool found = false;
                    for (int ui = 0; ui < user_count; ui++) {
                        if (expr->shape_lit.field_names[ui] &&
                            strcmp(expr->shape_lit.field_names[ui], fname) == 0) {
                            new_names[fi] = expr->shape_lit.field_names[ui];
                            new_values[fi] = expr->shape_lit.field_values[ui];
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        new_names[fi] = stype->shape.fields[fi].name;
                        // Create default AST node
                        AstNode *def = arena_alloc(sema->arena, sizeof(AstNode));
                        memset(def, 0, sizeof(AstNode));
                        def->loc = expr->loc;
                        MixType *ftype = stype->shape.fields[fi].type;
                        if (ftype && ftype->kind == TYPE_BOOL) {
                            def->kind = NODE_BOOL_LIT;
                            def->bool_lit.value = false;
                            def->resolved_type = make_type(sema->arena, TYPE_BOOL);
                        } else if (ftype && ftype->kind == TYPE_LIST) {
                            def->kind = NODE_LIST_LIT;
                            def->list_lit.elements = NULL;
                            def->list_lit.element_count = 0;
                            def->resolved_type = ftype;
                        } else {
                            // String default: ""
                            def->kind = NODE_STRING_LIT;
                            def->string_lit.value = "";
                            def->string_lit.length = 0;
                            def->resolved_type = make_type(sema->arena, TYPE_STR);
                        }
                        new_values[fi] = def;
                    }
                }
                expr->shape_lit.field_names = new_names;
                expr->shape_lit.field_values = new_values;
                expr->shape_lit.field_count = total;
            }
            // Resolve field value expressions
            for (int i = 0; i < expr->shape_lit.field_count; i++) {
                resolve_expr(sema, expr->shape_lit.field_values[i]);
            }
            return expr->resolved_type;
        }
        case NODE_FIELD_EXPR: {
            MixType *obj_type = resolve_expr(sema, expr->field_expr.object);
            if (obj_type && obj_type->kind == TYPE_LIST) {
                // List built-in fields: len
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else {
                    mix_error(expr->loc, "list has no field '%s'", expr->field_expr.field_name);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_type && obj_type->kind == TYPE_MAP) {
                // Map built-in fields: len, keys, values
                const char *fn = expr->field_expr.field_name;
                if (strcmp(fn, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(fn, "keys") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, obj_type->map.key_type);
                } else if (strcmp(fn, "values") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, obj_type->map.val_type);
                } else {
                    mix_error(expr->loc, "map has no field '%s'", fn);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_type && obj_type->kind == TYPE_SET) {
                // Set built-in fields: len, values
                const char *fn = expr->field_expr.field_name;
                if (strcmp(fn, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(fn, "values") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, obj_type->set.elem_type);
                } else {
                    mix_error(expr->loc, "set has no field '%s'", fn);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_type && obj_type->kind == TYPE_STR) {
                // String built-in fields: len
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else {
                    mix_error(expr->loc, "str has no field '%s'", expr->field_expr.field_name);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_type && obj_type->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi = type_find_field(obj_type, expr->field_expr.field_name);
                if (fi) {
                    expr->resolved_type = fi->type;
                } else {
                    // Try zero-param method (computed field): r.area → r.area()
                    char mangled[256];
                    snprintf(mangled, sizeof(mangled), "%s_%s",
                             obj_type->shape.name, expr->field_expr.field_name);
                    Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                    if (msym && msym->type && msym->type->kind == TYPE_FUNC) {
                        expr->resolved_type = msym->type->func.return_type;
                    } else {
                        mix_error(expr->loc, "shape '%s' has no field '%s'",
                                  obj_type->shape.name, expr->field_expr.field_name);
                        expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    }
                }
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_VOID);
            }
            return expr->resolved_type;
        }
        case NODE_METHOD_CALL: {
            MixType *obj_type = resolve_expr(sema, expr->method_call.object);
            // Resolve argument expressions
            for (int i2 = 0; i2 < expr->method_call.arg_count; i2++) {
                resolve_expr(sema, expr->method_call.args[i2]);
            }
            // Shared built-in methods: .read(), .update!(fn)
            if (obj_type && obj_type->kind == TYPE_SHARED) {
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "read") == 0) {
                    expr->resolved_type = obj_type->shared.inner;
                } else if (strcmp(m, "update") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else {
                    mix_error(expr->loc, "shared has no method '%s'", m);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // String built-in methods
            if (obj_type && obj_type->kind == TYPE_STR) {
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "upper") == 0 || strcmp(m, "lower") == 0 ||
                    strcmp(m, "trim") == 0 || strcmp(m, "replace") == 0 ||
                    strcmp(m, "char_at") == 0 || strcmp(m, "slice") == 0 ||
                    strcmp(m, "repeat") == 0 || strcmp(m, "reverse") == 0 ||
                    strcmp(m, "sort") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_STR);
                } else if (strcmp(m, "split") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, make_type(sema->arena, TYPE_STR));
                } else if (strcmp(m, "contains") == 0 || strcmp(m, "starts_with") == 0 ||
                           strcmp(m, "ends_with") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "index_of") == 0 || strcmp(m, "code") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(m, "join") == 0) {
                    // Actually join is on lists, but we handle "str".join() as a possible pattern too
                    expr->resolved_type = make_type(sema->arena, TYPE_STR);
                } else {
                    mix_error(expr->loc, "str has no method '%s'", m);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // List built-in methods
            if (obj_type && obj_type->kind == TYPE_LIST) {
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "push") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (strcmp(m, "pop") == 0) {
                    expr->resolved_type = obj_type->list.elem_type
                        ? obj_type->list.elem_type : make_type(sema->arena, TYPE_INT);
                } else if (strcmp(m, "remove") == 0 || strcmp(m, "insert") == 0 ||
                           strcmp(m, "sort") == 0 || strcmp(m, "reverse") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (strcmp(m, "contains") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "index_of") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(m, "join") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_STR);
                } else {
                    mix_error(expr->loc, "list has no method '%s'", m);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // Map built-in methods
            if (obj_type && obj_type->kind == TYPE_MAP) {
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "has") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "remove") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else {
                    mix_error(expr->loc, "map has no method '%s'", m);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // Set built-in methods
            if (obj_type && obj_type->kind == TYPE_SET) {
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "has") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "add") == 0 || strcmp(m, "remove") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (strcmp(m, "union") == 0 || strcmp(m, "intersect") == 0 ||
                           strcmp(m, "diff") == 0) {
                    expr->resolved_type = make_set_type(sema->arena, obj_type->set.elem_type);
                } else {
                    mix_error(expr->loc, "set has no method '%s'", m);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // Look up ShapeName_methodName
            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                         obj_type->shape.name, expr->method_call.method_name);
                Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                if (msym && msym->type && msym->type->kind == TYPE_FUNC) {
                    expr->resolved_type = msym->type->func.return_type;
                    // Check method argument types (param 0 is self, user args start at 1)
                    MixType *mtype = msym->type;
                    for (int i2 = 0; i2 < expr->method_call.arg_count &&
                         i2 + 1 < mtype->func.param_count; i2++) {
                        MixType *expected = mtype->func.param_types[i2 + 1];
                        MixType *actual = expr->method_call.args[i2]->resolved_type;
                        if (!types_compatible(expected, actual)) {
                            mix_error(expr->method_call.args[i2]->loc,
                                      "argument %d of '%s.%s': expected %s, got %s",
                                      i2 + 1, obj_type->shape.name,
                                      expr->method_call.method_name,
                                      type_name(sema->arena, expected),
                                      type_name(sema->arena, actual));
                        }
                    }
                } else {
                    mix_error(expr->loc, "shape '%s' has no method '%s'",
                              obj_type->shape.name, expr->method_call.method_name);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_VOID);
            }
            return expr->resolved_type;
        }
        case NODE_SHARED_EXPR: {
            MixType *inner = resolve_expr(sema, expr->shared_expr.init_expr);
            MixType *t = make_type(sema->arena, TYPE_SHARED);
            t->shared.inner = inner;
            expr->resolved_type = t;
            return expr->resolved_type;
        }
        case NODE_GO_EXPR: {
            MixType *call_type = resolve_expr(sema, expr->go_expr.call_expr);
            MixType *t = make_type(sema->arena, TYPE_TASK);
            t->task.result_type = call_type;
            expr->resolved_type = t;
            return expr->resolved_type;
        }
        case NODE_WAIT_EXPR: {
            MixType *handle_type = resolve_expr(sema, expr->wait_expr.handle_expr);
            if (handle_type && handle_type->kind == TYPE_TASK) {
                expr->resolved_type = handle_type->task.result_type;
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_INT);
            }
            return expr->resolved_type;
        }
        case NODE_TRY_EXPR: {
            MixType *inner = resolve_expr(sema, expr->try_expr.expr);
            // Unwrap: result -> ok_type, optional -> inner
            if (inner && inner->kind == TYPE_RESULT) {
                expr->resolved_type = inner->result.ok_type;
            } else if (inner && inner->kind == TYPE_OPTIONAL) {
                expr->resolved_type = inner->optional.inner;
            } else {
                expr->resolved_type = inner;
            }
            return expr->resolved_type;
        }
        default:
            expr->resolved_type = make_type(sema->arena, TYPE_VOID);
            return expr->resolved_type;
    }
}

static void analyze_stmt(Sema *sema, AstNode *stmt);

static void analyze_block(Sema *sema, AstNode *block) {
    if (!block || block->kind != NODE_BLOCK) return;
    symtab_push_scope(&sema->symtab);
    for (int i = 0; i < block->block.stmt_count; i++) {
        analyze_stmt(sema, block->block.stmts[i]);
    }
    symtab_pop_scope(&sema->symtab);
}

static void analyze_stmt(Sema *sema, AstNode *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case NODE_VAR_DECL: {
            MixType *type;
            if (stmt->var_decl.type_ann) {
                type = resolve_type_node(sema, stmt->var_decl.type_ann);
            } else if (stmt->var_decl.init_expr) {
                type = resolve_expr(sema, stmt->var_decl.init_expr);
            } else {
                type = make_type(sema->arena, TYPE_VOID);
            }
            stmt->resolved_type = type;
            // When there's a type annotation, still resolve the init expression
            // so it gets type-checked and its resolved_type is set.
            if (stmt->var_decl.type_ann && stmt->var_decl.init_expr) {
                MixType *init_type = resolve_expr(sema, stmt->var_decl.init_expr);
                if (!types_compatible(type, init_type)) {
                    mix_error(stmt->loc,
                              "cannot assign %s to variable '%s' of type %s",
                              type_name(sema->arena, init_type),
                              stmt->var_decl.name,
                              type_name(sema->arena, type));
                }
            }
            symtab_insert(&sema->symtab, stmt->var_decl.name, type, stmt->var_decl.is_mutable);
            break;
        }
        case NODE_ASSIGN: {
            MixType *val_type = resolve_expr(sema, stmt->assign.value);
            Symbol *var_sym = symtab_lookup(&sema->symtab, stmt->assign.name);
            if (var_sym && var_sym->type && val_type) {
                if (!var_sym->is_mutable) {
                    mix_error(stmt->loc, "cannot assign to immutable variable '%s'",
                              stmt->assign.name);
                }
                if (stmt->assign.op == TOK_EQ && !types_compatible(var_sym->type, val_type)) {
                    mix_error(stmt->loc,
                              "cannot assign %s to '%s' of type %s",
                              type_name(sema->arena, val_type),
                              stmt->assign.name,
                              type_name(sema->arena, var_sym->type));
                }
            }
            break;
        }
        case NODE_IF_STMT:
            resolve_expr(sema, stmt->if_stmt.condition);
            analyze_block(sema, stmt->if_stmt.then_block);
            if (stmt->if_stmt.else_block) {
                if (stmt->if_stmt.else_block->kind == NODE_BLOCK)
                    analyze_block(sema, stmt->if_stmt.else_block);
                else
                    analyze_stmt(sema, stmt->if_stmt.else_block);
            }
            break;
        case NODE_WHILE_STMT:
            resolve_expr(sema, stmt->while_stmt.condition);
            analyze_block(sema, stmt->while_stmt.body);
            break;
        case NODE_FOR_STMT: {
            MixType *iter_type = resolve_expr(sema, stmt->for_stmt.iterable);
            // For range loops, push scope and add loop variable
            symtab_push_scope(&sema->symtab);
            // Infer loop variable type from iterable
            MixType *var_type = make_type(sema->arena, TYPE_INT);
            if (iter_type && iter_type->kind == TYPE_LIST && iter_type->list.elem_type) {
                var_type = iter_type->list.elem_type;
            } else if (iter_type && iter_type->kind == TYPE_MAP) {
                // for key, value in map: var_name = value type
                var_type = iter_type->map.val_type;
            } else if (iter_type && iter_type->kind == TYPE_SET && iter_type->set.elem_type) {
                var_type = iter_type->set.elem_type;
            } else if (iter_type && (type_is_integer(iter_type) || iter_type->kind == TYPE_INT)) {
                var_type = make_type(sema->arena, TYPE_INT);
            }
            symtab_insert(&sema->symtab, stmt->for_stmt.var_name, var_type, true);
            if (stmt->for_stmt.index_name) {
                // For maps: index_name = key type; for lists: index_name = int
                MixType *idx_type = make_type(sema->arena, TYPE_INT);
                if (iter_type && iter_type->kind == TYPE_MAP) {
                    idx_type = iter_type->map.key_type;
                }
                symtab_insert(&sema->symtab, stmt->for_stmt.index_name, idx_type, false);
            }
            // Analyze body (block handles its own scope, but we already pushed one for the var)
            if (stmt->for_stmt.body && stmt->for_stmt.body->kind == NODE_BLOCK) {
                for (int i2 = 0; i2 < stmt->for_stmt.body->block.stmt_count; i2++) {
                    analyze_stmt(sema, stmt->for_stmt.body->block.stmts[i2]);
                }
            }
            symtab_pop_scope(&sema->symtab);
            break;
        }
        case NODE_MATCH_STMT: {
            MixType *subj_type = resolve_expr(sema, stmt->match_stmt.subject);
            bool is_tagged = subj_type && subj_type->kind == TYPE_SHAPE && subj_type->shape.is_tagged_union;

            for (int i2 = 0; i2 < stmt->match_stmt.arm_count; i2++) {
                struct MatchArm *arm_s = &stmt->match_stmt.arms[i2];
                AstNode *body = arm_s->body;

                if (is_tagged && arm_s->pattern && arm_s->pattern->kind == NODE_CALL_EXPR) {
                    // Variant pattern: Circle(r) → add r to scope for the body
                    ShapeVariant *sv = type_find_variant(subj_type, arm_s->pattern->call.name);
                    symtab_push_scope(&sema->symtab);
                    if (sv) {
                        for (int k = 0; k < arm_s->pattern->call.arg_count && k < sv->field_count; k++) {
                            AstNode *binding = arm_s->pattern->call.args[k];
                            if (binding->kind == NODE_IDENT) {
                                symtab_insert(&sema->symtab, binding->ident.name,
                                              sv->fields[k].type, false);
                            }
                        }
                    }
                    if (body) {
                        if (body->kind == NODE_BLOCK) {
                            for (int k = 0; k < body->block.stmt_count; k++)
                                analyze_stmt(sema, body->block.stmts[k]);
                        } else {
                            resolve_expr(sema, body);
                        }
                    }
                    symtab_pop_scope(&sema->symtab);
                } else {
                    if (arm_s->pattern) resolve_expr(sema, arm_s->pattern);
                    if (body) {
                        if (body->kind == NODE_BLOCK) analyze_block(sema, body);
                        else resolve_expr(sema, body);
                    }
                }
            }
            // Set resolved_type from first arm body (for match-as-expression)
            for (int i2 = 0; i2 < stmt->match_stmt.arm_count; i2++) {
                AstNode *body = stmt->match_stmt.arms[i2].body;
                if (body && body->kind != NODE_BLOCK && body->resolved_type) {
                    stmt->resolved_type = body->resolved_type;
                    break;
                }
            }
            break;
        }
        case NODE_DONE_STMT:
            if (stmt->done_stmt.value) resolve_expr(sema, stmt->done_stmt.value);
            break;
        case NODE_EXPR_STMT:
            resolve_expr(sema, stmt->expr_stmt.expr);
            break;
        case NODE_DEFER_STMT:
            analyze_stmt(sema, stmt->defer_stmt.stmt);
            break;
        case NODE_UNSAFE_BLOCK:
            analyze_block(sema, stmt->unsafe_block.body);
            break;
        case NODE_ZONE_STMT:
            analyze_block(sema, stmt->zone_stmt.body);
            break;
        case NODE_DEREF_ASSIGN:
            resolve_expr(sema, stmt->deref_assign.ptr_expr);
            resolve_expr(sema, stmt->deref_assign.value);
            break;
        case NODE_INDEX_ASSIGN:
            resolve_expr(sema, stmt->index_assign.object);
            resolve_expr(sema, stmt->index_assign.index);
            resolve_expr(sema, stmt->index_assign.value);
            break;
        case NODE_FAIL_STMT:
            resolve_expr(sema, stmt->fail_stmt.value);
            break;
        default:
            break;
    }
}

// Check if a function body contains a 'fail' statement or '?' operator (recursive)
static bool body_contains_fail(AstNode *node) {
    if (!node) return false;
    if (node->kind == NODE_FAIL_STMT) return true;
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++) {
            if (body_contains_fail(node->block.stmts[i])) return true;
        }
    }
    if (node->kind == NODE_IF_STMT) {
        if (body_contains_fail(node->if_stmt.then_block)) return true;
        if (body_contains_fail(node->if_stmt.else_block)) return true;
    }
    if (node->kind == NODE_WHILE_STMT) {
        if (body_contains_fail(node->while_stmt.body)) return true;
    }
    if (node->kind == NODE_FOR_STMT) {
        if (body_contains_fail(node->for_stmt.body)) return true;
    }
    if (node->kind == NODE_MATCH_STMT) {
        for (int i = 0; i < node->match_stmt.arm_count; i++) {
            if (node->match_stmt.arms[i].body &&
                body_contains_fail(node->match_stmt.arms[i].body)) return true;
        }
    }
    // Check expressions in statements
    if (node->kind == NODE_VAR_DECL && node->var_decl.init_expr) {
        if (body_contains_fail(node->var_decl.init_expr)) return true;
    }
    if (node->kind == NODE_EXPR_STMT && node->expr_stmt.expr) {
        if (body_contains_fail(node->expr_stmt.expr)) return true;
    }
    if (node->kind == NODE_DONE_STMT && node->done_stmt.value) {
        if (body_contains_fail(node->done_stmt.value)) return true;
    }
    // Check for ? (try) operator in expressions
    if (node->kind == NODE_TRY_EXPR) return true;
    // Recurse into binary/call expressions
    if (node->kind == NODE_BINARY_EXPR) {
        if (body_contains_fail(node->binary.left)) return true;
        if (body_contains_fail(node->binary.right)) return true;
    }
    if (node->kind == NODE_CALL_EXPR) {
        for (int i = 0; i < node->call.arg_count; i++) {
            if (body_contains_fail(node->call.args[i])) return true;
        }
    }
    return false;
}

static void register_fn(Sema *sema, AstNode *fn) {
    // Set generic context if this is a generic function
    char **saved_gp = sema->generic_params;
    int saved_gc = sema->generic_param_count;
    if (fn->fn_decl.type_param_count > 0) {
        sema->generic_params = fn->fn_decl.type_params;
        sema->generic_param_count = fn->fn_decl.type_param_count;
    }

    MixType *func_type = make_type(sema->arena, TYPE_FUNC);
    MixType *ret_type = fn->fn_decl.return_type
        ? resolve_type_node(sema, fn->fn_decl.return_type)
        : make_type(sema->arena, TYPE_VOID);

    // If function has side effects (~), non-void return, and body contains fail,
    // wrap return type in TYPE_RESULT automatically
    if (fn->fn_decl.has_side_effects && ret_type->kind != TYPE_VOID &&
        body_contains_fail(fn->fn_decl.body)) {
        MixType *result_type = make_type(sema->arena, TYPE_RESULT);
        result_type->result.ok_type = ret_type;
        func_type->func.return_type = result_type;
    } else {
        func_type->func.return_type = ret_type;
    }

    // Annotate the return type AST node so the emitter can use it
    if (fn->fn_decl.return_type) {
        fn->fn_decl.return_type->resolved_type = ret_type;
    }

    func_type->func.param_count = fn->fn_decl.param_count;
    if (fn->fn_decl.param_count > 0) {
        func_type->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * fn->fn_decl.param_count);
        for (int i = 0; i < fn->fn_decl.param_count; i++) {
            MixType *ptype = fn->fn_decl.params[i].type
                ? resolve_type_node(sema, fn->fn_decl.params[i].type)
                : make_type(sema->arena, TYPE_INFER);
            func_type->func.param_types[i] = ptype;
            // Annotate param type AST node
            if (fn->fn_decl.params[i].type) {
                fn->fn_decl.params[i].type->resolved_type = ptype;
            }
        }
    }

    symtab_insert(&sema->symtab, fn->fn_decl.name, func_type, false);

    // Restore generic context
    sema->generic_params = saved_gp;
    sema->generic_param_count = saved_gc;
}

static void register_extern_fn(Sema *sema, AstNode *fn) {
    MixType *func_type = make_type(sema->arena, TYPE_FUNC);
    func_type->func.return_type = fn->extern_fn_decl.return_type
        ? resolve_type_node(sema, fn->extern_fn_decl.return_type)
        : make_type(sema->arena, TYPE_VOID);

    func_type->func.param_count = fn->extern_fn_decl.param_count;
    if (fn->extern_fn_decl.param_count > 0) {
        func_type->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * fn->extern_fn_decl.param_count);
        for (int i = 0; i < fn->extern_fn_decl.param_count; i++) {
            func_type->func.param_types[i] = fn->extern_fn_decl.params[i].type
                ? resolve_type_node(sema, fn->extern_fn_decl.params[i].type)
                : make_type(sema->arena, TYPE_INFER);
        }
    }

    symtab_insert(&sema->symtab, fn->extern_fn_decl.name, func_type, false);

    // Store optional C symbol alias
    if (fn->extern_fn_decl.c_name) {
        Symbol *sym = symtab_lookup(&sema->symtab, fn->extern_fn_decl.name);
        if (sym) sym->c_name = arena_strdup(sema->arena, fn->extern_fn_decl.c_name);
    }
}

void sema_analyze(Sema *sema, AstNode *program) {
    if (!program || program->kind != NODE_PROGRAM) return;

    // Register built-in functions
    // print(value) — we'll handle overloading in the emitter
    MixType *print_type = make_type(sema->arena, TYPE_FUNC);
    print_type->func.return_type = make_type(sema->arena, TYPE_VOID);
    print_type->func.param_count = 1;
    print_type->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
    print_type->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
    symtab_insert(&sema->symtab, "print", print_type, false);

    // File I/O built-ins
    {
        // file_open(path: str, mode: str) -> int
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "file_open", ft, false);
    }
    {
        // file_read(handle: int) -> str
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "file_read", ft, false);
    }
    {
        // file_write(handle: int, data: str) -> void
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "file_write", ft, false);
    }
    {
        // file_close(handle: int) -> void
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "file_close", ft, false);
    }
    {
        // file_read_all(path: str) -> str
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "file_read_all", ft, false);
    }
    {
        // file_write_all(path: str, content: str) -> bool
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_BOOL);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "file_write_all", ft, false);
    }

    // OS builtins
    {   // shell(cmd: str) -> int
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "shell", ft, false);
    }
    {   // shell_output(cmd: str) -> str
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "shell_output", ft, false);
    }
    {   // file_exists(path: str) -> bool
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_BOOL);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "file_exists", ft, false);
    }
    {   // list_dir(path: str) -> [str]
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_list_type(sema->arena, make_type(sema->arena, TYPE_STR));
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "list_dir", ft, false);
    }
    {   // env(name: str) -> str
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "env", ft, false);
    }
    {   // exit(code: int) -> void
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "exit", ft, false);
    }
    {   // getcwd() -> str
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 0;
        ft->func.param_types = NULL;
        symtab_insert(&sema->symtab, "getcwd", ft, false);
    }
    {   // mkdir(path: str) -> bool
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_BOOL);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "mkdir", ft, false);
    }
    {   // args() -> [str]
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_list_type(sema->arena, make_type(sema->arena, TYPE_STR));
        ft->func.param_count = 0;
        ft->func.param_types = NULL;
        symtab_insert(&sema->symtab, "args", ft, false);
    }

    // Built-in Project shape for build system
    {
        MixType *proj = make_type(sema->arena, TYPE_SHAPE);
        proj->shape.name = "Project";
        proj->shape.field_count = 8;
        proj->shape.fields = arena_alloc(sema->arena, 8 * sizeof(ShapeFieldInfo));
        proj->shape.is_tagged_union = false;

        MixType *str_type = make_type(sema->arena, TYPE_STR);
        MixType *list_str = make_list_type(sema->arena, make_type(sema->arena, TYPE_STR));
        MixType *bool_type = make_type(sema->arena, TYPE_BOOL);

        // name: str (offset 0)
        proj->shape.fields[0] = (ShapeFieldInfo){"name", str_type, 0, 8};
        // entry: str (offset 8)
        proj->shape.fields[1] = (ShapeFieldInfo){"entry", str_type, 8, 8};
        // output: str (offset 16)
        proj->shape.fields[2] = (ShapeFieldInfo){"output", str_type, 16, 8};
        // libs: [str] (offset 24)
        proj->shape.fields[3] = (ShapeFieldInfo){"libs", list_str, 24, 8};
        // lib_paths: [str] (offset 32)
        proj->shape.fields[4] = (ShapeFieldInfo){"lib_paths", list_str, 32, 8};
        // include_paths: [str] (offset 40)
        proj->shape.fields[5] = (ShapeFieldInfo){"include_paths", list_str, 40, 8};
        // flags: [str] (offset 48)
        proj->shape.fields[6] = (ShapeFieldInfo){"flags", list_str, 48, 8};
        // debug: bool (offset 56)
        proj->shape.fields[7] = (ShapeFieldInfo){"debug", bool_type, 56, 4};

        proj->shape.total_size = 64; // 56 + 4 = 60, aligned to 8 = 64
        proj->shape.alignment = 8;

        symtab_insert(&sema->symtab, "Project", proj, false);

        // Register build() method as Project_build
        MixType *build_fn = make_type(sema->arena, TYPE_FUNC);
        build_fn->func.return_type = make_type(sema->arena, TYPE_VOID);
        build_fn->func.param_count = 1;
        build_fn->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        build_fn->func.param_types[0] = proj;
        symtab_insert(&sema->symtab, "Project_build", build_fn, false);
    }

    // to_string(value) -> str
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
        symtab_insert(&sema->symtab, "to_string", ft, false);
    }

    // to_int(value) -> int  (float->int or str->int)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
        symtab_insert(&sema->symtab, "to_int", ft, false);
    }

    // to_float(value) -> float  (int->float or str->float)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_FLOAT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
        symtab_insert(&sema->symtab, "to_float", ft, false);
    }

    // to_set(list) -> set  (accepts any list type, infers element type)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_set_type(sema->arena, make_type(sema->arena, TYPE_STR));
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_list_type(sema->arena, make_type(sema->arena, TYPE_INFER));
        symtab_insert(&sema->symtab, "to_set", ft, false);
    }

    // ord(s: str) -> int  (Unicode code point of first character)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "ord", ft, false);
    }

    // chr(n: int) -> str  (Unicode code point to character string)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "chr", ft, false);
    }

    // str_reverse(s: str) -> str
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "str_reverse", ft, false);
    }

    // str_count(s: str, sub: str) -> int
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "str_count", ft, false);
    }

    // Memory builtins for C interop
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // alloc(n: int) -> *byte
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "alloc", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // bytes(n: int) -> *byte
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "bytes", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // peek_u32(ptr: *byte) -> uint32
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_UINT32);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = ptr_byte;
        symtab_insert(&sema->symtab, "peek_u32", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // poke_f32(ptr: *byte, offset: int, val: float) ~
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 3;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 3);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        ft->func.param_types[2] = make_type(sema->arena, TYPE_FLOAT);
        symtab_insert(&sema->symtab, "poke_f32", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // pack2(a: *byte, b: *byte, elem_size: int) -> *byte
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        ft->func.param_count = 3;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 3);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = ptr_byte;
        ft->func.param_types[2] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "pack2", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // pack3(a: *byte, b: *byte, c: *byte, elem_size: int) -> *byte
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        ft->func.param_count = 4;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 4);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = ptr_byte;
        ft->func.param_types[2] = ptr_byte;
        ft->func.param_types[3] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "pack3", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // poke_u32(ptr: *byte, offset: int, val: int) ~
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 3;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 3);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        ft->func.param_types[2] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "poke_u32", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // poke_ptr(ptr: *byte, offset: int, val: *byte) ~
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 3;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 3);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        ft->func.param_types[2] = ptr_byte;
        symtab_insert(&sema->symtab, "poke_ptr", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // peek_ptr(ptr: *byte, offset: int) -> *byte
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "peek_ptr", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // list_to_f32(list: [float]) -> *byte
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        MixType *float_list = make_type(sema->arena, TYPE_LIST);
        float_list->list.elem_type = make_type(sema->arena, TYPE_FLOAT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = float_list;
        symtab_insert(&sema->symtab, "list_to_f32", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // free_mem(ptr: *byte) -> void
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = ptr_byte;
        symtab_insert(&sema->symtab, "free_mem", ft, false);
    }

    // Math built-ins (single arg: float -> float)
    {
        const char *math_fns[] = {"sqrt", "abs", "sin", "cos", "tan", "log", "floor", "ceil", "round"};
        for (int mi = 0; mi < 9; mi++) {
            MixType *ft = make_type(sema->arena, TYPE_FUNC);
            ft->func.return_type = make_type(sema->arena, TYPE_FLOAT);
            ft->func.param_count = 1;
            ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
            ft->func.param_types[0] = make_type(sema->arena, TYPE_FLOAT);
            symtab_insert(&sema->symtab, math_fns[mi], ft, false);
        }
    }
    {
        // pow, min, max: (float, float) -> float
        const char *math_fns2[] = {"pow", "min", "max"};
        for (int mi = 0; mi < 3; mi++) {
            MixType *ft = make_type(sema->arena, TYPE_FUNC);
            ft->func.return_type = make_type(sema->arena, TYPE_FLOAT);
            ft->func.param_count = 2;
            ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
            ft->func.param_types[0] = make_type(sema->arena, TYPE_FLOAT);
            ft->func.param_types[1] = make_type(sema->arena, TYPE_FLOAT);
            symtab_insert(&sema->symtab, math_fns2[mi], ft, false);
        }
    }

    // First pass (a): register all shapes and type aliases first
    // (so extern blocks can reference shape types regardless of source order)
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_SHAPE_DECL) {
            // Build shape type
            MixType *shape_type = make_type(sema->arena, TYPE_SHAPE);
            shape_type->shape.name = decl->shape_decl.name;
            shape_type->shape.field_count = decl->shape_decl.field_count;
            shape_type->shape.fields = arena_alloc(sema->arena,
                sizeof(ShapeFieldInfo) * decl->shape_decl.field_count);

            int offset = 0;
            bool is_union = decl->shape_decl.is_union;
            int max_field_size = 0;
            for (int j = 0; j < decl->shape_decl.field_count; j++) {
                ShapeField *sf = &decl->shape_decl.fields[j];
                MixType *ftype = sf->type ? resolve_type_node(sema, sf->type)
                                          : make_type(sema->arena, TYPE_INT);
                // Annotate AST node
                if (sf->type) sf->type->resolved_type = ftype;
                int fsize = type_size(ftype);
                int falign = type_alignment(ftype);

                if (is_union) {
                    // Union: all fields at offset 0
                    shape_type->shape.fields[j].name = sf->name;
                    shape_type->shape.fields[j].type = ftype;
                    shape_type->shape.fields[j].offset = 0;
                    shape_type->shape.fields[j].size = fsize;
                    sf->offset = 0;
                    sf->size = fsize;
                    if (fsize > max_field_size) max_field_size = fsize;
                } else {
                    // Align offset
                    offset = (offset + falign - 1) & ~(falign - 1);

                    shape_type->shape.fields[j].name = sf->name;
                    shape_type->shape.fields[j].type = ftype;
                    shape_type->shape.fields[j].offset = offset;
                    shape_type->shape.fields[j].size = fsize;

                    // Store computed values back in AST for emitter
                    sf->offset = offset;
                    sf->size = fsize;

                    offset += fsize;
                }
            }
            // Final alignment
            if (is_union) {
                shape_type->shape.total_size = (max_field_size + 7) & ~7;
                shape_type->shape.is_union = true;
            } else {
                shape_type->shape.total_size = (offset + 7) & ~7;
            }
            shape_type->shape.alignment = 8;

            // Handle tagged union variants
            if (decl->shape_decl.variant_count > 0) {
                shape_type->shape.is_tagged_union = true;
                shape_type->shape.variant_count = decl->shape_decl.variant_count;
                shape_type->shape.variants = arena_alloc(sema->arena,
                    sizeof(ShapeVariant) * decl->shape_decl.variant_count);

                int max_data_size = 0;
                for (int j = 0; j < decl->shape_decl.variant_count; j++) {
                    ShapeVariantDecl *vd = &decl->shape_decl.variants[j];
                    ShapeVariant *sv = &shape_type->shape.variants[j];
                    sv->name = vd->name;
                    sv->tag = j;
                    sv->field_count = vd->field_count;
                    sv->fields = arena_alloc(sema->arena,
                        sizeof(ShapeFieldInfo) * vd->field_count);

                    int voffset = 0;
                    for (int k = 0; k < vd->field_count; k++) {
                        MixType *ft = vd->fields[k].type
                            ? resolve_type_node(sema, vd->fields[k].type)
                            : make_type(sema->arena, TYPE_INT);
                        if (vd->fields[k].type) vd->fields[k].type->resolved_type = ft;
                        int fs = type_size(ft);
                        int fa = type_alignment(ft);
                        voffset = (voffset + fa - 1) & ~(fa - 1);
                        sv->fields[k].name = vd->fields[k].name;
                        sv->fields[k].type = ft;
                        sv->fields[k].offset = voffset;
                        sv->fields[k].size = fs;
                        voffset += fs;
                    }
                    sv->data_size = (voffset + 7) & ~7;
                    if (sv->data_size > max_data_size) max_data_size = sv->data_size;
                }
                // Total size: 8 (tag) + max variant data size
                shape_type->shape.total_size = 8 + max_data_size;

                // Register each variant as a constructor function
                for (int j = 0; j < decl->shape_decl.variant_count; j++) {
                    ShapeVariantDecl *vd = &decl->shape_decl.variants[j];
                    MixType *ctor_type = make_type(sema->arena, TYPE_FUNC);
                    ctor_type->func.return_type = shape_type;
                    ctor_type->func.param_count = vd->field_count;
                    ctor_type->func.param_types = arena_alloc(sema->arena,
                        sizeof(MixType*) * vd->field_count);
                    for (int k = 0; k < vd->field_count; k++) {
                        ctor_type->func.param_types[k] = vd->fields[k].type
                            ? vd->fields[k].type->resolved_type
                            : make_type(sema->arena, TYPE_INT);
                    }
                    symtab_insert(&sema->symtab, vd->name, ctor_type, false);
                }
            }

            symtab_insert(&sema->symtab, decl->shape_decl.name, shape_type, false);

            // Register methods as ShapeName_methodName
            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                AstNode *method = decl->shape_decl.methods[j];
                // Build mangled name: ShapeName_methodName
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                         decl->shape_decl.name, method->fn_decl.name);

                // Build function type: first param is the shape (self), then user params
                MixType *mtype = make_type(sema->arena, TYPE_FUNC);
                MixType *ret = method->fn_decl.return_type
                    ? resolve_type_node(sema, method->fn_decl.return_type) : make_type(sema->arena, TYPE_VOID);
                if (method->fn_decl.return_type) method->fn_decl.return_type->resolved_type = ret;
                mtype->func.return_type = ret;
                mtype->func.param_count = method->fn_decl.param_count + 1; // +1 for self
                mtype->func.param_types = arena_alloc(sema->arena,
                    sizeof(MixType*) * mtype->func.param_count);
                mtype->func.param_types[0] = shape_type; // self
                for (int k = 0; k < method->fn_decl.param_count; k++) {
                    MixType *pt = method->fn_decl.params[k].type
                        ? resolve_type_node(sema, method->fn_decl.params[k].type)
                        : make_type(sema->arena, TYPE_INFER);
                    if (method->fn_decl.params[k].type)
                        method->fn_decl.params[k].type->resolved_type = pt;
                    mtype->func.param_types[k + 1] = pt;
                }
                symtab_insert(&sema->symtab, mangled, mtype, false);
            }
        } else if (decl->kind == NODE_TYPE_ALIAS) {
            MixType *target = resolve_type_node(sema, decl->type_alias.target_type);
            if (decl->type_alias.target_type) decl->type_alias.target_type->resolved_type = target;
            symtab_insert(&sema->symtab, decl->type_alias.name, target, false);
        } else if (decl->kind == NODE_COND_DECL) {
            // Evaluate compile-time condition
            bool active = false;
            const char *cn = decl->cond_decl.condition_name;
            const char *cv = decl->cond_decl.condition_value;
            if (strcmp(cn, "os") == 0) {
                #ifdef __APPLE__
                active = !cv || strcmp(cv, "macos") == 0;
                #elif defined(__linux__)
                active = !cv || strcmp(cv, "linux") == 0;
                #elif defined(__sgi)
                active = !cv || strcmp(cv, "irix") == 0;
                #endif
            } else if (strcmp(cn, "arch") == 0) {
                #ifdef __aarch64__
                active = !cv || strcmp(cv, "aarch64") == 0 || strcmp(cv, "arm64") == 0;
                #elif defined(__x86_64__)
                active = !cv || strcmp(cv, "x86_64") == 0;
                #elif defined(__mips__) && defined(__mips64)
                active = !cv || strcmp(cv, "mips64") == 0 || strcmp(cv, "mips") == 0;
                #elif defined(__mips__)
                active = !cv || strcmp(cv, "mips") == 0;
                #endif
            } else if (strcmp(cn, "debug") == 0) {
                active = sema->debug_mode;
            } else if (strcmp(cn, "release") == 0) {
                active = !sema->debug_mode;
            }
            decl->cond_decl.active = active;
        }
        // Skip non-shape declarations in this pass
    }

    // First pass (b): register functions, externs, constants
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_CONST_DECL) {
            MixType *ctype = resolve_expr(sema, decl->const_decl.value);
            symtab_insert(&sema->symtab, decl->const_decl.name, ctype, false);
        } else if (decl->kind == NODE_FN_DECL) {
            register_fn(sema, decl);
        } else if (decl->kind == NODE_EXTERN_BLOCK) {
            for (int j = 0; j < decl->extern_block.decl_count; j++) {
                register_extern_fn(sema, decl->extern_block.decls[j]);
            }
        } else if (decl->kind == NODE_COND_DECL && decl->cond_decl.active) {
            for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                AstNode *cd = decl->cond_decl.decls[j];
                if (cd->kind == NODE_FN_DECL) {
                    register_fn(sema, cd);
                } else if (cd->kind == NODE_CONST_DECL) {
                    MixType *ctype = resolve_expr(sema, cd->const_decl.value);
                    symtab_insert(&sema->symtab, cd->const_decl.name, ctype, false);
                } else if (cd->kind == NODE_EXTERN_BLOCK) {
                    for (int k = 0; k < cd->extern_block.decl_count; k++) {
                        register_extern_fn(sema, cd->extern_block.decls[k]);
                    }
                }
            }
        }
    }

    // Second pass: analyze function bodies and method bodies
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            symtab_push_scope(&sema->symtab);

            // Set generic context
            if (decl->fn_decl.type_param_count > 0) {
                sema->generic_params = decl->fn_decl.type_params;
                sema->generic_param_count = decl->fn_decl.type_param_count;
            }

            // Insert parameters into scope
            for (int j = 0; j < decl->fn_decl.param_count; j++) {
                Param *param = &decl->fn_decl.params[j];
                MixType *ptype = param->type
                    ? resolve_type_node(sema, param->type)
                    : make_type(sema->arena, TYPE_INFER);
                symtab_insert(&sema->symtab, param->name, ptype, param->is_mutable);
            }

            // Analyze body
            if (decl->fn_decl.body) {
                AstNode *body = decl->fn_decl.body;
                for (int j = 0; j < body->block.stmt_count; j++) {
                    analyze_stmt(sema, body->block.stmts[j]);
                }
            }

            // Clear generic context
            sema->generic_params = NULL;
            sema->generic_param_count = 0;

            symtab_pop_scope(&sema->symtab);
        } else if (decl->kind == NODE_SHAPE_DECL) {
            // Analyze method bodies
            Symbol *shape_sym = symtab_lookup(&sema->symtab, decl->shape_decl.name);
            MixType *shape_type = shape_sym ? shape_sym->type : NULL;

            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                AstNode *method = decl->shape_decl.methods[j];
                symtab_push_scope(&sema->symtab);

                // Insert 'self' as an implicit parameter
                if (shape_type) {
                    symtab_insert(&sema->symtab, "self", shape_type, false);
                    // Insert shape fields as variables accessible by name
                    for (int k = 0; k < shape_type->shape.field_count; k++) {
                        symtab_insert(&sema->symtab, shape_type->shape.fields[k].name,
                                      shape_type->shape.fields[k].type, false);
                    }
                }

                // Insert explicit method parameters
                for (int k = 0; k < method->fn_decl.param_count; k++) {
                    Param *param = &method->fn_decl.params[k];
                    MixType *ptype = param->type
                        ? resolve_type_node(sema, param->type)
                        : make_type(sema->arena, TYPE_INFER);
                    symtab_insert(&sema->symtab, param->name, ptype, param->is_mutable);
                }

                if (method->fn_decl.body) {
                    AstNode *body = method->fn_decl.body;
                    for (int k = 0; k < body->block.stmt_count; k++) {
                        analyze_stmt(sema, body->block.stmts[k]);
                    }
                }

                symtab_pop_scope(&sema->symtab);
            }
        } else if (decl->kind == NODE_COND_DECL && decl->cond_decl.active) {
            // Analyze function bodies in active conditional blocks
            for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                AstNode *cd = decl->cond_decl.decls[j];
                if (cd->kind == NODE_FN_DECL) {
                    symtab_push_scope(&sema->symtab);
                    for (int k = 0; k < cd->fn_decl.param_count; k++) {
                        Param *param = &cd->fn_decl.params[k];
                        MixType *ptype = param->type
                            ? resolve_type_node(sema, param->type)
                            : make_type(sema->arena, TYPE_INFER);
                        symtab_insert(&sema->symtab, param->name, ptype, param->is_mutable);
                    }
                    if (cd->fn_decl.body) {
                        AstNode *body = cd->fn_decl.body;
                        for (int k = 0; k < body->block.stmt_count; k++) {
                            analyze_stmt(sema, body->block.stmts[k]);
                        }
                    }
                    symtab_pop_scope(&sema->symtab);
                }
            }
        }
    }
}
