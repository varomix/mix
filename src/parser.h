#ifndef PARSER_H
#define PARSER_H

#include "mix.h"
#include "token.h"
#include "ast.h"
#include "arena.h"

typedef struct {
    Token *tokens;
    int token_count;
    int pos;
    Arena *arena;
    const char *filename;
} Parser;

Parser parser_create(Token *tokens, int token_count, Arena *arena, const char *filename);
AstNode *parser_parse(Parser *p);

#endif // PARSER_H
