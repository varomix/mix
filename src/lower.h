// AST → LIR lowering.
//
// Phase 2 scope: lower a hello.mix-shape program (top-level main() with
// print(string-literal) calls) into a LirModule. Anything outside scope
// raises mix_error with a clear "lowering (Phase 2): ... — not yet
// implemented" message at the source location.
//
// Phase 3 expands this to scalars, locals, control flow. Phase 4A/4B
// expands to shapes. The shape of the public API stays the same — just
// more code paths get covered.

#ifndef LOWER_H
#define LOWER_H

#include "mix.h"
#include "ast.h"
#include "lir.h"
#include "symtab.h"

// Lower an entire program. Returns the new LirModule allocated in `arena`,
// or NULL if lowering failed (in which case mix_error_count() will be > 0).
LirModule *lower_program(AstNode *program, Arena *arena, SymTab *symtab);

#endif // LOWER_H
