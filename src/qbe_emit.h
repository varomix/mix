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
    // Refcount Phase 1: shape-typed locals to release at function exit.
    // Entries store the emitted `%v.<name>` slot names, not source names.
    // Reset at function entry, walked before each `ret` to emit mix_release.
    char *rc_locals[256];
    int   rc_local_count;
    // Names of shape types declared in THIS translation unit. SHAPE_LIT
    // codegen passes `$release_<Name>` only if the shape is in here;
    // otherwise it passes 0 and lets mix_release fall back to a plain
    // mix_shape_free (correct for C-imported shapes from `use c`).
    char *local_shape_names[1024];
    int   local_shape_name_count;
} QbeEmitter;

QbeEmitter qbe_emitter_create(FILE *out, Arena *arena, SymTab *symtab, bool debug);
void qbe_emit_program(QbeEmitter *emit, AstNode *program);

#endif // QBE_EMIT_H
