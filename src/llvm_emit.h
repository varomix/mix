// LLVM textual IR emitter — Phase 2.
//
// The emitter is now a LIR consumer (see `src/lir.h`, `src/lower.h`). It
// no longer walks the AST directly. Phase 1's AST-walking entrypoint has
// been replaced; main.c calls `lower_program()` to build a `LirModule`
// and then `llvm_emit_module()` here to render it as textual `.ll`.
//
// Scope is unchanged from Phase 1: hello.mix-shape programs only.
// Anything outside scope is rejected during lowering (in `src/lower.c`),
// not here.

#ifndef LLVM_EMIT_H
#define LLVM_EMIT_H

#include "mix.h"
#include "lir.h"

typedef struct {
    FILE *out;
} LlvmEmitter;

LlvmEmitter llvm_emitter_create(FILE *out);
void        llvm_emit_module(LlvmEmitter *emit, LirModule *mod);

#endif // LLVM_EMIT_H
