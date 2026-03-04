#ifndef LSP_POSITION_H
#define LSP_POSITION_H

#include "../ast.h"
#include "../token.h"

typedef struct {
    AstNode *node;
    AstNode *parent;
    AstNode *scope_owner;
} PositionResult;

// Find the deepest AST node at the given 1-based (line, col).
bool ast_find_node_at(AstNode *root, int line, int col, PositionResult *result);

// Find the token whose range contains the given 1-based (line, col).
Token *tokens_find_at(Token *tokens, int count, int line, int col);

// Extract identifier name from token at position. Returns NULL if not an ident.
// Writes into buf, stripping trailing ! from mutable idents.
const char *token_ident_name(Token *tok, char *buf, int bufsize);

#endif // LSP_POSITION_H
