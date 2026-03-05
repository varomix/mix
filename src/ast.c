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
            printf("For: %s", node->for_stmt.var_name);
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
            printf("Ident: %s%s\n", node->ident.name, node->ident.is_mutable ? "!" : ""); break;
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
            printf("MethodCall: .%s(%d args)\n", node->method_call.method_name,
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
