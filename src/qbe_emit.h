#ifndef QBE_EMIT_H
#define QBE_EMIT_H

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
    // Collect string literals for data section
    struct { const char *value; int length; char *qbe_name; } strings[1024];
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
    // Debug info (DWARF via QBE dbgloc)
    bool emit_debug_info;
    int dbg_line;
    // Match expression result temp (for implicit return)
    int last_match_temp;
    // Loop label stack for break/continue
    int break_labels[32];
    int continue_labels[32];
    int loop_depth;
    // Active local-name bindings for this function. Source identifiers map to
    // the currently-visible QBE local name (used to keep shadowed locals and
    // loop vars from colliding on the same `%v.*` slot).
    struct { char *source_name; char *emitted_name; } local_bindings[2048];
    int local_binding_count;
    int local_scope_marks[256];
    int local_scope_depth;
    // Mutable params are passed by address, copied into local storage at
    // function entry, then written back on every return path.
    char *mutable_param_names[256];
    MixType *mutable_param_types[256];
    int mutable_param_count;
    // QBE's alloc* instructions behave like dynamic stack allocation if they
    // are emitted inside loops/branches. Record stack slots per function and
    // hoist them into the function prologue instead of emitting inline.
    char *stack_allocs[4096];
    int stack_alloc_count;
} QbeEmitter;

QbeEmitter qbe_emitter_create(FILE *out, Arena *arena, SymTab *symtab, bool debug);
void qbe_emit_program(QbeEmitter *emit, AstNode *program);

#endif // QBE_EMIT_H
