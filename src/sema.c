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

// Insert a function/method parameter into the current scope. Plain shape
// params are value parameters, so function bodies work against stack-backed
// copies (`is_stack_slot=true`). Mutable shape params are explicit by-ref
// aliases to caller storage, so they stay as direct pointer values.
static void insert_param(SymTab *st, Param *param, MixType *ptype) {
    symtab_insert(st, param->name, ptype, param->is_mutable);
    if (!param->is_mutable && ptype && ptype->kind == TYPE_SHAPE) {
        Symbol *psym = symtab_lookup_current(st, param->name);
        if (psym) psym->is_stack_slot = true;
    }
}

static MixType *make_ptr_type(Arena *a, MixType *base) {
    MixType *t = make_type(a, TYPE_PTR);
    t->ptr.base = base;
    return t;
}

static MixType *make_ref_type(Arena *a, MixType *base, bool is_mutable) {
    MixType *t = make_type(a, TYPE_REF);
    t->ref.base = base;
    t->ref.is_mutable = is_mutable;
    return t;
}

static MixType *make_box_type(Arena *a, MixType *inner) {
    MixType *t = make_type(a, TYPE_BOX);
    t->box.inner = inner;
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
        case TYPE_REF: {
            const char *inner = type_name(a, t->ref.base);
            char buf[128];
            snprintf(buf, sizeof(buf), "ref%s %s",
                     t->ref.is_mutable ? "!" : "", inner);
            return arena_strdup(a, buf);
        }
        case TYPE_BOX: {
            const char *inner = type_name(a, t->box.inner);
            char buf[128];
            snprintf(buf, sizeof(buf), "Box[%s]", inner);
            return arena_strdup(a, buf);
        }
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
        case TYPE_ZONE:
            return "Zone";
        case TYPE_SHAPE:
            return t->shape.name ? t->shape.name : "shape";
        case TYPE_FUNC:
            return "func";
        default:
            return type_kind_name(t->kind);
    }
}

static const char *binary_op_name(TokenKind op) {
    switch (op) {
        case TOK_PLUS:    return "+";
        case TOK_MINUS:   return "-";
        case TOK_STAR:    return "*";
        case TOK_SLASH:   return "/";
        case TOK_PERCENT: return "%";
        case TOK_EQEQ:    return "==";
        case TOK_NEQ:     return "!=";
        case TOK_LT:      return "<";
        case TOK_GT:      return ">";
        case TOK_LTE:     return "<=";
        case TOK_GTE:     return ">=";
        case TOK_AND:     return "and";
        case TOK_OR:      return "or";
        case TOK_PIPE:    return "|";
        default:          return token_kind_name(op);
    }
}

static bool type_is_unresolved_for_binary(MixType *t) {
    return !t || t->kind == TYPE_INFER || t->kind == TYPE_GENERIC ||
           t->kind == TYPE_NAMED || t->kind == TYPE_VOID;
}

/* Check if two types are compatible (same kind + matching inner types) */
// Does `t` satisfy a `has` constraint? For operator constraints (+, -, *, /,
// %, ==, !=, <, >, <=, >=) we know the built-in type rules. For named
// method constraints (e.g. `area`, `len`) we look for a matching shape
// method via `Shape_method` in the symbol table.
static bool type_satisfies_constraint(MixType *t, const char *cons, SymTab *st) {
    if (!t || !cons) return true;
    if (t->kind == TYPE_GENERIC || t->kind == TYPE_INFER) return true;

    bool is_op = (cons[0] == '+' || cons[0] == '-' || cons[0] == '*' ||
                  cons[0] == '/' || cons[0] == '%' || cons[0] == '<' ||
                  cons[0] == '>' || cons[0] == '=' || cons[0] == '!');
    if (is_op) {
        bool eq_like = strcmp(cons, "==") == 0 || strcmp(cons, "!=") == 0;
        bool ord_like = strcmp(cons, "<") == 0 || strcmp(cons, ">") == 0 ||
                         strcmp(cons, "<=") == 0 || strcmp(cons, ">=") == 0;
        bool plus = strcmp(cons, "+") == 0;
        // Numeric ops on numbers
        if (type_is_numeric(t)) return true;
        // == / != / < / > / <= / >= / + on strings
        if (t->kind == TYPE_STR && (eq_like || ord_like || plus)) return true;
        // == / != on bools and pointers
        if (eq_like && (t->kind == TYPE_BOOL || t->kind == TYPE_PTR)) return true;
        // User shape: look for the matching op_* method
        if (t->kind == TYPE_SHAPE) {
            const char *m = NULL;
            if (plus) m = "op_add";
            else if (strcmp(cons, "-") == 0) m = "op_sub";
            else if (strcmp(cons, "*") == 0) m = "op_mul";
            else if (strcmp(cons, "/") == 0) m = "op_div";
            else if (strcmp(cons, "%") == 0) m = "op_mod";
            else if (strcmp(cons, "==") == 0) m = "op_eq";
            else if (strcmp(cons, "!=") == 0) m = "op_neq";
            else if (strcmp(cons, "<") == 0)  m = "op_lt";
            else if (strcmp(cons, ">") == 0)  m = "op_gt";
            else if (strcmp(cons, "<=") == 0) m = "op_lte";
            else if (strcmp(cons, ">=") == 0) m = "op_gte";
            if (m) {
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s", t->shape.name, m);
                Symbol *sym = symtab_lookup(st, mangled);
                if (sym && sym->type && sym->type->kind == TYPE_FUNC) return true;
            }
        }
        return false;
    }

    // Named method constraint (e.g. `area`, `len`). For shapes, check methods.
    if (t->kind == TYPE_SHAPE) {
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s_%s", t->shape.name, cons);
        Symbol *sym = symtab_lookup(st, mangled);
        if (sym && sym->type && sym->type->kind == TYPE_FUNC) return true;
    }
    // Built-in `len` on str/list/map/set
    if (strcmp(cons, "len") == 0) {
        if (t->kind == TYPE_STR || t->kind == TYPE_LIST ||
            t->kind == TYPE_MAP || t->kind == TYPE_SET) return true;
    }
    return false;
}

// Pretty-print a constraint name for diagnostics (operators stay as-is,
// named methods get a `()` suffix to read clearly).
static const char *constraint_label(const char *c) {
    if (!c) return "?";
    if (c[0] >= 'a' && c[0] <= 'z') {
        // method name — keep raw; caller wraps in quotes
        return c;
    }
    return c;
}

// Find the concrete type that should bind to `T` (TYPE_GENERIC) by scanning
// param/arg pairs. Looks for both bare `T` and `[T]` patterns.
static MixType *infer_generic_arg(MixType *func_type, AstNode **args, int arg_count) {
    if (!func_type) return NULL;
    int n = func_type->func.param_count < arg_count
        ? func_type->func.param_count : arg_count;
    for (int i = 0; i < n; i++) {
        MixType *pt = func_type->func.param_types[i];
        MixType *at = args[i]->resolved_type;
        if (!pt || !at) continue;
        if (pt->kind == TYPE_GENERIC) return at;
        if (pt->kind == TYPE_LIST && pt->list.elem_type &&
            pt->list.elem_type->kind == TYPE_GENERIC &&
            at->kind == TYPE_LIST && at->list.elem_type) {
            return at->list.elem_type;
        }
    }
    return NULL;
}

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

    if (expected->kind == TYPE_REF || actual->kind == TYPE_REF) {
        if (expected->kind != TYPE_REF || actual->kind != TYPE_REF) return false;
        if (expected->ref.is_mutable && !actual->ref.is_mutable) return false;
        return types_compatible(expected->ref.base, actual->ref.base);
    }

    if (expected->kind == TYPE_BOX || actual->kind == TYPE_BOX) {
        if (expected->kind != TYPE_BOX || actual->kind != TYPE_BOX) return false;
        return types_compatible(expected->box.inner, actual->box.inner);
    }

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

// --- Generic shape monomorphization ---

static GenericShapeTemplate *find_template(Sema *sema, const char *name) {
    for (GenericShapeTemplate *t = sema->templates; t; t = t->next)
        if (strcmp(t->name, name) == 0) return t;
    return NULL;
}

static void register_template(Sema *sema, AstNode *decl) {
    GenericShapeTemplate *t = arena_alloc(sema->arena, sizeof(*t));
    t->name = decl->shape_decl.name;
    t->decl = decl;
    t->next = sema->templates;
    sema->templates = t;
}

static MixType *find_instance(Sema *sema, const char *mangled) {
    for (GenericShapeInstance *i = sema->instances; i; i = i->next)
        if (strcmp(i->mangled, mangled) == 0) return i->type;
    return NULL;
}

static void cache_instance(Sema *sema, const char *mangled, MixType *type) {
    GenericShapeInstance *i = arena_alloc(sema->arena, sizeof(*i));
    i->mangled = arena_strdup(sema->arena, mangled);
    i->type = type;
    i->next = sema->instances;
    sema->instances = i;
}

// Build a short stable name fragment for a type. Used for mangling.
static void type_mangle(MixType *t, char *out, int out_size) {
    if (!t) { snprintf(out, out_size, "void"); return; }
    switch (t->kind) {
        case TYPE_INT: snprintf(out, out_size, "int"); return;
        case TYPE_FLOAT: snprintf(out, out_size, "float"); return;
        case TYPE_BOOL: snprintf(out, out_size, "bool"); return;
        case TYPE_BYTE: snprintf(out, out_size, "byte"); return;
        case TYPE_STR: snprintf(out, out_size, "str"); return;
        case TYPE_PTR: snprintf(out, out_size, "ptr"); return;
        case TYPE_REF: {
            char inner[64];
            type_mangle(t->ref.base, inner, sizeof(inner));
            snprintf(out, out_size, "ref%s_%s",
                     t->ref.is_mutable ? "m" : "", inner);
            return;
        }
        case TYPE_BOX: {
            char inner[64];
            type_mangle(t->box.inner, inner, sizeof(inner));
            snprintf(out, out_size, "box_%s", inner);
            return;
        }
        case TYPE_INT32: snprintf(out, out_size, "int32"); return;
        case TYPE_UINT32: snprintf(out, out_size, "uint32"); return;
        case TYPE_FLOAT32: snprintf(out, out_size, "float32"); return;
        case TYPE_SHAPE: snprintf(out, out_size, "%s", t->shape.name); return;
        default: snprintf(out, out_size, "T%d", (int)t->kind); return;
    }
}

// Forward decls
static MixType *resolve_type_node(Sema *sema, AstNode *type_node);
static void instantiate_methods_into(Sema *sema, AstNode *cloned_decl,
                                     MixType *shape_type);

// Look up or build the instantiation `Template[args...]`. Returns the
// instantiated TYPE_SHAPE. Returns NULL if not a generic template.
static MixType *instantiate_generic_shape(Sema *sema, const char *template_name,
                                          AstNode **type_args, int type_arg_count) {
    GenericShapeTemplate *tpl = find_template(sema, template_name);
    if (!tpl) return NULL;
    AstNode *decl = tpl->decl;

    if (decl->shape_decl.type_param_count != type_arg_count) {
        mix_error(decl->loc, "shape '%s' takes %d type arg(s), got %d",
                  template_name, decl->shape_decl.type_param_count, type_arg_count);
        mix_help(decl->loc, "add or remove type arguments in brackets: `%s[%s]`",
                 template_name, decl->shape_decl.type_param_count > 0 ? "T1, T2, ..." : "");
        return NULL;
    }

    // Resolve the concrete types and build a binding list for the cloner.
    // We also build the mangled name as we go.
    char mangled[256];
    int off = snprintf(mangled, sizeof(mangled), "%s$", template_name);
    TypeBinding *bindings = arena_alloc(sema->arena, sizeof(TypeBinding) * type_arg_count);
    for (int i = 0; i < type_arg_count; i++) {
        MixType *concrete = resolve_type_node(sema, type_args[i]);
        char frag[64];
        type_mangle(concrete, frag, sizeof(frag));
        if (i > 0 && off + 1 < (int)sizeof(mangled)) mangled[off++] = '_';
        off += snprintf(mangled + off, sizeof(mangled) - off, "%s", frag);
        bindings[i].name = decl->shape_decl.type_params[i];
        bindings[i].type_node = type_args[i];
    }
    if (off < (int)sizeof(mangled)) mangled[off] = '\0';

    MixType *cached = find_instance(sema, mangled);
    if (cached) return cached;

    // Clone the decl with substitution.
    AstNode *clone = arena_alloc(sema->arena, sizeof(AstNode));
    *clone = *decl;
    clone->shape_decl.name = arena_strdup(sema->arena, mangled);
    clone->shape_decl.type_params = NULL;
    clone->shape_decl.type_param_count = 0;
    // Clone fields with type substitution.
    if (decl->shape_decl.field_count > 0) {
        clone->shape_decl.fields = arena_alloc(sema->arena,
            sizeof(ShapeField) * decl->shape_decl.field_count);
        for (int i = 0; i < decl->shape_decl.field_count; i++) {
            ShapeField *src = &decl->shape_decl.fields[i];
            clone->shape_decl.fields[i].name = src->name;
            clone->shape_decl.fields[i].type = ast_clone(src->type, sema->arena,
                                                         bindings, type_arg_count);
            clone->shape_decl.fields[i].offset = 0;
            clone->shape_decl.fields[i].size = 0;
        }
    }
    // Clone methods (including bodies) with substitution.
    if (decl->shape_decl.method_count > 0) {
        clone->shape_decl.methods = arena_alloc(sema->arena,
            sizeof(AstNode*) * decl->shape_decl.method_count);
        for (int i = 0; i < decl->shape_decl.method_count; i++) {
            clone->shape_decl.methods[i] = ast_clone(decl->shape_decl.methods[i],
                sema->arena, bindings, type_arg_count);
        }
    }

    // Build a TYPE_SHAPE for the instantiation. Field offsets/sizes
    // match the existing shape-pass logic but with concrete types.
    MixType *shape_type = make_type(sema->arena, TYPE_SHAPE);
    shape_type->shape.name = clone->shape_decl.name;
    shape_type->shape.field_count = clone->shape_decl.field_count;
    shape_type->shape.fields = arena_alloc(sema->arena,
        sizeof(ShapeFieldInfo) * clone->shape_decl.field_count);
    int offset = 0;
    int max_align = 1;
    for (int j = 0; j < clone->shape_decl.field_count; j++) {
        ShapeField *sf = &clone->shape_decl.fields[j];
        MixType *ftype = sf->type ? resolve_type_node(sema, sf->type)
                                  : make_type(sema->arena, TYPE_INT);
        if (ftype && ftype->kind == TYPE_REF) {
            mix_error(sf->type ? sf->type->loc : clone->loc,
                      "shape field '%s.%s' cannot have ref type",
                      clone->shape_decl.name, sf->name);
            mix_help(sf->type ? sf->type->loc : clone->loc,
                     "store the referenced value directly in the shape instead of using ref");
        }
        if (sf->type) sf->type->resolved_type = ftype;
        int fsize = type_size(ftype);
        int falign = type_alignment(ftype);
        if (falign > max_align) max_align = falign;
        offset = (offset + falign - 1) & ~(falign - 1);
        shape_type->shape.fields[j].name = sf->name;
        shape_type->shape.fields[j].type = ftype;
        shape_type->shape.fields[j].offset = offset;
        shape_type->shape.fields[j].size = fsize;
        sf->offset = offset;
        sf->size = fsize;
        offset += fsize;
    }
    shape_type->shape.total_size = (offset + max_align - 1) & ~(max_align - 1);
    shape_type->shape.alignment = max_align;
    clone->resolved_type = shape_type;

    // Register the instantiated shape into the GLOBAL scope, not whatever
    // function-local scope happens to be active when the use site is being
    // analyzed. Otherwise the entry vanishes when the local scope pops.
    Scope *saved_scope = sema->symtab.current;
    Scope *root = saved_scope;
    while (root->parent) root = root->parent;
    sema->symtab.current = root;
    symtab_insert(&sema->symtab, mangled, shape_type, false);
    cache_instance(sema, mangled, shape_type);
    instantiate_methods_into(sema, clone, shape_type);
    sema->symtab.current = saved_scope;

    // Append the cloned decl to the program-level instantiated list so
    // codegen sees it alongside hand-written shapes.
    if (sema->instantiated_decl_count >= sema->instantiated_decl_cap) {
        int new_cap = sema->instantiated_decl_cap ? sema->instantiated_decl_cap * 2 : 8;
        AstNode **na = arena_alloc(sema->arena, sizeof(AstNode*) * new_cap);
        if (sema->instantiated_decls)
            memcpy(na, sema->instantiated_decls,
                   sizeof(AstNode*) * sema->instantiated_decl_count);
        sema->instantiated_decls = na;
        sema->instantiated_decl_cap = new_cap;
    }
    sema->instantiated_decls[sema->instantiated_decl_count++] = clone;

    return shape_type;
}

static MixType *resolve_type_node(Sema *sema, AstNode *type_node);

static void instantiate_methods_into(Sema *sema, AstNode *cloned_decl,
                                     MixType *shape_type) {
    for (int j = 0; j < cloned_decl->shape_decl.method_count; j++) {
        AstNode *method = cloned_decl->shape_decl.methods[j];
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s_%s",
                 cloned_decl->shape_decl.name, method->fn_decl.name);

        MixType *mtype = make_type(sema->arena, TYPE_FUNC);
        MixType *ret = method->fn_decl.return_type
            ? resolve_type_node(sema, method->fn_decl.return_type)
            : make_type(sema->arena, TYPE_VOID);
        if (method->fn_decl.return_type)
            method->fn_decl.return_type->resolved_type = ret;
        mtype->func.return_type = ret;
        mtype->func.param_count = method->fn_decl.param_count + 1;
        mtype->func.param_types = arena_alloc(sema->arena,
            sizeof(MixType*) * mtype->func.param_count);
        mtype->func.param_mutable = arena_alloc(sema->arena,
            sizeof(bool) * mtype->func.param_count);
        memset(mtype->func.param_mutable, 0,
               sizeof(bool) * mtype->func.param_count);
        mtype->func.param_mutable[0] = method->fn_decl.has_mutation;
        mtype->func.param_types[0] = shape_type;
        for (int k = 0; k < method->fn_decl.param_count; k++) {
            MixType *pt = method->fn_decl.params[k].type
                ? resolve_type_node(sema, method->fn_decl.params[k].type)
                : make_type(sema->arena, TYPE_INFER);
            if (method->fn_decl.params[k].default_value &&
                method->fn_decl.params[k].is_mutable) {
                mix_error(method->loc,
                    "mutable parameter '%s' of '%s.%s' cannot have a default value",
                    method->fn_decl.params[k].name,
                    cloned_decl->shape_decl.name, method->fn_decl.name);
                mix_help(method->loc,
                         "remove the default value or the `!` from parameter '%s'",
                         method->fn_decl.params[k].name);
            }
            if (method->fn_decl.params[k].type)
                method->fn_decl.params[k].type->resolved_type = pt;
            mtype->func.param_types[k + 1] = pt;
            mtype->func.param_mutable[k + 1] =
                method->fn_decl.params[k].is_mutable;
        }
        symtab_insert(&sema->symtab, mangled, mtype, false);
        Symbol *msym = symtab_lookup(&sema->symtab, mangled);
        if (msym) msym->has_mutation = method->fn_decl.has_mutation;
    }
}

static MixType *resolve_type_node(Sema *sema, AstNode *type_node) {
    if (!type_node) return make_type(sema->arena, TYPE_VOID);

    if (type_node->kind == NODE_TYPE_REF) {
        MixType *base = resolve_type_node(sema, type_node->type_ref.base_type);
        return make_ref_type(sema->arena, base, type_node->type_ref.is_mutable);
    }

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
                if (strcmp(type_node->type_name.name, "Box") == 0) {
                    if (type_node->type_name.type_arg_count != 1) {
                    mix_error(type_node->loc, "Box expects exactly 1 type argument");
                    mix_help(type_node->loc, "write `Box[YourType]` to create a box");
                    return make_type(sema->arena, TYPE_VOID);
                    }
                    MixType *inner = resolve_type_node(
                        sema, type_node->type_name.type_args[0]);
                    return make_box_type(sema->arena, inner);
                }
                // Generic instantiation: `Stack[int]`
                if (type_node->type_name.type_arg_count > 0) {
                    MixType *inst = instantiate_generic_shape(sema,
                        type_node->type_name.name,
                        type_node->type_name.type_args,
                        type_node->type_name.type_arg_count);
                    if (inst) return inst;
                    // Fall through to opaque-named on error
                }
                // Look up in symbol table — could be a shape type or
                // a type-alias target (`type Score = int`).
                Symbol *sym = symtab_lookup(&sema->symtab, type_node->type_name.name);
                if (sym && sym->type) {
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
                mix_error(type_node->loc, "unknown type '%s'",
                          type_node->type_name.name ? type_node->type_name.name : "");
                mix_help(type_node->loc, "check the type name or use a built-in type (`int`, `float`, `bool`, `str`)");
                return make_type(sema->arena, TYPE_INT);
        }
    }

    return make_type(sema->arena, TYPE_VOID);
}

static MixType *resolve_expr(Sema *sema, AstNode *expr);

static bool func_param_is_mutable(MixType *func_type, int index) {
    return func_type &&
           func_type->kind == TYPE_FUNC &&
           func_type->func.param_mutable &&
           index >= 0 &&
           index < func_type->func.param_count &&
           func_type->func.param_mutable[index];
}

static bool func_has_mutable_params(MixType *func_type) {
    if (!func_type || func_type->kind != TYPE_FUNC ||
        !func_type->func.param_mutable) {
        return false;
    }
    for (int i = 0; i < func_type->func.param_count; i++) {
        if (func_type->func.param_mutable[i]) return true;
    }
    return false;
}

static MixType *unwrap_ref_type(MixType *type) {
    while (type &&
           (type->kind == TYPE_REF || type->kind == TYPE_BOX)) {
        if (type->kind == TYPE_REF) type = type->ref.base;
        else type = type->box.inner;
    }
    return type;
}

static MixType *resolve_collection_type_expr(Sema *sema, AstNode *expr) {
    if (!expr || expr->kind != NODE_IDENT || expr->ident.type_arg_count <= 0) {
        return NULL;
    }

    const char *name = expr->ident.name;
    if (strcmp(name, "List") == 0) {
        if (expr->ident.type_arg_count != 1) {
            mix_error(expr->loc, "List expects exactly 1 type argument");
            mix_help(expr->loc, "write `List[YourType]` to create a typed list");
            return make_type(sema->arena, TYPE_VOID);
        }
        return make_list_type(sema->arena,
            resolve_type_node(sema, expr->ident.type_args[0]));
    }
    if (strcmp(name, "Map") == 0) {
        if (expr->ident.type_arg_count != 2) {
            mix_error(expr->loc, "Map expects exactly 2 type arguments");
            mix_help(expr->loc, "write `Map[KeyType, ValueType]` to create a typed map");
            return make_type(sema->arena, TYPE_VOID);
        }
        return make_map_type(sema->arena,
            resolve_type_node(sema, expr->ident.type_args[0]),
            resolve_type_node(sema, expr->ident.type_args[1]));
    }
    if (strcmp(name, "Set") == 0) {
        if (expr->ident.type_arg_count != 1) {
            mix_error(expr->loc, "Set expects exactly 1 type argument");
            mix_help(expr->loc, "write `Set[YourType]` to create a typed set");
            return make_type(sema->arena, TYPE_VOID);
        }
        return make_set_type(sema->arena,
            resolve_type_node(sema, expr->ident.type_args[0]));
    }
    return NULL;
}

static bool is_list_at_mut_call(AstNode *expr) {
    return expr &&
           expr->kind == NODE_METHOD_CALL &&
           expr->method_call.is_mutable_call &&
           strcmp(expr->method_call.method_name, "at_mut") == 0 &&
           expr->method_call.object &&
           unwrap_ref_type(expr->method_call.object->resolved_type) &&
           unwrap_ref_type(expr->method_call.object->resolved_type)->kind == TYPE_LIST;
}

static bool expr_is_borrowable(AstNode *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case NODE_IDENT:
            return true;
        case NODE_FIELD_EXPR: {
            MixType *obj_type = unwrap_ref_type(
                expr->field_expr.object ? expr->field_expr.object->resolved_type : NULL);
            return obj_type && obj_type->kind == TYPE_SHAPE &&
                   type_find_field(obj_type, expr->field_expr.field_name) != NULL;
        }
        case NODE_INDEX_EXPR: {
            MixType *obj_type = unwrap_ref_type(
                expr->index_expr.object ? expr->index_expr.object->resolved_type : NULL);
            return obj_type && obj_type->kind == TYPE_LIST;
        }
        case NODE_METHOD_CALL:
            return expr->resolved_type && expr->resolved_type->kind == TYPE_REF;
        case NODE_UNARY_EXPR:
            return expr->unary.op == TOK_STAR;
        default:
            return false;
    }
}

static AstNode *method_receiver_root(AstNode *expr) {
    while (expr) {
        if (expr->kind == NODE_FIELD_EXPR) {
            expr = expr->field_expr.object;
            continue;
        }
        if (is_list_at_mut_call(expr)) {
            expr = expr->method_call.object;
            continue;
        }
        break;
    }
    return expr;
}

static bool expr_is_mutable_place(Sema *sema, AstNode *expr) {
    (void)sema;
    if (!expr) return false;
    if (expr->resolved_type && expr->resolved_type->kind == TYPE_REF) {
        return expr->resolved_type->ref.is_mutable;
    }
    if (expr->kind == NODE_UNARY_EXPR && expr->unary.op == TOK_STAR) {
        return true;
    }
    AstNode *root = method_receiver_root(expr);
    if (root && root->kind == NODE_IDENT) {
        Symbol *root_sym = symtab_lookup(&sema->symtab, root->ident.name);
        return root_sym && root_sym->is_mutable;
    }
    return false;
}

static bool type_uses_eightbyte_list_repr(MixType *type) {
    if (!type) return true;
    if (type->kind == TYPE_SHAPE) return true;
    return type_size(type) == 8;
}

static void require_mutable_call_argument(Sema *sema, AstNode *arg,
                                          MixType *expected_type,
                                          const char *callee_name,
                                          int arg_index) {
    if (!arg) return;

    MixType *arg_type = arg->resolved_type;
    if (arg_type && arg_type->kind == TYPE_REF) {
        if (!arg_type->ref.is_mutable) {
            mix_error(arg->loc,
                      "argument %d of '%s' must be mutable",
                      arg_index + 1, callee_name);
            mix_help(arg->loc, "pass a mutable ref (`ref!`) or a value declared with `!`");
            return;
        }
        if (expected_type && expected_type->kind == TYPE_SHAPE) {
            mix_error(arg->loc,
                      "argument %d of '%s' cannot bind mutable shape parameter "
                      "through ref! yet",
                      arg_index + 1, callee_name);
            mix_help(arg->loc, "pass the mutable shape variable directly for now");
            return;
        }
        return;
    }

    if (arg->kind == NODE_UNARY_EXPR && arg->unary.op == TOK_STAR) {
        return;
    }

    if (is_list_at_mut_call(arg) &&
        expected_type && !type_uses_eightbyte_list_repr(expected_type)) {
        mix_error(arg->loc,
                  "argument %d of '%s' cannot bind mutable %s parameter "
                  "through `at_mut!` yet",
                  arg_index + 1, callee_name,
                  type_name(sema->arena, expected_type));
        mix_help(arg->loc, "store the element in a mutable local, call '%s', then write it back", callee_name);
        return;
    }

    if (arg->kind == NODE_IDENT) {
        Symbol *sym = symtab_lookup(&sema->symtab, arg->ident.name);
        if (sym) {
            if (sym->is_mutable) {
                if (expected_type && expected_type->kind == TYPE_SHAPE &&
                    sema->current_shape &&
                    type_find_field(sema->current_shape, arg->ident.name)) {
                    mix_error(arg->loc,
                              "argument %d of '%s' cannot bind mutable shape "
                              "parameter to inline field '%s'",
                              arg_index + 1, callee_name, arg->ident.name);
                    mix_help(arg->loc, "copy the field into a mutable local before passing it");
                }
                return;
            }
            mix_error(arg->loc,
                      "argument %d of '%s' must be mutable "
                      "(declare `%s! = ...`)",
                      arg_index + 1, callee_name, arg->ident.name);
            mix_help(arg->loc, "change the declaration to `%s! = ...` if '%s' should modify it",
                     arg->ident.name, callee_name);
            return;
        }
    }

    if (arg->kind == NODE_FIELD_EXPR) {
        if (expected_type && expected_type->kind == TYPE_SHAPE) {
            mix_error(arg->loc,
                      "argument %d of '%s' cannot bind mutable shape "
                      "parameter to an inline field",
                      arg_index + 1, callee_name);
            mix_help(arg->loc, "copy the field into a mutable local before passing it");
            return;
        }
    }

    if (expr_is_mutable_place(sema, arg)) return;

    AstNode *root = method_receiver_root(arg);
    if (root && root->kind == NODE_IDENT) {
        Symbol *root_sym = symtab_lookup(&sema->symtab, root->ident.name);
        if (root_sym) {
            mix_error(arg->loc,
                      "argument %d of '%s' must be mutable through '%s' "
                      "(declare `%s! = ...`)",
                      arg_index + 1, callee_name,
                      root->ident.name, root->ident.name);
            mix_help(arg->loc, "change the root binding to `%s! = ...`", root->ident.name);
            return;
        }
    }

    mix_error(arg->loc,
              "argument %d of '%s' must be a mutable variable, field, or dereference",
              arg_index + 1, callee_name);
    mix_help(arg->loc, "store the value in a mutable local before passing it");
}

static void require_mutable_method_receiver(Sema *sema, AstNode *object,
                                            const char *method_name,
                                            SrcLoc loc) {
    if (expr_is_mutable_place(sema, object)) return;

    AstNode *root = method_receiver_root(object);
    if (root && root->kind == NODE_IDENT) {
        Symbol *root_sym = symtab_lookup(&sema->symtab, root->ident.name);
        if (root_sym) {
            mix_error(loc,
                      "cannot call mutating method '%s!' through immutable '%s' "
                      "(declare it as `%s! = ...`)",
                      method_name, root->ident.name, root->ident.name);
            mix_help(loc, "change the receiver declaration to `%s! = ...`", root->ident.name);
        }
        return;
    }
    mix_error(loc, "cannot call mutating method '%s!' on a non-mutable temporary",
              method_name);
    mix_help(loc, "store the receiver in a mutable variable first, then call `%s!`", method_name);
}

static void require_mutable_loop_iterable(Sema *sema, AstNode *iterable,
                                          const char *var_name, SrcLoc loc) {
    AstNode *root = method_receiver_root(iterable);
    if (root && root->kind == NODE_IDENT) {
        Symbol *root_sym = symtab_lookup(&sema->symtab, root->ident.name);
        if (root_sym && root_sym->is_mutable) return;
        if (root_sym) {
            if (strcmp(root->ident.name, "self") == 0) {
                mix_error(
                    loc,
                    "cannot use mutable loop binding '%s!' through immutable "
                    "'self' (declare the method with a trailing `!`)",
                    var_name);
                mix_help(loc,
                         "add `!` to the method name to make it mutating: `fn method_name!(...)`");
            } else {
                mix_error(
                    loc,
                    "cannot use mutable loop binding '%s!' through immutable "
                    "'%s' (declare `%s! = ...`)",
                    var_name, root->ident.name, root->ident.name);
                mix_help(loc,
                         "change the declaration to `%s! = ...` to make it mutable",
                         root->ident.name);
            }
            return;
        }
    }

    mix_error(loc,
              "cannot use mutable loop binding '%s!' over a non-mutable temporary",
              var_name);
    mix_help(loc,
             "store the value in a mutable variable before using it in this loop");
}

static bool is_mutating_builtin_method(MixType *obj_type, const char *method_name) {
    obj_type = unwrap_ref_type(obj_type);
    if (!obj_type) return false;
    switch (obj_type->kind) {
        case TYPE_SHARED:
            return strcmp(method_name, "update") == 0;
        case TYPE_LIST:
            return strcmp(method_name, "push") == 0 ||
                   strcmp(method_name, "pop") == 0 ||
                   strcmp(method_name, "remove") == 0 ||
                   strcmp(method_name, "insert") == 0 ||
                   strcmp(method_name, "sort") == 0 ||
                   strcmp(method_name, "reverse") == 0;
        case TYPE_MAP:
            return strcmp(method_name, "remove") == 0;
        case TYPE_SET:
            return strcmp(method_name, "add") == 0 ||
                   strcmp(method_name, "remove") == 0;
        default:
            return false;
    }
}

// If `expr` is an empty collection literal whose default-inferred element
// type doesn't match the surrounding context, retype it to the hint. Lets
// `bricks.sprites = []` work even though `[]` would otherwise resolve to
// [int]. Operates on lists, sets, and maps; no-op for everything else.
static MixType *resolve_expr_hinted(Sema *sema, AstNode *expr, MixType *hint) {
    MixType *t = resolve_expr(sema, expr);
    if (!hint || !expr) return t;
    if (expr->kind == NODE_LIST_LIT && expr->list_lit.element_count == 0
        && hint->kind == TYPE_LIST) {
        expr->resolved_type = hint;
        return hint;
    }
    if (expr->kind == NODE_SET_LIT && expr->set_lit.element_count == 0
        && hint->kind == TYPE_SET) {
        expr->resolved_type = hint;
        return hint;
    }
    if (expr->kind == NODE_MAP_LIT && expr->map_lit.entry_count == 0
        && hint->kind == TYPE_MAP) {
        expr->resolved_type = hint;
        return hint;
    }
    return t;
}

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
                if (expr->string_interp.exprs[i2]->resolved_type &&
                    expr->string_interp.exprs[i2]->resolved_type->kind == TYPE_FUNC) {
                    mix_error(expr->loc, "cannot convert function to string in interpolated expression");
                    mix_help(expr->loc, "call the function first: `{func()}` instead of `{func}`");
                }
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
            MixType *collection_type = resolve_collection_type_expr(sema, expr);
            if (collection_type) {
                mix_error(expr->loc,
                          "type expression '%s' cannot be used as a value; use `.new(zone)`",
                          type_name(sema->arena, collection_type));
                expr->resolved_type = collection_type;
                return expr->resolved_type;
            }
            Symbol *sym = symtab_lookup(&sema->symtab, expr->ident.name);
            if (!sym) {
                mix_error(expr->loc, "undefined variable '%s'", expr->ident.name);
                const char *suggestion = find_similar_name(&sema->symtab, expr->ident.name, 2);
                if (suggestion) {
                    mix_help(expr->loc, "did you mean '%s'?", suggestion);
                } else {
                    mix_help(expr->loc, "declare '%s' before using it, or check the spelling/import", expr->ident.name);
                }
                expr->resolved_type = make_type(sema->arena, TYPE_INT);
            } else {
                expr->resolved_type = sym->type;
                // Mark whether the variable was declared as mutable
                // (needed by emitter to know if load or copy)
                expr->ident.is_mutable = sym->is_mutable;
                // Sema's scopes are gone by emit time; capture this flag
                // here so the C backend knows whether the binding is the
                // pointer (the usual shape var) or a slot holding the
                // pointer (a for-each loop var over a shape list).
                expr->ident.is_pointer_slot = sym->is_pointer_slot;
                expr->ident.is_stack_slot = sym->is_stack_slot;
            }
            return expr->resolved_type;
        }
        case NODE_BINARY_EXPR: {
            MixType *left = resolve_expr(sema, expr->binary.left);
            MixType *right = resolve_expr(sema, expr->binary.right);

            // String concatenation: str + str -> str
            if (expr->binary.op == TOK_PLUS &&
                ((left && left->kind == TYPE_STR) || (right && right->kind == TYPE_STR))) {
                if (left && right && left->kind == TYPE_STR && right->kind == TYPE_STR) {
                    expr->resolved_type = make_type(sema->arena, TYPE_STR);
                } else {
                    mix_error(expr->loc,
                              "cannot apply operator '%s' to %s and %s",
                              binary_op_name(expr->binary.op),
                              type_name(sema->arena, left),
                              type_name(sema->arena, right));
                    mix_help(expr->loc,
                             "make both sides strings, or convert one side before using '+'");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
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

            if (expr->binary.op == TOK_PLUS || expr->binary.op == TOK_MINUS ||
                expr->binary.op == TOK_STAR || expr->binary.op == TOK_SLASH ||
                expr->binary.op == TOK_PERCENT || expr->binary.op == TOK_PIPE) {
                bool ptr_arith = (expr->binary.op == TOK_PLUS || expr->binary.op == TOK_MINUS) &&
                    ((left && left->kind == TYPE_PTR && right && type_is_integer(right)) ||
                     (right && right->kind == TYPE_PTR && left && type_is_integer(left)));
                if (ptr_arith) {
                    expr->resolved_type = (left && left->kind == TYPE_PTR) ? left : right;
                    return expr->resolved_type;
                }

                bool ok = false;
                if (type_is_unresolved_for_binary(left) ||
                    type_is_unresolved_for_binary(right)) {
                    ok = true;
                } else if (expr->binary.op == TOK_PERCENT || expr->binary.op == TOK_PIPE) {
                    ok = left && right && type_is_integer(left) && type_is_integer(right);
                } else {
                    ok = left && right &&
                         ((type_is_integer(left) && type_is_integer(right)) ||
                          (type_is_float(left) && type_is_float(right)));
                }

                if (!ok) {
                    mix_error(expr->loc,
                              "cannot apply operator '%s' to %s and %s",
                              binary_op_name(expr->binary.op),
                              type_name(sema->arena, left),
                              type_name(sema->arena, right));
                    mix_help(expr->loc,
                             "convert one operand or use an operator both types support");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    return expr->resolved_type;
                }

                expr->resolved_type = left;
                return expr->resolved_type;
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
            } else if (expr->unary.op == TOK_REF ||
                       expr->unary.op == TOK_REF_MUT) {
                if (!expr_is_borrowable(expr->unary.operand)) {
                    mix_error(expr->loc, "cannot borrow a non-addressable expression");
                    mix_help(expr->loc, "borrow a variable, field, index, or dereference instead of a temporary value");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (operand && operand->kind == TYPE_REF) {
                    mix_error(expr->loc, "cannot borrow an existing ref value");
                    mix_help(expr->loc, "use the existing ref directly, or dereference it before borrowing again");
                    expr->resolved_type = operand;
                } else {
                    bool want_mut = expr->unary.op == TOK_REF_MUT;
                    if (want_mut && !expr_is_mutable_place(sema, expr->unary.operand)) {
                        AstNode *root = method_receiver_root(expr->unary.operand);
                        if (root && root->kind == NODE_IDENT) {
                            mix_error(expr->loc,
                                      "cannot take mutable ref through immutable '%s' "
                                      "(declare it as `%s! = ...`)",
                                      root->ident.name, root->ident.name);
                            mix_help(expr->loc, "change the binding to `%s! = ...` if this value should be mutated", root->ident.name);
                        } else {
                            mix_error(expr->loc,
                                      "cannot take mutable ref of a non-mutable temporary");
                            mix_help(expr->loc, "store the value in a mutable variable first, then take the mutable ref");
                        }
                        expr->resolved_type = make_ref_type(
                            sema->arena, operand, false);
                    } else {
                        expr->resolved_type = make_ref_type(
                            sema->arena, operand, want_mut);
                    }
                }
            } else if (expr->unary.op == TOK_AMPERSAND) {
                expr->resolved_type = make_ptr_type(sema->arena, operand);
            } else if (expr->unary.op == TOK_STAR &&
                       operand &&
                       (operand->kind == TYPE_PTR || operand->kind == TYPE_REF ||
                        operand->kind == TYPE_BOX)) {
                expr->resolved_type = operand->kind == TYPE_PTR
                    ? operand->ptr.base
                    : (operand->kind == TYPE_REF
                        ? operand->ref.base
                        : operand->box.inner);
            } else {
                expr->resolved_type = operand;
            }
            return expr->resolved_type;
        }
        case NODE_ELSE_EXPR: {
            MixType *val_type = resolve_expr(sema, expr->else_expr.value);
            MixType *fb_type  = resolve_expr(sema, expr->else_expr.fallback);
            // Result type is the inner type of the optional or result
            if (val_type && val_type->kind == TYPE_OPTIONAL) {
                expr->resolved_type = val_type->optional.inner;
            } else if (val_type && val_type->kind == TYPE_RESULT) {
                expr->resolved_type = val_type->result.ok_type;
            } else if (val_type && val_type->kind == TYPE_VOID) {
                // `none else x` — value is bare none; type comes from fallback.
                expr->resolved_type = fb_type;
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
            if (expr->lambda.param_count > 0) {
                func_type->func.param_mutable = arena_alloc(sema->arena,
                    sizeof(bool) * expr->lambda.param_count);
                memset(func_type->func.param_mutable, 0,
                       sizeof(bool) * expr->lambda.param_count);
            }
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
            MixType *obj_type = unwrap_ref_type(
                resolve_expr(sema, expr->index_expr.object));
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
            // Slice of a list returns same list type; slice of str returns str
            if (obj_type && obj_type->kind == TYPE_LIST) {
                expr->resolved_type = obj_type;
            } else if (obj_type && obj_type->kind == TYPE_STR) {
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
            if ((strcmp(expr->call.name, "box") == 0 ||
                 strcmp(expr->call.name, "promote") == 0)) {
                if (expr->call.arg_count != 2) {
                    mix_error(expr->loc, "'%s' expects 2 argument(s), got %d",
                              expr->call.name, expr->call.arg_count);
                    mix_help(expr->loc, "call it as `%s(zone, value)`", expr->call.name);
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    for (int i = 0; i < expr->call.arg_count; i++) {
                        resolve_expr(sema, expr->call.args[i]);
                    }
                    return expr->resolved_type;
                }
                MixType *zone_type = resolve_expr(sema, expr->call.args[0]);
                MixType *value_type = resolve_expr(sema, expr->call.args[1]);
                if (!zone_type || zone_type->kind != TYPE_ZONE) {
                    mix_error(expr->call.args[0]->loc,
                              "argument 1 of '%s': expected Zone, got %s",
                              expr->call.name, type_name(sema->arena, zone_type));
                    mix_help(expr->call.args[0]->loc, "pass an active Zone as the first argument");
                }
                if (!value_type || value_type->kind == TYPE_VOID) {
                    mix_error(expr->call.args[1]->loc,
                              "argument 2 of '%s' must be a value",
                              expr->call.name);
                    mix_help(expr->call.args[1]->loc, "pass the value you want to store in the box");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    return expr->resolved_type;
                }
                MixType *inner = value_type;
                if (inner->kind == TYPE_REF) inner = inner->ref.base;
                if (inner->kind == TYPE_BOX) inner = inner->box.inner;
                expr->resolved_type = make_box_type(sema->arena, inner);
                return expr->resolved_type;
            }
            Symbol *sym = symtab_lookup(&sema->symtab, expr->call.name);
            if (!sym) {
                mix_error(expr->loc, "undefined function '%s'", expr->call.name);
                const char *suggestion = find_similar_name(&sema->symtab, expr->call.name, 2);
                if (suggestion) {
                    mix_help(expr->loc, "did you mean '%s'?", suggestion);
                } else {
                    mix_help(expr->loc, "define '%s' before calling it, or add the module that exports it with `use`", expr->call.name);
                }
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
                // Inject default values for any missing trailing args.
                // The earlier walk already verified defaults form a
                // trailing run, so we only need to fill from arg_count
                // up to the first param without a default.
                if (!ftype->func.is_variadic
                    && expr->call.arg_count < ftype->func.param_count
                    && ftype->func.param_defaults) {
                    int needed = ftype->func.param_count;
                    int have = expr->call.arg_count;
                    bool can_fill = true;
                    for (int i = have; i < needed; i++) {
                        if (!ftype->func.param_defaults[i]) {
                            can_fill = false;
                            break;
                        }
                    }
                    if (can_fill) {
                        AstNode **filled = arena_alloc(sema->arena,
                            sizeof(AstNode *) * needed);
                        for (int i = 0; i < have; i++)
                            filled[i] = expr->call.args[i];
                        for (int i = have; i < needed; i++) {
                            AstNode *def = (AstNode *)ftype->func.param_defaults[i];
                            // Default exprs are stored once on the
                            // function decl; re-resolve at the call
                            // site so per-call type info is fresh.
                            resolve_expr(sema, def);
                            filled[i] = def;
                        }
                        expr->call.args = filled;
                        expr->call.arg_count = needed;
                    }
                }
                // Check argument count (skip variadic and builtins with overloads)
                if (!ftype->func.is_variadic && ftype->func.param_count > 0 &&
                    expr->call.arg_count != ftype->func.param_count) {
                    // Skip count check for overloaded builtins (print, min, max, etc.)
                    const char *n = expr->call.name;
                    bool is_overloaded = (strcmp(n, "print") == 0 || strcmp(n, "to_string") == 0 ||
                        strcmp(n, "to_int") == 0 || strcmp(n, "to_float") == 0 ||
                        strcmp(n, "to_set") == 0 ||
                        strcmp(n, "max") == 0 || strcmp(n, "min") == 0);
                    if (!is_overloaded) {
                        mix_error(expr->loc, "'%s' expects %d argument(s), got %d",
                                  expr->call.name, ftype->func.param_count, expr->call.arg_count);
                        mix_help(expr->loc, "add missing arguments or remove extras to match '%s'", expr->call.name);
                    }
                }
                // Check argument types
                for (int i = 0; i < expr->call.arg_count && i < ftype->func.param_count; i++) {
                    MixType *expected = ftype->func.param_types[i];
                    MixType *actual = expr->call.args[i]->resolved_type;
                    if (func_param_is_mutable(ftype, i)) {
                        require_mutable_call_argument(
                            sema, expr->call.args[i], expected,
                            expr->call.name, i);
                        if (actual && actual->kind == TYPE_REF) {
                            actual = actual->ref.base;
                        }
                    }
                    if (!types_compatible(expected, actual)) {
                        mix_error(expr->call.args[i]->loc,
                                  "argument %d of '%s': expected %s, got %s",
                                  i + 1, expr->call.name,
                                  type_name(sema->arena, expected),
                                  type_name(sema->arena, actual));
                        mix_help(expr->call.args[i]->loc,
                                 "change this argument or update '%s' to accept %s",
                                 expr->call.name, type_name(sema->arena, actual));
                    }
                }
                // Enforce `has` constraints on the inferred type parameter.
                // Only applies to generic functions (constraint_count > 0).
                if (ftype->func.constraint_count > 0) {
                    MixType *t_concrete = infer_generic_arg(
                        ftype, expr->call.args, expr->call.arg_count);
                    if (t_concrete) {
                        for (int ci = 0; ci < ftype->func.constraint_count; ci++) {
                            const char *c = ftype->func.constraints[ci];
                            if (!type_satisfies_constraint(t_concrete, c, &sema->symtab)) {
                                mix_error(expr->loc,
                                    "type %s does not satisfy constraint '%s' on '%s'",
                                    type_name(sema->arena, t_concrete),
                                    constraint_label(c), expr->call.name);
                                mix_help(expr->loc,
                                    "constraint '%s' requires the type to have specific methods or fields; implement them or choose a different type",
                                    constraint_label(c));
                            }
                        }
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
            // min/max: infer return type from args (list or scalar pair)
            if ((strcmp(expr->call.name, "min") == 0 || strcmp(expr->call.name, "max") == 0)) {
                if (expr->call.arg_count == 1) {
                    MixType *arg_type = expr->call.args[0]->resolved_type;
                    if (arg_type && arg_type->kind == TYPE_LIST && arg_type->list.elem_type) {
                        expr->resolved_type = arg_type->list.elem_type;
                    } else {
                        expr->resolved_type = make_type(sema->arena, TYPE_INT);
                    }
                } else if (expr->call.arg_count == 2) {
                    MixType *t0 = expr->call.args[0]->resolved_type;
                    MixType *t1 = expr->call.args[1]->resolved_type;
                    if ((t0 && type_is_float(t0)) || (t1 && type_is_float(t1))) {
                        expr->resolved_type = make_type(sema->arena, TYPE_FLOAT);
                    } else {
                        expr->resolved_type = make_type(sema->arena, TYPE_INT);
                    }
                }
            }
            return expr->resolved_type;
        }
        handle_shape_lit:
        case NODE_SHAPE_LIT: {
            MixType *stype = NULL;
            // Generic shape constructor: `Stack[int](items: [])` — look up
            // or build the instantiation, then redirect shape_name so the
            // emitter dispatches against the mangled methods.
            if (expr->shape_lit.type_arg_count > 0) {
                stype = instantiate_generic_shape(sema,
                    expr->shape_lit.shape_name,
                    expr->shape_lit.type_args,
                    expr->shape_lit.type_arg_count);
                if (stype) {
                    expr->shape_lit.shape_name = stype->shape.name;
                }
            } else {
                // Constructor inference: `Stack(items: [42])` — if the
                // shape name is a generic template, infer the type args
                // from the supplied field values.
                GenericShapeTemplate *tpl = find_template(sema, expr->shape_lit.shape_name);
                if (tpl) {
                    AstNode *tdecl = tpl->decl;
                    int tpc = tdecl->shape_decl.type_param_count;
                    AstNode **inferred = arena_alloc(sema->arena, sizeof(AstNode*) * tpc);
                    for (int i = 0; i < tpc; i++) inferred[i] = NULL;
                    // Resolve field-value types first.
                    for (int i = 0; i < expr->shape_lit.field_count; i++)
                        resolve_expr(sema, expr->shape_lit.field_values[i]);
                    // For each type param, walk template fields looking for
                    // a field whose declared type is `T` or `[T]` and pick
                    // up the actual type from the corresponding arg.
                    for (int p = 0; p < tpc; p++) {
                        const char *tname = tdecl->shape_decl.type_params[p];
                        for (int fi = 0; fi < tdecl->shape_decl.field_count && !inferred[p]; fi++) {
                            ShapeField *sf = &tdecl->shape_decl.fields[fi];
                            if (!sf->type) continue;
                            // Find which user arg matches this field by name
                            // (named args) or by position.
                            AstNode *arg_val = NULL;
                            if (expr->shape_lit.field_names) {
                                for (int ui = 0; ui < expr->shape_lit.field_count; ui++) {
                                    if (expr->shape_lit.field_names[ui] &&
                                        strcmp(expr->shape_lit.field_names[ui], sf->name) == 0) {
                                        arg_val = expr->shape_lit.field_values[ui];
                                        break;
                                    }
                                }
                            } else if (fi < expr->shape_lit.field_count) {
                                arg_val = expr->shape_lit.field_values[fi];
                            }
                            if (!arg_val || !arg_val->resolved_type) continue;
                            MixType *at = arg_val->resolved_type;
                            // Field type is bare T -> inferred = at
                            if (sf->type->kind == NODE_TYPE_NAME &&
                                sf->type->type_name.name &&
                                strcmp(sf->type->type_name.name, tname) == 0) {
                                // Build a synthetic type-name node from `at`'s kind.
                                AstNode *syn = arena_alloc(sema->arena, sizeof(AstNode));
                                memset(syn, 0, sizeof(*syn));
                                syn->kind = NODE_TYPE_NAME;
                                const char *kname = type_kind_name(at->kind);
                                syn->type_name.name = arena_strdup(sema->arena, kname);
                                // Map common kinds back to TokenKind for resolve.
                                switch (at->kind) {
                                    case TYPE_INT: syn->type_name.type_kind = TOK_INT; break;
                                    case TYPE_FLOAT: syn->type_name.type_kind = TOK_FLOAT; break;
                                    case TYPE_STR: syn->type_name.type_kind = TOK_STR; break;
                                    case TYPE_BOOL: syn->type_name.type_kind = TOK_BOOL; break;
                                    default: syn->type_name.type_kind = TOK_INT; break;
                                }
                                inferred[p] = syn;
                            }
                            // Field type is [T] and arg is a list -> inferred = list elem type
                            else if (sf->type->kind == NODE_TYPE_NAME &&
                                     sf->type->type_name.type_kind == TOK_LBRACKET &&
                                     at->kind == TYPE_LIST && at->list.elem_type) {
                                MixType *et = at->list.elem_type;
                                if (et->kind == TYPE_INFER) continue;  // empty list — try another field
                                AstNode *syn = arena_alloc(sema->arena, sizeof(AstNode));
                                memset(syn, 0, sizeof(*syn));
                                syn->kind = NODE_TYPE_NAME;
                                const char *kname = type_kind_name(et->kind);
                                syn->type_name.name = arena_strdup(sema->arena, kname);
                                switch (et->kind) {
                                    case TYPE_INT: syn->type_name.type_kind = TOK_INT; break;
                                    case TYPE_FLOAT: syn->type_name.type_kind = TOK_FLOAT; break;
                                    case TYPE_STR: syn->type_name.type_kind = TOK_STR; break;
                                    case TYPE_BOOL: syn->type_name.type_kind = TOK_BOOL; break;
                                    default: syn->type_name.type_kind = TOK_INT; break;
                                }
                                inferred[p] = syn;
                            }
                        }
                    }
                    // Default any uninferred param (e.g. all-empty-lists case)
                    // to int — keeps the legacy behavior where untyped Stack
                    // worked as int.
                    for (int p = 0; p < tpc; p++) {
                        if (!inferred[p]) {
                            AstNode *syn = arena_alloc(sema->arena, sizeof(AstNode));
                            memset(syn, 0, sizeof(*syn));
                            syn->kind = NODE_TYPE_NAME;
                            syn->type_name.name = "int";
                            syn->type_name.type_kind = TOK_INT;
                            inferred[p] = syn;
                        }
                    }
                    stype = instantiate_generic_shape(sema, expr->shape_lit.shape_name,
                                                      inferred, tpc);
                    if (stype) {
                        expr->shape_lit.shape_name = stype->shape.name;
                        expr->shape_lit.type_args = inferred;
                        expr->shape_lit.type_arg_count = tpc;
                    }
                }
            }
            // Look up the shape type — could be a direct shape or a variant constructor
            if (!stype) {
                Symbol *sym = symtab_lookup(&sema->symtab, expr->shape_lit.shape_name);
                if (sym && sym->type && sym->type->kind == TYPE_SHAPE) {
                    stype = sym->type;
                } else if (sym && sym->type && sym->type->kind == TYPE_FUNC &&
                           sym->type->func.return_type && sym->type->func.return_type->kind == TYPE_SHAPE) {
                    stype = sym->type->func.return_type;
                } else {
                    mix_error(expr->loc, "unknown shape '%s'", expr->shape_lit.shape_name);
                    mix_help(expr->loc, "declare the shape before using it, or check the spelling");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    return expr->resolved_type;
                }
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
            // Resolve field value expressions, hinting with each field's
            // declared type so empty collection literals (e.g.
            // `Group(sprites: [])`) take on the correct element type.
            for (int i = 0; i < expr->shape_lit.field_count; i++) {
                MixType *hint = NULL;
                if (stype && expr->shape_lit.field_names &&
                    expr->shape_lit.field_names[i]) {
                    ShapeFieldInfo *fi = type_find_field(stype,
                        expr->shape_lit.field_names[i]);
                    if (fi) hint = fi->type;
                }
                resolve_expr_hinted(sema, expr->shape_lit.field_values[i], hint);
            }
            return expr->resolved_type;
        }
        case NODE_FIELD_EXPR: {
            MixType *obj_type = resolve_expr(sema, expr->field_expr.object);
            MixType *obj_base = unwrap_ref_type(obj_type);
            if (obj_base && obj_base->kind == TYPE_LIST) {
                // List built-in fields: len
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else {
                    mix_error(expr->loc, "list has no field '%s'", expr->field_expr.field_name);
                    mix_help(expr->loc, "use `.len` for list length, or call a list method with parentheses");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_base && obj_base->kind == TYPE_MAP) {
                // Map built-in fields: len, keys, values
                const char *fn = expr->field_expr.field_name;
                if (strcmp(fn, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(fn, "keys") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, obj_base->map.key_type);
                } else if (strcmp(fn, "values") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, obj_base->map.val_type);
                } else {
                    mix_error(expr->loc, "map has no field '%s'", fn);
                    mix_help(expr->loc, "map fields are `.len`, `.keys`, and `.values`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_base && obj_base->kind == TYPE_SET) {
                // Set built-in fields: len, values
                const char *fn = expr->field_expr.field_name;
                if (strcmp(fn, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(fn, "values") == 0) {
                    expr->resolved_type = make_list_type(sema->arena, obj_base->set.elem_type);
                } else {
                    mix_error(expr->loc, "set has no field '%s'", fn);
                    mix_help(expr->loc, "set fields are `.len` and `.values`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_base && obj_base->kind == TYPE_STR) {
                // String built-in fields: len
                if (strcmp(expr->field_expr.field_name, "len") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else {
                    mix_error(expr->loc, "str has no field '%s'", expr->field_expr.field_name);
                    mix_help(expr->loc, "use `.len` for string length, or call a string method with parentheses");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
            } else if (obj_base && obj_base->kind == TYPE_SHAPE) {
                ShapeFieldInfo *fi = type_find_field(obj_base, expr->field_expr.field_name);
                if (fi) {
                    expr->resolved_type = fi->type;
                } else {
                    // Try zero-param method (computed field): r.area → r.area()
                    char mangled[256];
                    snprintf(mangled, sizeof(mangled), "%s_%s",
                             obj_base->shape.name, expr->field_expr.field_name);
                    Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                    if (msym && msym->type && msym->type->kind == TYPE_FUNC) {
                        expr->resolved_type = msym->type->func.return_type;
                    } else {
                        mix_error(expr->loc, "shape '%s' has no field '%s'",
                                  obj_base->shape.name, expr->field_expr.field_name);
                        mix_help(expr->loc, "add the field to shape '%s' or check the field name", obj_base->shape.name);
                        expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    }
                }
            } else {
                expr->resolved_type = make_type(sema->arena, TYPE_VOID);
            }
            return expr->resolved_type;
        }
        case NODE_METHOD_CALL: {
            // Resolve argument expressions
            for (int i2 = 0; i2 < expr->method_call.arg_count; i2++) {
                resolve_expr(sema, expr->method_call.args[i2]);
            }
            MixType *static_collection = resolve_collection_type_expr(
                sema, expr->method_call.object);
            if (static_collection) {
                expr->method_call.object->resolved_type = static_collection;
                const char *m = expr->method_call.method_name;
                if (strcmp(m, "new") != 0) {
                    mix_error(expr->loc,
                              "type expression '%s' has no static method '%s'",
                              type_name(sema->arena, static_collection), m);
                    mix_help(expr->loc, "use `.new(zone)` to create this collection type");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    return expr->resolved_type;
                }
                if (expr->method_call.arg_count != 1) {
                    mix_error(expr->loc,
                              "method 'new' expects exactly 1 argument: Zone");
                    mix_help(expr->loc, "call it as `.new(zone)`");
                    expr->resolved_type = static_collection;
                    return expr->resolved_type;
                }
                MixType *zone_type = expr->method_call.args[0]->resolved_type;
                if (!zone_type || zone_type->kind != TYPE_ZONE) {
                    mix_error(expr->method_call.args[0]->loc,
                              "argument 1 of 'new': expected Zone, got %s",
                              type_name(sema->arena, zone_type));
                    mix_help(expr->method_call.args[0]->loc, "pass the Zone that should own the collection storage");
                }
                expr->resolved_type = static_collection;
                return expr->resolved_type;
            }
            MixType *obj_type = resolve_expr(sema, expr->method_call.object);
            MixType *obj_base = unwrap_ref_type(obj_type);
            // Shared built-in methods: .read(), .update!(fn)
            if (obj_base && obj_base->kind == TYPE_SHARED) {
                const char *m = expr->method_call.method_name;
                if (is_mutating_builtin_method(obj_type, m) &&
                    !expr->method_call.is_mutable_call) {
                    mix_error(expr->loc,
                              "method '%s' is mutating; call it as `%s!(...)`",
                              m, m);
                } else if (is_mutating_builtin_method(obj_type, m)) {
                    require_mutable_method_receiver(
                        sema, expr->method_call.object, m, expr->loc);
                }
                if (strcmp(m, "read") == 0) {
                    expr->resolved_type = obj_base->shared.inner;
                } else if (strcmp(m, "update") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else {
                    mix_error(expr->loc, "shared has no method '%s'", m);
                    mix_help(expr->loc, "shared values support `read()` and `update!(...)`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // String built-in methods
            if (obj_base && obj_base->kind == TYPE_STR) {
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
                    mix_help(expr->loc, "check the method name; common string methods include `split`, `contains`, `trim`, and `upper`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // List built-in methods
            if (obj_base && obj_base->kind == TYPE_LIST) {
                const char *m = expr->method_call.method_name;
                bool is_at_mut = strcmp(m, "at_mut") == 0;
                if (is_at_mut && !expr->method_call.is_mutable_call) {
                    mix_error(expr->loc,
                              "method '%s' is mutating; call it as `%s!(...)`",
                              m, m);
                } else if (is_at_mut) {
                    require_mutable_method_receiver(
                        sema, expr->method_call.object, m, expr->loc);
                } else if (is_mutating_builtin_method(obj_type, m) &&
                    !expr->method_call.is_mutable_call) {
                    mix_error(expr->loc,
                              "method '%s' is mutating; call it as `%s!(...)`",
                              m, m);
                } else if (is_mutating_builtin_method(obj_type, m)) {
                    require_mutable_method_receiver(
                        sema, expr->method_call.object, m, expr->loc);
                }
                if (strcmp(m, "push") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (strcmp(m, "pop") == 0) {
                    expr->resolved_type = obj_base->list.elem_type
                        ? obj_base->list.elem_type : make_type(sema->arena, TYPE_INT);
                } else if ((strcmp(m, "at") == 0 || strcmp(m, "at_mut") == 0) &&
                           expr->method_call.arg_count == 1) {
                    MixType *elem = obj_base->list.elem_type
                        ? obj_base->list.elem_type : make_type(sema->arena, TYPE_INT);
                    expr->resolved_type = make_ref_type(
                        sema->arena, elem, strcmp(m, "at_mut") == 0);
                } else if (strcmp(m, "remove") == 0 || strcmp(m, "insert") == 0 ||
                           strcmp(m, "sort") == 0 || strcmp(m, "reverse") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (strcmp(m, "slice") == 0) {
                    expr->resolved_type = obj_base;
                } else if (strcmp(m, "contains") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "index_of") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_INT);
                } else if (strcmp(m, "join") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_STR);
                } else {
                    mix_error(expr->loc, "list has no method '%s'", m);
                    mix_help(expr->loc, "check the method name; common list methods include `push!`, `pop`, `at`, `at_mut!`, and `contains`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // Map built-in methods
            if (obj_base && obj_base->kind == TYPE_MAP) {
                const char *m = expr->method_call.method_name;
                if (is_mutating_builtin_method(obj_type, m) &&
                    !expr->method_call.is_mutable_call) {
                    mix_error(expr->loc,
                              "method '%s' is mutating; call it as `%s!(...)`",
                              m, m);
                } else if (is_mutating_builtin_method(obj_type, m)) {
                    require_mutable_method_receiver(
                        sema, expr->method_call.object, m, expr->loc);
                }
                if (strcmp(m, "has") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "remove") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else {
                    mix_error(expr->loc, "map has no method '%s'", m);
                    mix_help(expr->loc, "map methods are `has` and `remove!`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // Set built-in methods
            if (obj_base && obj_base->kind == TYPE_SET) {
                const char *m = expr->method_call.method_name;
                if (is_mutating_builtin_method(obj_type, m) &&
                    !expr->method_call.is_mutable_call) {
                    mix_error(expr->loc,
                              "method '%s' is mutating; call it as `%s!(...)`",
                              m, m);
                } else if (is_mutating_builtin_method(obj_type, m)) {
                    require_mutable_method_receiver(
                        sema, expr->method_call.object, m, expr->loc);
                }
                if (strcmp(m, "has") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_BOOL);
                } else if (strcmp(m, "add") == 0 || strcmp(m, "remove") == 0) {
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                } else if (strcmp(m, "union") == 0 || strcmp(m, "intersect") == 0 ||
                           strcmp(m, "diff") == 0) {
                    expr->resolved_type = make_set_type(sema->arena, obj_base->set.elem_type);
                } else {
                    mix_error(expr->loc, "set has no method '%s'", m);
                    mix_help(expr->loc, "set methods include `has`, `add!`, `remove!`, `union`, `intersect`, and `diff`");
                    expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                }
                return expr->resolved_type;
            }
            // Look up ShapeName_methodName
            if (obj_base && obj_base->kind == TYPE_SHAPE) {
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                         obj_base->shape.name, expr->method_call.method_name);
                Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                if (msym && msym->type && msym->type->kind == TYPE_FUNC) {
                    if (msym->has_mutation && !expr->method_call.is_mutable_call) {
                        mix_error(expr->loc,
                                  "method '%s' is mutating; call it as `%s!(...)`",
                                  expr->method_call.method_name,
                                  expr->method_call.method_name);
                    } else if (msym->has_mutation) {
                        require_mutable_method_receiver(
                            sema, expr->method_call.object,
                            expr->method_call.method_name, expr->loc);
                    }
                    expr->resolved_type = msym->type->func.return_type;
                    // Check method argument types (param 0 is self, user args start at 1)
                    MixType *mtype = msym->type;
                    for (int i2 = 0; i2 < expr->method_call.arg_count &&
                         i2 + 1 < mtype->func.param_count; i2++) {
                        MixType *expected = mtype->func.param_types[i2 + 1];
                        MixType *actual = expr->method_call.args[i2]->resolved_type;
                        if (func_param_is_mutable(mtype, i2 + 1)) {
                            require_mutable_call_argument(
                                sema, expr->method_call.args[i2], expected,
                                expr->method_call.method_name, i2);
                            if (actual && actual->kind == TYPE_REF) {
                                actual = actual->ref.base;
                            }
                        }
                        if (!types_compatible(expected, actual)) {
                            mix_error(expr->method_call.args[i2]->loc,
                                      "argument %d of '%s.%s': expected %s, got %s",
                                      i2 + 1, obj_base->shape.name,
                                      expr->method_call.method_name,
                                      type_name(sema->arena, expected),
                                      type_name(sema->arena, actual));
                            mix_help(expr->method_call.args[i2]->loc,
                                     "change this argument or update '%s.%s' to accept %s",
                                     obj_base->shape.name,
                                     expr->method_call.method_name,
                                     type_name(sema->arena, actual));
                        }
                    }
                } else {
                    // No method by that name — fall back to a fn-typed field
                    // (e.g. `Timer.fn(t)` where `fn` is a callback field).
                    // Indirect call through the field; no `self` prepended.
                    // Field type may be TYPE_FUNC (real fn) or TYPE_INT
                    // (the placeholder MIX programs use today since there's
                    // no fn-type syntax in the parser yet) — both are
                    // callable, mirroring the lambda-variable indirect-call
                    // path which is similarly untyped.
                    ShapeFieldInfo *fi = type_find_field(obj_base, expr->method_call.method_name);
                    bool callable_field = fi && fi->type
                        && (fi->type->kind == TYPE_FUNC
                            || fi->type->kind == TYPE_INT);
                    if (callable_field) {
                        expr->method_call.is_field_call = true;
                        // For real fn types, use the declared return. For the
                        // `int` placeholder convention, default to int (matches
                        // NODE_CALL_EXPR's lambda-var fallback).
                        if (fi->type->kind == TYPE_FUNC && fi->type->func.return_type) {
                            expr->resolved_type = fi->type->func.return_type;
                        } else {
                            expr->resolved_type = make_type(sema->arena, TYPE_INT);
                        }
                    } else if (fi) {
                        mix_error(expr->loc,
                                  "field '%s.%s' is not callable (type %s)",
                                  obj_base->shape.name,
                                  expr->method_call.method_name,
                                  type_name(sema->arena, fi->type));
                        mix_help(expr->loc, "use `%s.%s` as a field value, or change the field type to a function",
                                 obj_base->shape.name, expr->method_call.method_name);
                        expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    } else {
                        mix_error(expr->loc, "shape '%s' has no method '%s'",
                                  obj_base->shape.name, expr->method_call.method_name);
                        mix_help(expr->loc, "add method '%s' to shape '%s' or check the method name",
                                 expr->method_call.method_name, obj_base->shape.name);
                        expr->resolved_type = make_type(sema->arena, TYPE_VOID);
                    }
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
            AstNode *call = expr->go_expr.call_expr;
            if (call && call->kind == NODE_CALL_EXPR) {
                Symbol *sym = symtab_lookup(&sema->symtab, call->call.name);
                if (sym && sym->type && sym->type->kind == TYPE_FUNC &&
                    func_has_mutable_params(sym->type)) {
                    mix_error(call->loc,
                              "cannot spawn '%s' with mutable parameter(s) via `go`",
                              call->call.name);
                    mix_help(call->loc,
                             "wrap the call in a non-mutating helper that captures the mutable args");
                }
            } else if (call && call->kind == NODE_METHOD_CALL) {
                MixType *obj_type = call->method_call.object
                    ? call->method_call.object->resolved_type : NULL;
                if (call->method_call.is_mutable_call) {
                    mix_error(call->loc,
                              "cannot spawn mutating method '%s!' via `go`",
                              call->method_call.method_name);
                    mix_help(call->loc,
                             "call the non-mutating version or wrap in a non-mutating helper");
                } else if (obj_type && obj_type->kind == TYPE_SHAPE) {
                    char mangled[256];
                    snprintf(mangled, sizeof(mangled), "%s_%s",
                             obj_type->shape.name, call->method_call.method_name);
                    Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                    if (msym && msym->type && msym->type->kind == TYPE_FUNC &&
                        func_has_mutable_params(msym->type)) {
                        mix_error(call->loc,
                                  "cannot spawn method '%s' with mutable parameter(s) via `go`",
                                  call->method_call.method_name);
                        mix_help(call->loc,
                                 "wrap the call in a non-mutating helper that captures the mutable args");
                    }
                }
            }
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
            // Parser produces VAR_DECL for any `name = expr`. If `name` is
            // already declared as mutable in an enclosing scope and this
            // VAR_DECL doesn't add a type annotation, it is really an
            // assignment — rewrite it. This makes `s! = 0; ...; s = s + 1`
            // work as expected and avoids spurious shadowing in both backends.
            // We also accept `s! = ...` when `s` already exists mutable
            // (the user's intent is mutation, not shadowing). This applies
            // to value shapes too: after the value-semantics shift, `text =
            // make_text(...)` must update the existing slot instead of
            // creating a fresh shadow local and leaving the old resource live.
            if (!stmt->var_decl.type_ann) {
                Symbol *existing = symtab_lookup(&sema->symtab, stmt->var_decl.name);
                if (existing && existing->is_mutable) {
                    AstNode *init = stmt->var_decl.init_expr;
                    char *name = stmt->var_decl.name;
                    stmt->kind = NODE_ASSIGN;
                    stmt->assign.name = name;
                    stmt->assign.op = TOK_EQ;
                    stmt->assign.value = init;
                    stmt->assign.target_is_global = false;
                    stmt->assign.target_is_stack_slot = false;
                    analyze_stmt(sema, stmt);
                    break;
                }
            }

            MixType *type;
            if (stmt->var_decl.type_ann) {
                type = resolve_type_node(sema, stmt->var_decl.type_ann);
            } else if (stmt->var_decl.init_expr) {
                type = resolve_expr(sema, stmt->var_decl.init_expr);
            } else {
                type = make_type(sema->arena, TYPE_VOID);
            }
            stmt->resolved_type = type;
            if (stmt->var_decl.is_global && type && type->kind == TYPE_REF) {
                mix_error(stmt->loc,
                          "global variable '%s' cannot have ref type",
                          stmt->var_decl.name);
                mix_help(stmt->loc, "store refs in local scope, or make the global hold the referenced value instead");
            }
            // When there's a type annotation, still resolve the init expression
            // so it gets type-checked and its resolved_type is set.
            if (stmt->var_decl.type_ann && stmt->var_decl.init_expr) {
                MixType *init_type = resolve_expr_hinted(sema,
                    stmt->var_decl.init_expr, type);
                if (!types_compatible(type, init_type)) {
                    mix_error(stmt->loc,
                              "cannot assign %s to variable '%s' of type %s",
                              type_name(sema->arena, init_type),
                              stmt->var_decl.name,
                              type_name(sema->arena, type));
                    mix_help(stmt->loc, "change the initializer or update '%s' to use type %s",
                             stmt->var_decl.name, type_name(sema->arena, init_type));
                }
            }
            symtab_insert(&sema->symtab, stmt->var_decl.name, type, stmt->var_decl.is_mutable);
            Symbol *new_sym = symtab_lookup_current(&sema->symtab, stmt->var_decl.name);
            // Plain shape locals are stack-backed value slots. NODE_IDENT
            // treats `%v.<name>` as the address of the local storage.
            if (type && type->kind == TYPE_SHAPE && !stmt->var_decl.is_global) {
                if (new_sym) new_sym->is_stack_slot = true;
            }
            break;
        }
        case NODE_ASSIGN: {
            // Inside a method body, a bare-name assignment whose name is a
            // field of the enclosing shape is sugar for `self.field op= val`.
            // Rewrite the AST node in place so the emitter sees a proper
            // field store. (Reads of bare field names are already handled
            // implicitly by the emitter's NODE_IDENT path.)
            if (sema->current_shape) {
                ShapeFieldInfo *fi = type_find_field(sema->current_shape, stmt->assign.name);
                if (fi) {
                    if (!sema->current_method_mutates) {
                        mix_error(stmt->loc,
                                  "cannot assign to field '%s' from a non-mutating method "
                                  "(declare the method with a trailing `!`)",
                                  stmt->assign.name);
                        mix_help(stmt->loc, "rename the method with a trailing `!` if it should mutate self");
                        break;
                    }
                    AstNode *self_ref = ast_new(sema->arena, NODE_IDENT, stmt->loc);
                    self_ref->ident.name = "self";
                    self_ref->ident.is_mutable = true;
                    self_ref->resolved_type = sema->current_shape;

                    char *field_name = stmt->assign.name;
                    TokenKind op = stmt->assign.op;
                    AstNode *value = stmt->assign.value;

                    stmt->kind = NODE_FIELD_ASSIGN;
                    stmt->field_assign.object = self_ref;
                    stmt->field_assign.field_name = field_name;
                    stmt->field_assign.value = value;
                    stmt->field_assign.op = op;
                    // Re-dispatch through the FIELD_ASSIGN branch below.
                    analyze_stmt(sema, stmt);
                    break;
                }
            }
            // Hint the value expression with the variable's declared type so
            // empty list/set/map literals retype to match the lvalue.
            Symbol *pre_sym = symtab_lookup(&sema->symtab, stmt->assign.name);
            MixType *hint = (pre_sym && pre_sym->type) ? pre_sym->type : NULL;
            MixType *val_type = resolve_expr_hinted(sema, stmt->assign.value, hint);
            Symbol *var_sym = pre_sym;
            stmt->resolved_type = var_sym && var_sym->type ? var_sym->type : val_type;
            stmt->assign.target_is_global = var_sym && var_sym->is_global;
            stmt->assign.target_is_stack_slot = var_sym && var_sym->is_stack_slot;
            if (var_sym && var_sym->type && val_type) {
                if (!var_sym->is_mutable) {
                    mix_error(stmt->loc, "cannot assign to immutable variable '%s'",
                              stmt->assign.name);
                    mix_help(stmt->loc, "declare it as `%s! = ...` if reassignment is intended",
                             stmt->assign.name);
                }
                if (stmt->assign.op == TOK_EQ && !types_compatible(var_sym->type, val_type)) {
                    mix_error(stmt->loc,
                              "cannot assign %s to '%s' of type %s",
                              type_name(sema->arena, val_type),
                              stmt->assign.name,
                              type_name(sema->arena, var_sym->type));
                    mix_help(stmt->loc, "change the assigned value or update '%s' to use type %s",
                             stmt->assign.name, type_name(sema->arena, val_type));
                }
            }
            break;
        }
        case NODE_FIELD_ASSIGN: {
            MixType *obj_type = resolve_expr(sema, stmt->field_assign.object);
            MixType *obj_base = unwrap_ref_type(obj_type);
            if (!obj_base) {
                resolve_expr(sema, stmt->field_assign.value);
                break;
            }
            if (obj_base->kind != TYPE_SHAPE) {
                resolve_expr(sema, stmt->field_assign.value);
                mix_error(stmt->loc, "cannot assign to field on non-shape type %s",
                          type_name(sema->arena, obj_type));
                mix_help(stmt->loc, "only shape values have assignable fields");
                break;
            }
            ShapeFieldInfo *fi = type_find_field(obj_base, stmt->field_assign.field_name);
            if (!fi) {
                resolve_expr(sema, stmt->field_assign.value);
                mix_error(stmt->loc, "shape '%s' has no field '%s'",
                          obj_base->shape.name, stmt->field_assign.field_name);
                mix_help(stmt->loc, "add field '%s' to shape '%s' or check the field name",
                         stmt->field_assign.field_name, obj_base->shape.name);
                break;
            }
            // Hint with the field's declared type so `b.sprites = []` retypes
            // the empty list literal to the field's [T] instead of [int].
            MixType *val_type = resolve_expr_hinted(sema,
                stmt->field_assign.value, fi->type);
            if (!expr_is_mutable_place(sema, stmt->field_assign.object)) {
                AstNode *root = method_receiver_root(stmt->field_assign.object);
                if (root && root->kind == NODE_IDENT) {
                    mix_error(stmt->loc,
                              "cannot assign to field '%s' through immutable '%s' "
                              "(declare it as `%s! = ...`)",
                              stmt->field_assign.field_name,
                              root->ident.name, root->ident.name);
                    mix_help(stmt->loc, "change the receiver declaration to `%s! = ...`", root->ident.name);
                } else {
                    mix_error(stmt->loc,
                              "cannot assign to field '%s' on a non-mutable temporary",
                              stmt->field_assign.field_name);
                    mix_help(stmt->loc, "store the receiver in a mutable variable before assigning the field");
                }
                break;
            }
            if (val_type && stmt->field_assign.op == TOK_EQ &&
                !types_compatible(fi->type, val_type)) {
                mix_error(stmt->loc,
                          "cannot assign %s to field '%s' of type %s",
                          type_name(sema->arena, val_type),
                          stmt->field_assign.field_name,
                          type_name(sema->arena, fi->type));
                mix_help(stmt->loc, "change the assigned value or update field '%s' to use type %s",
                         stmt->field_assign.field_name, type_name(sema->arena, val_type));
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
            if (stmt->for_stmt.var_is_mutable) {
                if (!iter_type || iter_type->kind != TYPE_LIST) {
                    mix_error(stmt->loc,
                              "mutable loop binding '%s!' is only supported when iterating a list",
                              stmt->for_stmt.var_name);
                    mix_help(stmt->loc,
                             "use a regular (immutable) loop binding `%s` or iterate over a list",
                             stmt->for_stmt.var_name);
                } else {
                    require_mutable_loop_iterable(
                        sema, stmt->for_stmt.iterable,
                        stmt->for_stmt.var_name, stmt->loc);
                }
            }
            // For range/list/map/set loops, push scope and bind the loop vars.
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
            symtab_insert(&sema->symtab, stmt->for_stmt.var_name,
                          var_type, stmt->for_stmt.var_is_mutable);
            // Loop vars are stack-backed locals. Shape loop vars hold an
            // inline copy of the current element, with mutable loops writing
            // back at the loop epilogue.
            Symbol *vsym = symtab_lookup_current(&sema->symtab,
                                                 stmt->for_stmt.var_name);
            if (vsym) {
                vsym->is_stack_slot = true;
            }
            if (stmt->for_stmt.index_name) {
                // For maps: index_name = key type; for lists: index_name = int
                MixType *idx_type = make_type(sema->arena, TYPE_INT);
                if (iter_type && iter_type->kind == TYPE_MAP) {
                    idx_type = iter_type->map.key_type;
                }
                symtab_insert(&sema->symtab, stmt->for_stmt.index_name,
                              idx_type, false);
                Symbol *isym = symtab_lookup_current(&sema->symtab,
                                                     stmt->for_stmt.index_name);
                if (isym) isym->is_stack_slot = true;
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
            bool is_optional = subj_type && subj_type->kind == TYPE_OPTIONAL;
            bool is_result = subj_type && subj_type->kind == TYPE_RESULT;

            for (int i2 = 0; i2 < stmt->match_stmt.arm_count; i2++) {
                struct MatchArm *arm_s = &stmt->match_stmt.arms[i2];
                AstNode *body = arm_s->body;
                AstNode *pat = arm_s->pattern;

                // some(v) / none / ok(v) / err(e) patterns bind a fresh name
                // visible inside the body. The pattern itself is not type-
                // checked as a regular expression (would resolve `some` as
                // an undefined identifier).
                bool is_capture_pat = false;
                if (pat) {
                    if (is_optional &&
                        ((pat->kind == NODE_CALL_EXPR && strcmp(pat->call.name, "some") == 0) ||
                         pat->kind == NODE_NONE_LIT))
                        is_capture_pat = true;
                    else if (is_result && pat->kind == NODE_CALL_EXPR &&
                             (strcmp(pat->call.name, "ok") == 0 ||
                              strcmp(pat->call.name, "err") == 0))
                        is_capture_pat = true;
                }

                if (is_capture_pat) {
                    symtab_push_scope(&sema->symtab);
                    if (pat->kind == NODE_CALL_EXPR && pat->call.arg_count > 0 &&
                        pat->call.args[0]->kind == NODE_IDENT) {
                        const char *bind_name = pat->call.args[0]->ident.name;
                        MixType *bind_type;
                        if (is_optional)
                            bind_type = subj_type->optional.inner;
                        else if (strcmp(pat->call.name, "ok") == 0)
                            bind_type = subj_type->result.ok_type;
                        else
                            // err carries the message string passed to fail.
                            bind_type = make_type(sema->arena, TYPE_STR);
                        symtab_insert(&sema->symtab, bind_name, bind_type, false);
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
                } else if (is_tagged && pat && pat->kind == NODE_CALL_EXPR) {
                    // Variant pattern: Circle(r) → add r to scope for the body
                    ShapeVariant *sv = type_find_variant(subj_type, pat->call.name);
                    symtab_push_scope(&sema->symtab);
                    if (sv) {
                        for (int k = 0; k < pat->call.arg_count && k < sv->field_count; k++) {
                            AstNode *binding = pat->call.args[k];
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
                    if (pat) resolve_expr(sema, pat);
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
            // Exhaustiveness check for tagged-union matches
            if (is_tagged && subj_type->shape.variant_count > 0) {
                bool has_wildcard = false;
                for (int wi = 0; wi < stmt->match_stmt.arm_count; wi++) {
                    if (stmt->match_stmt.arms[wi].is_wildcard) { has_wildcard = true; break; }
                }
                if (!has_wildcard) {
                    int vc = subj_type->shape.variant_count;
                    bool *covered = arena_alloc(sema->arena, sizeof(bool) * vc);
                    for (int vi = 0; vi < vc; vi++) covered[vi] = false;
                    for (int ai = 0; ai < stmt->match_stmt.arm_count; ai++) {
                        AstNode *pat = stmt->match_stmt.arms[ai].pattern;
                        const char *vname = NULL;
                        if (pat && pat->kind == NODE_CALL_EXPR) vname = pat->call.name;
                        else if (pat && pat->kind == NODE_IDENT) vname = pat->ident.name;
                        if (!vname) continue;
                        for (int vi = 0; vi < vc; vi++) {
                            if (strcmp(subj_type->shape.variants[vi].name, vname) == 0) {
                                covered[vi] = true;
                                break;
                            }
                        }
                    }
                    for (int vi = 0; vi < vc; vi++) {
                        if (!covered[vi]) {
                            mix_warning(stmt->loc,
                                "non-exhaustive match on '%s': variant '%s' not handled",
                                subj_type->shape.name,
                                subj_type->shape.variants[vi].name);
                            mix_note(stmt->loc,
                                "add a '%s' arm or '_' wildcard to silence this warning",
                                subj_type->shape.variants[vi].name);
                            break;
                        }
                    }
                }
            }
            // Exhaustiveness for optional / result subjects: both branches
            // (some+none / ok+err) must be present, or there must be a `_`.
            if (is_optional || is_result) {
                bool has_wildcard = false;
                bool saw_truthy = false;     // some / ok
                bool saw_falsy = false;      // none / err
                for (int ai = 0; ai < stmt->match_stmt.arm_count; ai++) {
                    struct MatchArm *a = &stmt->match_stmt.arms[ai];
                    if (a->is_wildcard) { has_wildcard = true; continue; }
                    AstNode *pat = a->pattern;
                    if (!pat) continue;
                    if (pat->kind == NODE_NONE_LIT) saw_falsy = true;
                    else if (pat->kind == NODE_CALL_EXPR) {
                        if (strcmp(pat->call.name, "some") == 0 ||
                            strcmp(pat->call.name, "ok") == 0)
                            saw_truthy = true;
                        else if (strcmp(pat->call.name, "err") == 0)
                            saw_falsy = true;
                    }
                }
                if (!has_wildcard && !(saw_truthy && saw_falsy)) {
                    const char *kind = is_optional ? "optional" : "result";
                    const char *missing = saw_truthy
                        ? (is_optional ? "none" : "err")
                        : (is_optional ? "some" : "ok");
                    mix_warning(stmt->loc,
                        "non-exhaustive match on %s: '%s' arm missing", kind, missing);
                    mix_note(stmt->loc,
                        "add a '%s%s' arm or '_' wildcard to silence this warning",
                        missing, strcmp(missing, "none") == 0 ? "" : "(...)");
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

// Validate that an initializer for a module-level mutable is something the
// emitters can lower into a static data section / file-scope C global.
// Accepts numeric/bool/string literals and unary minus on a numeric literal.
static bool is_global_init_const(AstNode *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_BOOL_LIT:
        case NODE_STRING_LIT:
            return true;
        case NODE_UNARY_EXPR:
            if (expr->unary.op == TOK_MINUS &&
                expr->unary.operand &&
                (expr->unary.operand->kind == NODE_INT_LIT ||
                 expr->unary.operand->kind == NODE_FLOAT_LIT))
                return true;
            return false;
        default:
            return false;
    }
}

// Register a module-level mutable. Adds to the shared symtab (with
// is_global/is_mutable set) and to `all_globals` so emitters can iterate.
static void register_global(Sema *sema, AstNode *decl) {
    if (!is_global_init_const(decl->var_decl.init_expr)) {
        mix_error(decl->loc,
                  "module-level mutable '%s' must be initialized with a literal "
                  "(int, float, bool, or string)", decl->var_decl.name);
        mix_help(decl->loc,
                 "assign a literal value like `0`, `true`, or `\"hello\"` as the initializer");
        return;
    }
    MixType *type = resolve_expr(sema, decl->var_decl.init_expr);
    decl->resolved_type = type;
    symtab_insert(&sema->symtab, decl->var_decl.name, type, true);
    Symbol *sym = symtab_lookup(&sema->symtab, decl->var_decl.name);
    if (sym) sym->is_global = true;
    if (sema->all_global_count >= sema->all_global_cap) {
        int new_cap = sema->all_global_cap ? sema->all_global_cap * 2 : 16;
        AstNode **na = arena_alloc(sema->arena, sizeof(AstNode*) * new_cap);
        if (sema->all_globals)
            memcpy(na, sema->all_globals, sizeof(AstNode*) * sema->all_global_count);
        sema->all_globals = na;
        sema->all_global_cap = new_cap;
    }
    sema->all_globals[sema->all_global_count++] = decl;
}

// Resolve and record a `@const` decl. Adds to the shared symtab and to the
// cross-module `all_consts` list that emitters consult for inlining.
static void register_const(Sema *sema, AstNode *decl) {
    MixType *ctype = resolve_expr(sema, decl->const_decl.value);
    symtab_insert(&sema->symtab, decl->const_decl.name, ctype, false);
    if (sema->all_const_count >= sema->all_const_cap) {
        int new_cap = sema->all_const_cap ? sema->all_const_cap * 2 : 32;
        AstNode **na = arena_alloc(sema->arena, sizeof(AstNode*) * new_cap);
        if (sema->all_consts)
            memcpy(na, sema->all_consts, sizeof(AstNode*) * sema->all_const_count);
        sema->all_consts = na;
        sema->all_const_cap = new_cap;
    }
    sema->all_consts[sema->all_const_count++] = decl;
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
    if (ret_type && ret_type->kind == TYPE_REF) {
        mix_error(fn->loc,
                  "function '%s' cannot return a ref type",
                  fn->fn_decl.name);
        mix_help(fn->loc,
                 "return the referenced value by value instead, or use a Box/Zone");
    }

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
        func_type->func.param_mutable = arena_alloc(sema->arena, sizeof(bool) * fn->fn_decl.param_count);
        func_type->func.param_defaults = arena_alloc(sema->arena, sizeof(void*) * fn->fn_decl.param_count);
        memset(func_type->func.param_mutable, 0, sizeof(bool) * fn->fn_decl.param_count);
        bool seen_default = false;
        for (int i = 0; i < fn->fn_decl.param_count; i++) {
            MixType *ptype = fn->fn_decl.params[i].type
                ? resolve_type_node(sema, fn->fn_decl.params[i].type)
                : make_type(sema->arena, TYPE_INFER);
            func_type->func.param_types[i] = ptype;
            func_type->func.param_mutable[i] = fn->fn_decl.params[i].is_mutable;
            func_type->func.param_defaults[i] = fn->fn_decl.params[i].default_value;
            // Defaults must form a trailing run — once a param has a
            // default, no later param may be required.
            if (fn->fn_decl.params[i].default_value) {
                if (fn->fn_decl.params[i].is_mutable) {
                    mix_error(fn->loc,
                        "mutable parameter '%s' of '%s' cannot have a default value",
                        fn->fn_decl.params[i].name, fn->fn_decl.name);
                    mix_help(fn->loc,
                             "remove the default value or the `!` from parameter '%s'",
                             fn->fn_decl.params[i].name);
                }
                seen_default = true;
            } else if (seen_default) {
                mix_error(fn->loc,
                    "parameter '%s' of '%s' must have a default — "
                    "non-default param after a defaulted one",
                    fn->fn_decl.params[i].name, fn->fn_decl.name);
                mix_help(fn->loc,
                         "reorder parameters so that defaulted ones come last, or add a default to '%s'",
                         fn->fn_decl.params[i].name);
            }
            // Annotate param type AST node
            if (fn->fn_decl.params[i].type) {
                fn->fn_decl.params[i].type->resolved_type = ptype;
            }
        }
    }

    // Carry generic constraints onto the function type so call sites can
    // enforce them without walking back to the AST.
    if (fn->fn_decl.constraint_count > 0) {
        func_type->func.constraints = fn->fn_decl.constraints;
        func_type->func.constraint_count = fn->fn_decl.constraint_count;
    }

    symtab_insert(&sema->symtab, fn->fn_decl.name, func_type, false);
    Symbol *fn_sym = symtab_lookup(&sema->symtab, fn->fn_decl.name);
    if (fn_sym) fn_sym->has_mutation = fn->fn_decl.has_mutation;

    // Restore generic context
    sema->generic_params = saved_gp;
    sema->generic_param_count = saved_gc;
}

static void register_extern_fn(Sema *sema, AstNode *fn) {
    MixType *func_type = make_type(sema->arena, TYPE_FUNC);
    func_type->func.return_type = fn->extern_fn_decl.return_type
        ? resolve_type_node(sema, fn->extern_fn_decl.return_type)
        : make_type(sema->arena, TYPE_VOID);
    if (func_type->func.return_type &&
        func_type->func.return_type->kind == TYPE_REF) {
        mix_error(fn->loc,
                  "extern function '%s' cannot return a ref type",
                  fn->extern_fn_decl.name);
        mix_help(fn->loc,
                 "return a pointer or value type instead; refs cannot cross the extern boundary");
    }

    func_type->func.param_count = fn->extern_fn_decl.param_count;
    if (fn->extern_fn_decl.param_count > 0) {
        func_type->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * fn->extern_fn_decl.param_count);
        func_type->func.param_mutable = arena_alloc(sema->arena, sizeof(bool) * fn->extern_fn_decl.param_count);
        memset(func_type->func.param_mutable, 0, sizeof(bool) * fn->extern_fn_decl.param_count);
        for (int i = 0; i < fn->extern_fn_decl.param_count; i++) {
            func_type->func.param_types[i] = fn->extern_fn_decl.params[i].type
                ? resolve_type_node(sema, fn->extern_fn_decl.params[i].type)
                : make_type(sema->arena, TYPE_INFER);
            func_type->func.param_mutable[i] = fn->extern_fn_decl.params[i].is_mutable;
        }
    }

    symtab_insert(&sema->symtab, fn->extern_fn_decl.name, func_type, false);
    Symbol *ext_sym = symtab_lookup(&sema->symtab, fn->extern_fn_decl.name);
    if (ext_sym) {
        ext_sym->has_mutation = fn->extern_fn_decl.has_mutation;
        ext_sym->is_extern = true;
    }

    // Store optional C symbol alias
    if (fn->extern_fn_decl.c_name) {
        Symbol *sym = ext_sym ? ext_sym
                              : symtab_lookup(&sema->symtab, fn->extern_fn_decl.name);
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

    // panic(msg: str) -> void  (abort with message)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "panic", ft, false);
    }

    // assert(cond: bool, msg: str) -> void  (abort if condition false)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_BOOL);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_STR);
        symtab_insert(&sema->symtab, "assert", ft, false);
    }

    // len(x) -> int  (polymorphic over str/list/map/set)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
        symtab_insert(&sema->symtab, "len", ft, false);
    }

    // type_of(x) -> str  (returns type name as string)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
        symtab_insert(&sema->symtab, "type_of", ft, false);
    }

    // sizeof(x) -> int  (size of value's type in bytes)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
        symtab_insert(&sema->symtab, "sizeof", ft, false);
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

        // peek_u32(ptr: *byte, offset: int) -> int
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "peek_u32", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // peek_byte(ptr: *byte, offset: int) -> int
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "peek_byte", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // peek_f32(ptr: *byte, offset: int) -> float
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_FLOAT);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "peek_f32", ft, false);
    }
    {
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        // memcpy(dst: *byte, src: *byte, n: int) ~
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 3;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 3);
        ft->func.param_types[0] = ptr_byte;
        ft->func.param_types[1] = ptr_byte;
        ft->func.param_types[2] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "memcpy", ft, false);
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
    {
        MixType *zone_type = make_type(sema->arena, TYPE_ZONE);
        MixType *ptr_byte = make_ptr_type(sema->arena, make_type(sema->arena, TYPE_BYTE));

        symtab_insert(&sema->symtab, "Zone", zone_type, false);

        // zone_create(name: str, capacity_hint: int) -> Zone
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = zone_type;
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_STR);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "zone_create", ft, false);

        // zone_destroy(zone: Zone) -> void
        ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = zone_type;
        symtab_insert(&sema->symtab, "zone_destroy", ft, false);

        // zone_reset(zone: Zone) -> void
        ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = zone_type;
        symtab_insert(&sema->symtab, "zone_reset", ft, false);

        // zone_alloc(zone: Zone, size: int) -> *byte
        ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = ptr_byte;
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = zone_type;
        ft->func.param_types[1] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "zone_alloc", ft, false);

        // _mix_zone_alloc_bytes(zone: Zone) -> int
        ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = zone_type;
        symtab_insert(&sema->symtab, "_mix_zone_alloc_bytes", ft, false);

        // _mix_zone_high_water(zone: Zone) -> int
        ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = zone_type;
        symtab_insert(&sema->symtab, "_mix_zone_high_water", ft, false);

        // _mix_zone_reset_count(zone: Zone) -> int
        ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = zone_type;
        symtab_insert(&sema->symtab, "_mix_zone_reset_count", ft, false);
    }

    // random_seed(seed: int) -> void
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_VOID);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "random_seed", ft, false);
    }
    // random_int() -> int
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 0;
        symtab_insert(&sema->symtab, "random_int", ft, false);
    }
    // random_float() -> float (in [0.0, 1.0])
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_FLOAT);
        ft->func.param_count = 0;
        symtab_insert(&sema->symtab, "random_float", ft, false);
    }
    // time_now_ms() -> int  (milliseconds since epoch)
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_INT);
        ft->func.param_count = 0;
        symtab_insert(&sema->symtab, "time_now_ms", ft, false);
    }
    // int_to_hex(n: int) -> str
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "int_to_hex", ft, false);
    }
    // int_to_bin(n: int) -> str
    {
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_STR);
        ft->func.param_count = 1;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*));
        ft->func.param_types[0] = make_type(sema->arena, TYPE_INT);
        symtab_insert(&sema->symtab, "int_to_bin", ft, false);
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
        // pow: (float, float) -> float
        MixType *ft = make_type(sema->arena, TYPE_FUNC);
        ft->func.return_type = make_type(sema->arena, TYPE_FLOAT);
        ft->func.param_count = 2;
        ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
        ft->func.param_types[0] = make_type(sema->arena, TYPE_FLOAT);
        ft->func.param_types[1] = make_type(sema->arena, TYPE_FLOAT);
        symtab_insert(&sema->symtab, "pow", ft, false);
    }
    {
        // min, max: (infer, infer) -> infer (supports both 2-arg and list-arg forms)
        const char *minmax[] = {"min", "max"};
        for (int mi = 0; mi < 2; mi++) {
            MixType *ft = make_type(sema->arena, TYPE_FUNC);
            ft->func.return_type = make_type(sema->arena, TYPE_INFER);
            ft->func.param_count = 2;
            ft->func.param_types = arena_alloc(sema->arena, sizeof(MixType*) * 2);
            ft->func.param_types[0] = make_type(sema->arena, TYPE_INFER);
            ft->func.param_types[1] = make_type(sema->arena, TYPE_INFER);
            symtab_insert(&sema->symtab, minmax[mi], ft, false);
        }
    }

    // First pass (a): register all shapes and type aliases first
    // (so extern blocks can reference shape types regardless of source order)
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_SHAPE_DECL) {
            // Generic shape with type params: register as a template, not
            // a concrete shape. The template materializes per use site
            // via instantiate_generic_shape().
            if (decl->shape_decl.type_param_count > 0) {
                register_template(sema, decl);
                continue;
            }

            // Build shape type
            MixType *shape_type = make_type(sema->arena, TYPE_SHAPE);
            shape_type->shape.name = decl->shape_decl.name;
            shape_type->shape.field_count = decl->shape_decl.field_count;
            shape_type->shape.fields = arena_alloc(sema->arena,
                sizeof(ShapeFieldInfo) * decl->shape_decl.field_count);

            // Make this shape's type params visible to resolve_type_node
            // for both fields and method signatures registered below.
            char **saved_gp = sema->generic_params;
            int saved_gc = sema->generic_param_count;
            if (decl->shape_decl.type_param_count > 0) {
                sema->generic_params = decl->shape_decl.type_params;
                sema->generic_param_count = decl->shape_decl.type_param_count;
            }

            int offset = 0;
            bool is_union = decl->shape_decl.is_union;
            int max_field_size = 0;
            int max_align = 1;
            for (int j = 0; j < decl->shape_decl.field_count; j++) {
                ShapeField *sf = &decl->shape_decl.fields[j];
                MixType *ftype = sf->type ? resolve_type_node(sema, sf->type)
                                          : make_type(sema->arena, TYPE_INT);
                if (ftype && ftype->kind == TYPE_REF) {
                    mix_error(sf->type ? sf->type->loc : decl->loc,
                              "shape field '%s.%s' cannot have ref type",
                              decl->shape_decl.name, sf->name);
                    mix_help(sf->type ? sf->type->loc : decl->loc,
                             "store the referenced value directly in the shape instead of using ref");
                }
                // Annotate AST node
                if (sf->type) sf->type->resolved_type = ftype;
                int fsize = type_size(ftype);
                int falign = type_alignment(ftype);
                if (falign > max_align) max_align = falign;

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
            // Final alignment: pad to the struct's natural alignment so
            // arrays of this shape and embedded fields lay out like C structs.
            if (is_union) {
                shape_type->shape.total_size = (max_field_size + max_align - 1) & ~(max_align - 1);
                shape_type->shape.is_union = true;
            } else {
                shape_type->shape.total_size = (offset + max_align - 1) & ~(max_align - 1);
            }
            shape_type->shape.alignment = max_align;
            // Annotate the AST node so downstream tools (e.g. the LSP outline)
            // can read the shape's MixType without re-doing the symbol lookup.
            decl->resolved_type = shape_type;

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
                mtype->func.param_mutable = arena_alloc(sema->arena,
                    sizeof(bool) * mtype->func.param_count);
                memset(mtype->func.param_mutable, 0,
                       sizeof(bool) * mtype->func.param_count);
                mtype->func.param_mutable[0] = method->fn_decl.has_mutation;
                mtype->func.param_types[0] = shape_type; // self
                for (int k = 0; k < method->fn_decl.param_count; k++) {
                    MixType *pt = method->fn_decl.params[k].type
                        ? resolve_type_node(sema, method->fn_decl.params[k].type)
                        : make_type(sema->arena, TYPE_INFER);
                    if (method->fn_decl.params[k].default_value &&
                        method->fn_decl.params[k].is_mutable) {
                        mix_error(method->loc,
                            "mutable parameter '%s' of '%s.%s' cannot have a default value",
                            method->fn_decl.params[k].name,
                            decl->shape_decl.name, method->fn_decl.name);
                        mix_help(method->loc,
                                 "remove the default value or the `!` from parameter '%s'",
                                 method->fn_decl.params[k].name);
                    }
                    if (method->fn_decl.params[k].type)
                        method->fn_decl.params[k].type->resolved_type = pt;
                    mtype->func.param_types[k + 1] = pt;
                    mtype->func.param_mutable[k + 1] =
                        method->fn_decl.params[k].is_mutable;
                }
                symtab_insert(&sema->symtab, mangled, mtype, false);
                Symbol *msym = symtab_lookup(&sema->symtab, mangled);
                if (msym) msym->has_mutation = method->fn_decl.has_mutation;
            }

            // Restore generic context after the shape is fully registered
            sema->generic_params = saved_gp;
            sema->generic_param_count = saved_gc;
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
            register_const(sema, decl);
        } else if (decl->kind == NODE_VAR_DECL && decl->var_decl.is_global) {
            register_global(sema, decl);
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
                    register_const(sema, cd);
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
                insert_param(&sema->symtab, param, ptype);
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
            // Generic shape templates have no methods to analyze (their
            // bodies are analyzed per instantiation in pass-c below).
            if (decl->shape_decl.type_param_count > 0) continue;

            // Analyze method bodies
            Symbol *shape_sym = symtab_lookup(&sema->symtab, decl->shape_decl.name);
            MixType *shape_type = shape_sym ? shape_sym->type : NULL;

            // Make shape's type params visible inside method bodies
            char **saved_gp = sema->generic_params;
            int saved_gc = sema->generic_param_count;
            if (decl->shape_decl.type_param_count > 0) {
                sema->generic_params = decl->shape_decl.type_params;
                sema->generic_param_count = decl->shape_decl.type_param_count;
            }

            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                AstNode *method = decl->shape_decl.methods[j];
                symtab_push_scope(&sema->symtab);

                MixType *saved_shape = sema->current_shape;
                bool saved_mutates = sema->current_method_mutates;
                sema->current_shape = shape_type;
                sema->current_method_mutates = method->fn_decl.has_mutation;

                // Insert 'self' as an implicit parameter. When the method is
                // marked mutating (postfix `!`), self is mutable so that
                // `self.field = val` and `self.field! += val` type-check.
                if (shape_type) {
                    symtab_insert(&sema->symtab, "self", shape_type,
                                  method->fn_decl.has_mutation);
                    // Insert shape fields as variables accessible by name.
                    // Mark them mutable inside mutating methods so that
                    // `radius! += amount` (rewritten to `self.radius += amount`)
                    // passes the assignment check.
                    for (int k = 0; k < shape_type->shape.field_count; k++) {
                        symtab_insert(&sema->symtab, shape_type->shape.fields[k].name,
                                      shape_type->shape.fields[k].type,
                                      method->fn_decl.has_mutation);
                    }
                }

                // Insert explicit method parameters
                for (int k = 0; k < method->fn_decl.param_count; k++) {
                    Param *param = &method->fn_decl.params[k];
                    MixType *ptype = param->type
                        ? resolve_type_node(sema, param->type)
                        : make_type(sema->arena, TYPE_INFER);
                    insert_param(&sema->symtab, param, ptype);
                }

                if (method->fn_decl.body) {
                    AstNode *body = method->fn_decl.body;
                    for (int k = 0; k < body->block.stmt_count; k++) {
                        analyze_stmt(sema, body->block.stmts[k]);
                    }
                }

                sema->current_shape = saved_shape;
                sema->current_method_mutates = saved_mutates;
                symtab_pop_scope(&sema->symtab);
            }

            sema->generic_params = saved_gp;
            sema->generic_param_count = saved_gc;
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
                        insert_param(&sema->symtab, param, ptype);
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

    // Pass (c): analyze method bodies of instantiated generic shapes.
    // Instantiations created during pass-b (or here, transitively) get
    // their method bodies type-checked with the concrete fields visible.
    // We loop because analyzing one instantiation can trigger another.
    int processed = 0;
    while (processed < sema->instantiated_decl_count) {
        int batch_end = sema->instantiated_decl_count;
        for (int idx = processed; idx < batch_end; idx++) {
            AstNode *decl = sema->instantiated_decls[idx];
            Symbol *shape_sym = symtab_lookup(&sema->symtab, decl->shape_decl.name);
            MixType *shape_type = shape_sym ? shape_sym->type : NULL;
            for (int j = 0; j < decl->shape_decl.method_count; j++) {
                AstNode *method = decl->shape_decl.methods[j];
                symtab_push_scope(&sema->symtab);
                MixType *saved_shape = sema->current_shape;
                bool saved_mutates = sema->current_method_mutates;
                sema->current_shape = shape_type;
                sema->current_method_mutates = method->fn_decl.has_mutation;
                if (shape_type) {
                    symtab_insert(&sema->symtab, "self", shape_type,
                                  method->fn_decl.has_mutation);
                    for (int k = 0; k < shape_type->shape.field_count; k++) {
                        symtab_insert(&sema->symtab, shape_type->shape.fields[k].name,
                                      shape_type->shape.fields[k].type,
                                      method->fn_decl.has_mutation);
                    }
                }
                for (int k = 0; k < method->fn_decl.param_count; k++) {
                    Param *param = &method->fn_decl.params[k];
                    MixType *ptype = param->type
                        ? resolve_type_node(sema, param->type)
                        : make_type(sema->arena, TYPE_INFER);
                    insert_param(&sema->symtab, param, ptype);
                }
                if (method->fn_decl.body) {
                    AstNode *body = method->fn_decl.body;
                    for (int k = 0; k < body->block.stmt_count; k++)
                        analyze_stmt(sema, body->block.stmts[k]);
                }
                sema->current_shape = saved_shape;
                sema->current_method_mutates = saved_mutates;
                symtab_pop_scope(&sema->symtab);
            }
        }
        processed = batch_end;
    }

    // Pass (d): splice the instantiated shape decls into the program AST
    // so the codegen sees them like ordinary user-written shapes.
    if (sema->instantiated_decl_count > 0) {
        int old_n = program->program.decl_count;
        int new_n = old_n + sema->instantiated_decl_count;
        AstNode **merged = arena_alloc(sema->arena, sizeof(AstNode*) * new_n);
        // Place instantiated shapes BEFORE other decls so codegen sees
        // them when emitting later use sites.
        for (int i = 0; i < sema->instantiated_decl_count; i++)
            merged[i] = sema->instantiated_decls[i];
        memcpy(merged + sema->instantiated_decl_count,
               program->program.decls, sizeof(AstNode*) * old_n);
        program->program.decls = merged;
        program->program.decl_count = new_n;
    }

    // Module-level mutable globals are NOT spliced. Each owning module
    // emits its storage exactly once; consumers reference the symbol via
    // extern (C backend) or by-name link (LLVM backend).

    // Pass (e): splice cross-module `@const` decls into this program.
    // The emitter inlines const values at use sites; without this, an
    // importer's emitter would never see consts defined in sub-modules.
    // Skip ones already present (by AST pointer) — that's the case for
    // the current program's own consts, which are already in decls.
    if (sema->all_const_count > 0) {
        int old_n = program->program.decl_count;
        int extra = 0;
        for (int i = 0; i < sema->all_const_count; i++) {
            bool present = false;
            for (int j = 0; j < old_n; j++) {
                if (program->program.decls[j] == sema->all_consts[i]) {
                    present = true;
                    break;
                }
            }
            if (!present) extra++;
        }
        if (extra > 0) {
            int new_n = old_n + extra;
            AstNode **merged = arena_alloc(sema->arena, sizeof(AstNode*) * new_n);
            int k = 0;
            for (int i = 0; i < sema->all_const_count; i++) {
                bool present = false;
                for (int j = 0; j < old_n; j++) {
                    if (program->program.decls[j] == sema->all_consts[i]) {
                        present = true;
                        break;
                    }
                }
                if (!present) merged[k++] = sema->all_consts[i];
            }
            memcpy(merged + extra, program->program.decls, sizeof(AstNode*) * old_n);
            program->program.decls = merged;
            program->program.decl_count = new_n;
        }
    }
}
