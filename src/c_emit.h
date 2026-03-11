#ifndef C_EMIT_H
#define C_EMIT_H

#include "mix.h"
#include "ast.h"
#include "types.h"
#include "symtab.h"

typedef struct {
    FILE *out;
    int temp_counter;
    int label_counter;
    int data_counter;
    Arena *arena;
    SymTab *symtab;
    // String literals (for dedup)
    struct { const char *value; int length; } strings[1024];
    int string_count;
    // Defer stack for current function
    AstNode *deferred[64];
    int defer_count;
    // Constants (name -> AST expr)
    struct { char *name; AstNode *value; int cached_temp; } constants[8192];
    int const_count;
    // Current shape being emitted (for method field access)
    MixType *current_shape;
    // Current function return type (for optional wrapping)
    MixType *current_return_type;
    // Lambda counter for generating unique names
    int lambda_counter;
    // Collected lambdas to emit after functions
    AstNode *lambdas[256];
    int lambda_count;
    // Names of local variables holding function pointers (for indirect calls)
    char *fn_ptr_vars[128];
    int fn_ptr_var_count;
    // Match expression result temp (for implicit return)
    int last_match_temp;
    // Indentation level
    int indent;
} CEmitter;

CEmitter c_emitter_create(FILE *out, Arena *arena, SymTab *symtab);
void c_emit_program(CEmitter *emit, AstNode *program);

#endif // C_EMIT_H
