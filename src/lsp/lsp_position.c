#include "lsp_position.h"

Token *tokens_find_at(Token *tokens, int count, int line, int col) {
    for (int i = 0; i < count; i++) {
        Token *t = &tokens[i];
        if (t->line == line && col >= t->col && col < t->col + t->length) {
            return t;
        }
    }
    return NULL;
}

const char *token_ident_name(Token *tok, char *buf, int bufsize) {
    if (!tok) return NULL;
    if (tok->kind != TOK_IDENT && tok->kind != TOK_IDENT_MUT) return NULL;
    int len = tok->length;
    if (tok->kind == TOK_IDENT_MUT && len > 0) len--; // strip !
    if (len >= bufsize) len = bufsize - 1;
    memcpy(buf, tok->start, len);
    buf[len] = '\0';
    return buf;
}

// --- AST position lookup ---

static void find_recursive(AstNode *node, AstNode *parent, AstNode *scope,
                            int line, int col, PositionResult *best) {
    if (!node) return;

    // Check if this node is on the target line and starts at or before our col
    if (node->loc.line == line && node->loc.col <= col) {
        if (!best->node || node->loc.col >= best->node->loc.col) {
            best->node = node;
            best->parent = parent;
            best->scope_owner = scope;
        }
    }

    AstNode *new_scope = (node->kind == NODE_FN_DECL) ? node : scope;

    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.decl_count; i++)
                find_recursive(node->program.decls[i], node, new_scope, line, col, best);
            break;
        case NODE_FN_DECL:
            find_recursive(node->fn_decl.body, node, new_scope, line, col, best);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++)
                find_recursive(node->block.stmts[i], node, new_scope, line, col, best);
            break;
        case NODE_VAR_DECL:
            find_recursive(node->var_decl.init_expr, node, new_scope, line, col, best);
            break;
        case NODE_ASSIGN:
            find_recursive(node->assign.value, node, new_scope, line, col, best);
            break;
        case NODE_IF_STMT:
            find_recursive(node->if_stmt.condition, node, new_scope, line, col, best);
            find_recursive(node->if_stmt.then_block, node, new_scope, line, col, best);
            find_recursive(node->if_stmt.else_block, node, new_scope, line, col, best);
            break;
        case NODE_WHILE_STMT:
            find_recursive(node->while_stmt.condition, node, new_scope, line, col, best);
            find_recursive(node->while_stmt.body, node, new_scope, line, col, best);
            break;
        case NODE_FOR_STMT:
            find_recursive(node->for_stmt.iterable, node, new_scope, line, col, best);
            find_recursive(node->for_stmt.body, node, new_scope, line, col, best);
            break;
        case NODE_DONE_STMT:
            find_recursive(node->done_stmt.value, node, new_scope, line, col, best);
            break;
        case NODE_EXPR_STMT:
            find_recursive(node->expr_stmt.expr, node, new_scope, line, col, best);
            break;
        case NODE_BINARY_EXPR:
            find_recursive(node->binary.left, node, new_scope, line, col, best);
            find_recursive(node->binary.right, node, new_scope, line, col, best);
            break;
        case NODE_UNARY_EXPR:
            find_recursive(node->unary.operand, node, new_scope, line, col, best);
            break;
        case NODE_CALL_EXPR:
            for (int i = 0; i < node->call.arg_count; i++)
                find_recursive(node->call.args[i], node, new_scope, line, col, best);
            break;
        case NODE_METHOD_CALL:
            find_recursive(node->method_call.object, node, new_scope, line, col, best);
            for (int i = 0; i < node->method_call.arg_count; i++)
                find_recursive(node->method_call.args[i], node, new_scope, line, col, best);
            break;
        case NODE_FIELD_EXPR:
            find_recursive(node->field_expr.object, node, new_scope, line, col, best);
            break;
        case NODE_INDEX_EXPR:
            find_recursive(node->index_expr.object, node, new_scope, line, col, best);
            find_recursive(node->index_expr.index, node, new_scope, line, col, best);
            break;
        case NODE_SHAPE_DECL:
            for (int i = 0; i < node->shape_decl.method_count; i++)
                find_recursive(node->shape_decl.methods[i], node, new_scope, line, col, best);
            break;
        case NODE_SHAPE_LIT:
            for (int i = 0; i < node->shape_lit.field_count; i++)
                find_recursive(node->shape_lit.field_values[i], node, new_scope, line, col, best);
            break;
        case NODE_LIST_LIT:
            for (int i = 0; i < node->list_lit.element_count; i++)
                find_recursive(node->list_lit.elements[i], node, new_scope, line, col, best);
            break;
        case NODE_MATCH_STMT:
            find_recursive(node->match_stmt.subject, node, new_scope, line, col, best);
            for (int i = 0; i < node->match_stmt.arm_count; i++) {
                find_recursive(node->match_stmt.arms[i].pattern, node, new_scope, line, col, best);
                find_recursive(node->match_stmt.arms[i].body, node, new_scope, line, col, best);
            }
            break;
        case NODE_FIELD_ASSIGN:
            find_recursive(node->field_assign.object, node, new_scope, line, col, best);
            find_recursive(node->field_assign.value, node, new_scope, line, col, best);
            break;
        case NODE_INDEX_ASSIGN:
            find_recursive(node->index_assign.object, node, new_scope, line, col, best);
            find_recursive(node->index_assign.index, node, new_scope, line, col, best);
            find_recursive(node->index_assign.value, node, new_scope, line, col, best);
            break;
        case NODE_ELSE_EXPR:
            find_recursive(node->else_expr.value, node, new_scope, line, col, best);
            find_recursive(node->else_expr.fallback, node, new_scope, line, col, best);
            break;
        case NODE_CONST_DECL:
            find_recursive(node->const_decl.value, node, new_scope, line, col, best);
            break;
        case NODE_DEFER_STMT:
            find_recursive(node->defer_stmt.stmt, node, new_scope, line, col, best);
            break;
        case NODE_UNSAFE_BLOCK:
            find_recursive(node->unsafe_block.body, node, new_scope, line, col, best);
            break;
        case NODE_ZONE_STMT:
            find_recursive(node->zone_stmt.body, node, new_scope, line, col, best);
            break;
        case NODE_COND_DECL:
            for (int i = 0; i < node->cond_decl.decl_count; i++)
                find_recursive(node->cond_decl.decls[i], node, new_scope, line, col, best);
            break;
        default:
            break;
    }
}

bool ast_find_node_at(AstNode *root, int line, int col, PositionResult *result) {
    memset(result, 0, sizeof(PositionResult));
    find_recursive(root, NULL, NULL, line, col, result);
    return result->node != NULL;
}
