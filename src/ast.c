#include "ast.h"
#include "arena.h"
#include <inttypes.h>

AstNode *ast_new(Arena *a, NodeKind kind, SrcLoc loc) {
    AstNode *node = arena_alloc(a, sizeof(AstNode));
    memset(node, 0, sizeof(AstNode));
    node->kind = kind;
    node->loc = loc;
    return node;
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void ast_print(AstNode *node, int indent) {
    if (!node) { print_indent(indent); printf("(null)\n"); return; }

    print_indent(indent);
    switch (node->kind) {
        case NODE_PROGRAM:
            printf("Program\n");
            for (int i = 0; i < node->program.decl_count; i++)
                ast_print(node->program.decls[i], indent + 1);
            break;
        case NODE_FN_DECL:
            printf("%sFnDecl: %s", node->fn_decl.is_pub ? "pub " : "", node->fn_decl.name);
            if (node->fn_decl.has_side_effects) printf(" ~");
            if (node->fn_decl.has_mutation) printf(" !");
            printf(" (%d params)\n", node->fn_decl.param_count);
            if (node->fn_decl.return_type) ast_print(node->fn_decl.return_type, indent + 1);
            if (node->fn_decl.body) ast_print(node->fn_decl.body, indent + 1);
            break;
        case NODE_EXTERN_BLOCK:
            printf("ExternBlock: \"%s\"\n", node->extern_block.lib_name);
            for (int i = 0; i < node->extern_block.decl_count; i++)
                ast_print(node->extern_block.decls[i], indent + 1);
            break;
        case NODE_EXTERN_FN_DECL:
            printf("ExternFnDecl: %s", node->extern_fn_decl.name);
            if (node->extern_fn_decl.has_side_effects) printf(" ~");
            if (node->extern_fn_decl.has_mutation) printf(" !");
            printf(" (%d params)\n", node->extern_fn_decl.param_count);
            break;
        case NODE_USE_DECL:
            printf("UseDecl: %s", node->use_decl.module_path);
            if (node->use_decl.alias) printf(" (alias: %s)", node->use_decl.alias);
            if (node->use_decl.import_count > 0) {
                printf(" imports:");
                for (int i = 0; i < node->use_decl.import_count; i++)
                    printf(" %s", node->use_decl.imports[i]);
            }
            printf("\n");
            break;
        case NODE_USE_C_DECL:
            printf("UseCDecl: \"%s\"", node->use_c_decl.header_path);
            if (node->use_c_decl.lib_name)
                printf(" link \"%s\"", node->use_c_decl.lib_name);
            if (node->use_c_decl.frameworks)
                printf(" frameworks \"%s\"", node->use_c_decl.frameworks);
            printf("\n");
            break;
        case NODE_BLOCK:
            printf("Block (%d stmts)\n", node->block.stmt_count);
            for (int i = 0; i < node->block.stmt_count; i++)
                ast_print(node->block.stmts[i], indent + 1);
            break;
        case NODE_VAR_DECL:
            printf("VarDecl: %s%s\n", node->var_decl.name, node->var_decl.is_mutable ? "!" : "");
            if (node->var_decl.type_ann) ast_print(node->var_decl.type_ann, indent + 1);
            if (node->var_decl.init_expr) ast_print(node->var_decl.init_expr, indent + 1);
            break;
        case NODE_ASSIGN:
            printf("Assign: %s (op=%s)\n", node->assign.name, token_kind_name(node->assign.op));
            ast_print(node->assign.value, indent + 1);
            break;
        case NODE_IF_STMT:
            printf("If\n");
            ast_print(node->if_stmt.condition, indent + 1);
            ast_print(node->if_stmt.then_block, indent + 1);
            if (node->if_stmt.else_block) ast_print(node->if_stmt.else_block, indent + 1);
            break;
        case NODE_WHILE_STMT:
            printf("While\n");
            ast_print(node->while_stmt.condition, indent + 1);
            ast_print(node->while_stmt.body, indent + 1);
            break;
        case NODE_FOR_STMT:
            printf("For: %s%s", node->for_stmt.var_name,
                   node->for_stmt.var_is_mutable ? "!" : "");
            if (node->for_stmt.index_name) printf(", %s", node->for_stmt.index_name);
            printf("\n");
            ast_print(node->for_stmt.iterable, indent + 1);
            ast_print(node->for_stmt.body, indent + 1);
            break;
        case NODE_MATCH_STMT:
            printf("Match (%d arms)\n", node->match_stmt.arm_count);
            ast_print(node->match_stmt.subject, indent + 1);
            for (int i = 0; i < node->match_stmt.arm_count; i++) {
                print_indent(indent + 1);
                if (node->match_stmt.arms[i].is_wildcard)
                    printf("_ =>\n");
                else {
                    printf("pattern =>\n");
                    ast_print(node->match_stmt.arms[i].pattern, indent + 2);
                }
                ast_print(node->match_stmt.arms[i].body, indent + 2);
            }
            break;
        case NODE_DONE_STMT:
            printf("Done\n");
            if (node->done_stmt.value) ast_print(node->done_stmt.value, indent + 1);
            break;
        case NODE_BREAK_STMT:
            printf("Break\n"); break;
        case NODE_CONTINUE_STMT:
            printf("Continue\n"); break;
        case NODE_EXPR_STMT:
            printf("ExprStmt\n");
            ast_print(node->expr_stmt.expr, indent + 1);
            break;
        case NODE_DEFER_STMT:
            printf("Defer\n");
            ast_print(node->defer_stmt.stmt, indent + 1);
            break;
        case NODE_UNSAFE_BLOCK:
            printf("UnsafeBlock\n");
            ast_print(node->unsafe_block.body, indent + 1);
            break;
        case NODE_ZONE_STMT:
            printf("Zone%s%s\n", node->zone_stmt.name ? ":" : "", node->zone_stmt.name ? node->zone_stmt.name : "");
            ast_print(node->zone_stmt.body, indent + 1);
            break;
        case NODE_DEREF_ASSIGN:
            printf("DerefAssign\n");
            ast_print(node->deref_assign.ptr_expr, indent + 1);
            ast_print(node->deref_assign.value, indent + 1);
            break;
        case NODE_FAIL_STMT:
            printf("Fail\n");
            ast_print(node->fail_stmt.value, indent + 1);
            break;
        case NODE_CONST_DECL:
            printf("%sConstDecl: %s\n", node->const_decl.is_pub ? "pub " : "", node->const_decl.name);
            ast_print(node->const_decl.value, indent + 1);
            break;
        case NODE_INT_LIT:
            printf("IntLit: %" PRId64 "\n", node->int_lit.value); break;
        case NODE_FLOAT_LIT:
            printf("FloatLit: %g\n", node->float_lit.value); break;
        case NODE_STRING_LIT:
            printf("StringLit: \"%s\"\n", node->string_lit.value); break;
        case NODE_STRING_INTERP:
            printf("StringInterp (%d exprs)\n", node->string_interp.expr_count);
            for (int i = 0; i <= node->string_interp.expr_count; i++) {
                print_indent(indent + 1);
                printf("part: \"%s\"\n", node->string_interp.parts[i]);
                if (i < node->string_interp.expr_count)
                    ast_print(node->string_interp.exprs[i], indent + 1);
            }
            break;
        case NODE_BOOL_LIT:
            printf("BoolLit: %s\n", node->bool_lit.value ? "true" : "false"); break;
        case NODE_NONE_LIT:
            printf("NoneLit\n"); break;
        case NODE_IDENT:
            printf("Ident: %s", node->ident.name);
            if (node->ident.type_arg_count > 0) {
                printf("[");
                for (int i = 0; i < node->ident.type_arg_count; i++) {
                    if (i > 0) printf(", ");
                    if (node->ident.type_args[i] &&
                        node->ident.type_args[i]->kind == NODE_TYPE_NAME) {
                        printf("%s", node->ident.type_args[i]->type_name.name);
                    } else {
                        printf("type");
                    }
                }
                printf("]");
            }
            printf("%s\n", node->ident.is_mutable ? "!" : "");
            break;
        case NODE_BINARY_EXPR:
            printf("BinaryExpr: %s\n", token_kind_name(node->binary.op));
            ast_print(node->binary.left, indent + 1);
            ast_print(node->binary.right, indent + 1);
            break;
        case NODE_UNARY_EXPR:
            printf("UnaryExpr: %s\n", token_kind_name(node->unary.op));
            ast_print(node->unary.operand, indent + 1);
            break;
        case NODE_CALL_EXPR:
            printf("CallExpr: %s%s (%d args)\n",
                   node->call.name, node->call.is_mutable_call ? "!" : "",
                   node->call.arg_count);
            for (int i = 0; i < node->call.arg_count; i++)
                ast_print(node->call.args[i], indent + 1);
            break;
        case NODE_LAMBDA:
            printf("Lambda (%d params)\n", node->lambda.param_count);
            for (int i = 0; i < node->lambda.param_count; i++) {
                print_indent(indent + 1);
                printf("param: %s\n", node->lambda.param_names[i]);
            }
            ast_print(node->lambda.body, indent + 1);
            break;
        case NODE_LIST_LIT:
            printf("ListLit (%d elements)\n", node->list_lit.element_count);
            for (int i = 0; i < node->list_lit.element_count; i++)
                ast_print(node->list_lit.elements[i], indent + 1);
            break;
        case NODE_MAP_LIT:
            printf("MapLit (%d entries)\n", node->map_lit.entry_count);
            break;
        case NODE_SET_LIT:
            printf("SetLit (%d elements)\n", node->set_lit.element_count);
            for (int i = 0; i < node->set_lit.element_count; i++)
                ast_print(node->set_lit.elements[i], indent + 1);
            break;
        case NODE_CAST_EXPR:
            printf("Cast\n");
            ast_print(node->cast_expr.value, indent + 1);
            break;
        case NODE_INDEX_EXPR:
            printf("IndexExpr\n");
            ast_print(node->index_expr.object, indent + 1);
            print_indent(indent + 1); printf("[index]\n");
            ast_print(node->index_expr.index, indent + 2);
            break;
        case NODE_INDEX_ASSIGN:
            printf("IndexAssign\n");
            break;
        case NODE_SLICE_EXPR:
            printf("SliceExpr\n");
            break;
        case NODE_LIST_COMP:
            printf("ListComp\n");
            break;
        case NODE_COND_DECL:
            printf("CondDecl @%s\n", node->cond_decl.condition_name);
            break;
        case NODE_ELSE_EXPR:
            printf("ElseExpr\n");
            ast_print(node->else_expr.value, indent + 1);
            print_indent(indent + 1); printf("else:\n");
            ast_print(node->else_expr.fallback, indent + 2);
            break;
        case NODE_TYPE_ALIAS:
            printf("%sTypeAlias: %s\n", node->type_alias.is_pub ? "pub " : "", node->type_alias.name);
            ast_print(node->type_alias.target_type, indent + 1);
            break;
        case NODE_SHAPE_DECL:
            printf("ShapeDecl: %s (%d fields, %d methods)\n",
                   node->shape_decl.name, node->shape_decl.field_count,
                   node->shape_decl.method_count);
            for (int i = 0; i < node->shape_decl.field_count; i++) {
                print_indent(indent + 1);
                printf("Field: %s\n", node->shape_decl.fields[i].name);
                if (node->shape_decl.fields[i].type)
                    ast_print(node->shape_decl.fields[i].type, indent + 2);
            }
            for (int i = 0; i < node->shape_decl.method_count; i++)
                ast_print(node->shape_decl.methods[i], indent + 1);
            break;
        case NODE_FIELD_EXPR:
            printf("FieldExpr: .%s\n", node->field_expr.field_name);
            ast_print(node->field_expr.object, indent + 1);
            break;
        case NODE_METHOD_CALL:
            printf("MethodCall: .%s%s(%d args)\n", node->method_call.method_name,
                   node->method_call.is_mutable_call ? "!" : "",
                   node->method_call.arg_count);
            ast_print(node->method_call.object, indent + 1);
            for (int i = 0; i < node->method_call.arg_count; i++)
                ast_print(node->method_call.args[i], indent + 1);
            break;
        case NODE_SHAPE_LIT:
            printf("ShapeLit: %s (%d fields)\n", node->shape_lit.shape_name,
                   node->shape_lit.field_count);
            for (int i = 0; i < node->shape_lit.field_count; i++) {
                print_indent(indent + 1);
                printf("%s:\n", node->shape_lit.field_names[i]);
                ast_print(node->shape_lit.field_values[i], indent + 2);
            }
            break;
        case NODE_FIELD_ASSIGN:
            printf("FieldAssign: .%s (op=%s)\n", node->field_assign.field_name,
                   token_kind_name(node->field_assign.op));
            ast_print(node->field_assign.object, indent + 1);
            ast_print(node->field_assign.value, indent + 1);
            break;
        case NODE_TYPE_NAME:
            printf("TypeName: %s\n", node->type_name.name); break;
        case NODE_TYPE_PTR:
            printf("TypePtr\n");
            ast_print(node->type_ptr.base_type, indent + 1);
            break;
        case NODE_TYPE_REF:
            printf("TypeRef%s\n", node->type_ref.is_mutable ? "!" : "");
            ast_print(node->type_ref.base_type, indent + 1);
            break;
        case NODE_TYPE_OPTIONAL:
            printf("TypeOptional\n");
            ast_print(node->type_optional.inner_type, indent + 1);
            break;
        case NODE_SHARED_EXPR:
            printf("SharedExpr\n");
            ast_print(node->shared_expr.init_expr, indent + 1);
            break;
        case NODE_GO_EXPR:
            printf("GoExpr\n");
            ast_print(node->go_expr.call_expr, indent + 1);
            break;
        case NODE_WAIT_EXPR:
            printf("WaitExpr\n");
            ast_print(node->wait_expr.handle_expr, indent + 1);
            break;
        case NODE_TRY_EXPR:
            printf("TryExpr\n");
            ast_print(node->try_expr.expr, indent + 1);
            break;
    }
}

// --- Deep clone (for generic shape monomorphization) ---
//
// Walks the node tree and produces an independent copy in `arena`. Every
// NODE_TYPE_NAME whose name matches an entry in `bindings` is replaced by
// a fresh clone of the binding's type node. The substitution applies
// recursively into method bodies, list/map literals, etc. so a function
// like `push!(val: T) ~ items.push!(val)` re-resolves cleanly per
// instantiation.
//
// Strings and small arrays are duplicated into `arena`; nothing in the
// returned tree shares storage with the input.

static AstNode *do_clone(AstNode *node, Arena *arena,
                         TypeBinding *bindings, int binding_count);

static char *clone_str(Arena *arena, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *out = arena_alloc(arena, n);
    memcpy(out, s, n);
    return out;
}

static AstNode **clone_node_array(AstNode **src, int n, Arena *arena,
                                  TypeBinding *bindings, int bc) {
    if (n == 0 || !src) return NULL;
    AstNode **dst = arena_alloc(arena, sizeof(AstNode*) * n);
    for (int i = 0; i < n; i++) dst[i] = do_clone(src[i], arena, bindings, bc);
    return dst;
}

static char **clone_str_array(char **src, int n, Arena *arena) {
    if (n == 0 || !src) return NULL;
    char **dst = arena_alloc(arena, sizeof(char*) * n);
    for (int i = 0; i < n; i++) dst[i] = clone_str(arena, src[i]);
    return dst;
}

static AstNode *do_clone(AstNode *node, Arena *arena,
                         TypeBinding *bindings, int binding_count) {
    if (!node) return NULL;

    // NODE_TYPE_NAME may match a binding; if so, swap in a clone of the
    // binding's substitute (still passing bindings so nested generics
    // would also resolve, though we don't use that case yet).
    if (node->kind == NODE_TYPE_NAME && node->type_name.name) {
        for (int i = 0; i < binding_count; i++) {
            if (strcmp(bindings[i].name, node->type_name.name) == 0) {
                return do_clone(bindings[i].type_node, arena, NULL, 0);
            }
        }
    }

    AstNode *out = arena_alloc(arena, sizeof(AstNode));
    *out = *node;   // shallow copy of the union — pointers fixed up below

    switch (node->kind) {
        case NODE_PROGRAM:
            out->program.decls = clone_node_array(node->program.decls,
                node->program.decl_count, arena, bindings, binding_count);
            break;
        case NODE_FN_DECL:
            out->fn_decl.name = clone_str(arena, node->fn_decl.name);
            out->fn_decl.return_type = do_clone(node->fn_decl.return_type, arena, bindings, binding_count);
            out->fn_decl.body = do_clone(node->fn_decl.body, arena, bindings, binding_count);
            if (node->fn_decl.param_count > 0) {
                out->fn_decl.params = arena_alloc(arena, sizeof(Param) * node->fn_decl.param_count);
                for (int i = 0; i < node->fn_decl.param_count; i++) {
                    out->fn_decl.params[i].name = clone_str(arena, node->fn_decl.params[i].name);
                    out->fn_decl.params[i].is_mutable = node->fn_decl.params[i].is_mutable;
                    out->fn_decl.params[i].type = do_clone(node->fn_decl.params[i].type,
                                                          arena, bindings, binding_count);
                }
            }
            out->fn_decl.type_params = clone_str_array(node->fn_decl.type_params,
                node->fn_decl.type_param_count, arena);
            out->fn_decl.constraints = clone_str_array(node->fn_decl.constraints,
                node->fn_decl.constraint_count, arena);
            break;
        case NODE_BLOCK:
            out->block.stmts = clone_node_array(node->block.stmts,
                node->block.stmt_count, arena, bindings, binding_count);
            break;
        case NODE_VAR_DECL:
            out->var_decl.name = clone_str(arena, node->var_decl.name);
            out->var_decl.type_ann = do_clone(node->var_decl.type_ann, arena, bindings, binding_count);
            out->var_decl.init_expr = do_clone(node->var_decl.init_expr, arena, bindings, binding_count);
            // Reset resolved_type so sema re-runs cleanly on the clone.
            out->resolved_type = NULL;
            break;
        case NODE_ASSIGN:
            out->assign.name = clone_str(arena, node->assign.name);
            out->assign.value = do_clone(node->assign.value, arena, bindings, binding_count);
            break;
        case NODE_IF_STMT:
            out->if_stmt.condition = do_clone(node->if_stmt.condition, arena, bindings, binding_count);
            out->if_stmt.then_block = do_clone(node->if_stmt.then_block, arena, bindings, binding_count);
            out->if_stmt.else_block = do_clone(node->if_stmt.else_block, arena, bindings, binding_count);
            break;
        case NODE_WHILE_STMT:
            out->while_stmt.condition = do_clone(node->while_stmt.condition, arena, bindings, binding_count);
            out->while_stmt.body = do_clone(node->while_stmt.body, arena, bindings, binding_count);
            break;
        case NODE_FOR_STMT:
            out->for_stmt.var_name = clone_str(arena, node->for_stmt.var_name);
            out->for_stmt.var_is_mutable = node->for_stmt.var_is_mutable;
            out->for_stmt.index_name = clone_str(arena, node->for_stmt.index_name);
            out->for_stmt.iterable = do_clone(node->for_stmt.iterable, arena, bindings, binding_count);
            out->for_stmt.body = do_clone(node->for_stmt.body, arena, bindings, binding_count);
            break;
        case NODE_MATCH_STMT: {
            out->match_stmt.subject = do_clone(node->match_stmt.subject, arena, bindings, binding_count);
            int n = node->match_stmt.arm_count;
            if (n > 0) {
                out->match_stmt.arms = arena_alloc(arena, sizeof(struct MatchArm) * n);
                for (int i = 0; i < n; i++) {
                    out->match_stmt.arms[i].is_wildcard = node->match_stmt.arms[i].is_wildcard;
                    out->match_stmt.arms[i].pattern = do_clone(node->match_stmt.arms[i].pattern, arena, bindings, binding_count);
                    out->match_stmt.arms[i].body = do_clone(node->match_stmt.arms[i].body, arena, bindings, binding_count);
                }
            }
            break;
        }
        case NODE_DONE_STMT:
            out->done_stmt.value = do_clone(node->done_stmt.value, arena, bindings, binding_count);
            break;
        case NODE_EXPR_STMT:
            out->expr_stmt.expr = do_clone(node->expr_stmt.expr, arena, bindings, binding_count);
            break;
        case NODE_DEFER_STMT:
            out->defer_stmt.stmt = do_clone(node->defer_stmt.stmt, arena, bindings, binding_count);
            break;
        case NODE_UNSAFE_BLOCK:
            out->unsafe_block.body = do_clone(node->unsafe_block.body, arena, bindings, binding_count);
            break;
        case NODE_ZONE_STMT:
            out->zone_stmt.name = clone_str(arena, node->zone_stmt.name);
            out->zone_stmt.body = do_clone(node->zone_stmt.body, arena, bindings, binding_count);
            break;
        case NODE_DEREF_ASSIGN:
            out->deref_assign.ptr_expr = do_clone(node->deref_assign.ptr_expr, arena, bindings, binding_count);
            out->deref_assign.value = do_clone(node->deref_assign.value, arena, bindings, binding_count);
            break;
        case NODE_FAIL_STMT:
            out->fail_stmt.value = do_clone(node->fail_stmt.value, arena, bindings, binding_count);
            break;
        case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_BOOL_LIT:
        case NODE_NONE_LIT:
            // Pure scalar literals — shallow copy already covers them.
            break;
        case NODE_STRING_LIT:
            // Keep pointer to source text (immutable).
            break;
        case NODE_STRING_INTERP: {
            int n = node->string_interp.expr_count;
            if (n > 0) {
                out->string_interp.exprs = arena_alloc(arena, sizeof(AstNode*) * n);
                for (int i = 0; i < n; i++)
                    out->string_interp.exprs[i] = do_clone(node->string_interp.exprs[i], arena, bindings, binding_count);
            }
            // parts/part_lengths shared (pointers into source).
            break;
        }
        case NODE_IDENT:
            out->ident.name = clone_str(arena, node->ident.name);
            out->ident.type_arg_count = node->ident.type_arg_count;
            if (node->ident.type_arg_count > 0) {
                out->ident.type_args = arena_alloc(arena,
                    sizeof(AstNode*) * node->ident.type_arg_count);
                for (int i = 0; i < node->ident.type_arg_count; i++) {
                    out->ident.type_args[i] = do_clone(node->ident.type_args[i],
                        arena, bindings, binding_count);
                }
            }
            break;
        case NODE_BINARY_EXPR:
            out->binary.left = do_clone(node->binary.left, arena, bindings, binding_count);
            out->binary.right = do_clone(node->binary.right, arena, bindings, binding_count);
            break;
        case NODE_UNARY_EXPR:
            out->unary.operand = do_clone(node->unary.operand, arena, bindings, binding_count);
            break;
        case NODE_CALL_EXPR:
            out->call.name = clone_str(arena, node->call.name);
            out->call.args = clone_node_array(node->call.args,
                node->call.arg_count, arena, bindings, binding_count);
            break;
        case NODE_LAMBDA:
            out->lambda.param_names = clone_str_array(node->lambda.param_names,
                node->lambda.param_count, arena);
            out->lambda.body = do_clone(node->lambda.body, arena, bindings, binding_count);
            out->lambda.generated_name = clone_str(arena, node->lambda.generated_name);
            break;
        case NODE_LIST_LIT:
            out->list_lit.elements = clone_node_array(node->list_lit.elements,
                node->list_lit.element_count, arena, bindings, binding_count);
            break;
        case NODE_MAP_LIT:
            out->map_lit.keys = clone_node_array(node->map_lit.keys,
                node->map_lit.entry_count, arena, bindings, binding_count);
            out->map_lit.values = clone_node_array(node->map_lit.values,
                node->map_lit.entry_count, arena, bindings, binding_count);
            break;
        case NODE_SET_LIT:
            out->set_lit.elements = clone_node_array(node->set_lit.elements,
                node->set_lit.element_count, arena, bindings, binding_count);
            break;
        case NODE_INDEX_EXPR:
            out->index_expr.object = do_clone(node->index_expr.object, arena, bindings, binding_count);
            out->index_expr.index = do_clone(node->index_expr.index, arena, bindings, binding_count);
            break;
        case NODE_SLICE_EXPR:
            out->slice_expr.object = do_clone(node->slice_expr.object, arena, bindings, binding_count);
            out->slice_expr.start = do_clone(node->slice_expr.start, arena, bindings, binding_count);
            out->slice_expr.end = do_clone(node->slice_expr.end, arena, bindings, binding_count);
            break;
        case NODE_LIST_COMP:
            out->list_comp.expr = do_clone(node->list_comp.expr, arena, bindings, binding_count);
            out->list_comp.var_name = clone_str(arena, node->list_comp.var_name);
            out->list_comp.iterable = do_clone(node->list_comp.iterable, arena, bindings, binding_count);
            out->list_comp.condition = do_clone(node->list_comp.condition, arena, bindings, binding_count);
            break;
        case NODE_ELSE_EXPR:
            out->else_expr.value = do_clone(node->else_expr.value, arena, bindings, binding_count);
            out->else_expr.fallback = do_clone(node->else_expr.fallback, arena, bindings, binding_count);
            break;
        case NODE_CAST_EXPR:
            out->cast_expr.value = do_clone(node->cast_expr.value, arena, bindings, binding_count);
            break;
        case NODE_FIELD_EXPR:
            out->field_expr.object = do_clone(node->field_expr.object, arena, bindings, binding_count);
            out->field_expr.field_name = clone_str(arena, node->field_expr.field_name);
            break;
        case NODE_METHOD_CALL:
            out->method_call.object = do_clone(node->method_call.object, arena, bindings, binding_count);
            out->method_call.method_name = clone_str(arena, node->method_call.method_name);
            out->method_call.args = clone_node_array(node->method_call.args,
                node->method_call.arg_count, arena, bindings, binding_count);
            out->method_call.is_mutable_call = node->method_call.is_mutable_call;
            out->method_call.is_field_call = node->method_call.is_field_call;
            break;
        case NODE_SHAPE_LIT:
            out->shape_lit.shape_name = clone_str(arena, node->shape_lit.shape_name);
            out->shape_lit.field_names = clone_str_array(node->shape_lit.field_names,
                node->shape_lit.field_count, arena);
            out->shape_lit.field_values = clone_node_array(node->shape_lit.field_values,
                node->shape_lit.field_count, arena, bindings, binding_count);
            out->shape_lit.type_args = clone_node_array(node->shape_lit.type_args,
                node->shape_lit.type_arg_count, arena, bindings, binding_count);
            break;
        case NODE_FIELD_ASSIGN:
            out->field_assign.object = do_clone(node->field_assign.object, arena, bindings, binding_count);
            out->field_assign.field_name = clone_str(arena, node->field_assign.field_name);
            out->field_assign.value = do_clone(node->field_assign.value, arena, bindings, binding_count);
            break;
        case NODE_INDEX_ASSIGN:
            out->index_assign.object = do_clone(node->index_assign.object, arena, bindings, binding_count);
            out->index_assign.index = do_clone(node->index_assign.index, arena, bindings, binding_count);
            out->index_assign.value = do_clone(node->index_assign.value, arena, bindings, binding_count);
            break;
        case NODE_TYPE_NAME:
            out->type_name.name = clone_str(arena, node->type_name.name);
            out->type_name.type_args = clone_node_array(node->type_name.type_args,
                node->type_name.type_arg_count, arena, bindings, binding_count);
            // Reuse type_ptr slot for list/set elem types — clone if present.
            if (node->type_name.type_kind == TOK_LBRACKET ||
                node->type_name.type_kind == TOK_SET) {
                out->type_ptr.base_type = do_clone(node->type_ptr.base_type, arena, bindings, binding_count);
            }
            break;
        case NODE_TYPE_PTR:
            out->type_ptr.base_type = do_clone(node->type_ptr.base_type, arena, bindings, binding_count);
            break;
        case NODE_TYPE_REF:
            out->type_ref.is_mutable = node->type_ref.is_mutable;
            out->type_ref.base_type = do_clone(node->type_ref.base_type, arena, bindings, binding_count);
            break;
        case NODE_TYPE_OPTIONAL:
            out->type_optional.inner_type = do_clone(node->type_optional.inner_type, arena, bindings, binding_count);
            break;
        case NODE_SHARED_EXPR:
            out->shared_expr.init_expr = do_clone(node->shared_expr.init_expr, arena, bindings, binding_count);
            break;
        case NODE_GO_EXPR:
            out->go_expr.call_expr = do_clone(node->go_expr.call_expr, arena, bindings, binding_count);
            break;
        case NODE_WAIT_EXPR:
            out->wait_expr.handle_expr = do_clone(node->wait_expr.handle_expr, arena, bindings, binding_count);
            break;
        case NODE_TRY_EXPR:
            out->try_expr.expr = do_clone(node->try_expr.expr, arena, bindings, binding_count);
            break;
        // Decls we don't expect to clone for shape monomorphization, but
        // handle defensively so deep recursion doesn't crash if invoked.
        case NODE_SHAPE_DECL:
        case NODE_EXTERN_BLOCK: case NODE_EXTERN_FN_DECL:
        case NODE_USE_DECL: case NODE_USE_C_DECL:
        case NODE_CONST_DECL: case NODE_TYPE_ALIAS:
        case NODE_COND_DECL: case NODE_BREAK_STMT:
        case NODE_CONTINUE_STMT:
            // Shallow copy is enough for these in our current usage.
            break;
    }
    out->resolved_type = NULL;  // force sema to re-resolve
    return out;
}

AstNode *ast_clone(AstNode *node, Arena *arena,
                   TypeBinding *bindings, int binding_count) {
    return do_clone(node, arena, bindings, binding_count);
}
