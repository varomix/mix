#ifndef SEMA_H
#define SEMA_H

#include "mix.h"
#include "ast.h"
#include "types.h"
#include "symtab.h"

typedef struct {
    SymTab symtab;
    Arena *arena;
    // Generic context: type parameter names currently in scope
    char **generic_params;
    int generic_param_count;
    // Conditional compilation
    bool debug_mode;
} Sema;

Sema sema_create(Arena *arena);
void sema_analyze(Sema *sema, AstNode *program);

#endif // SEMA_H
