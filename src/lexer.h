#ifndef LEXER_H
#define LEXER_H

#include "mix.h"
#include "token.h"

typedef struct {
    const char *source;     // source text
    const char *current;    // current position
    const char *filename;
    int line;
    int col;

    // Indentation tracking
    int indent_stack[128];
    int indent_top;

    // Bracket nesting depth (suppress NEWLINE when > 0)
    int paren_depth;

    // Token output buffer (for emitting multiple DEDENT tokens)
    Token *tokens;
    int token_count;
    int token_capacity;

    // Are we at the start of a line? (for indent processing)
    bool at_line_start;

    Arena *arena;
} Lexer;

Lexer lexer_create(const char *source, const char *filename, Arena *arena);
void lexer_tokenize(Lexer *lex);
void lexer_print_tokens(Lexer *lex);

#endif // LEXER_H
