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
    int file_md_id;
    const char *filename;     // basename for !DIFile
    const char *directory;    // dirname for !DIFile
} LlvmDbgFile;

typedef struct {
    int sp_md_id;
    int subroutine_type_md_id;
    int file_md_id;
    int line;
    const char *name;
} LlvmDbgFn;

typedef struct {
    int md_id;
    int line;
    int col;
    int scope_md_id;          // !DISubprogram id
} LlvmDbgLoc;

typedef struct {
    int     md_id;
    LirType lir_type;          // for scalar locals; LIR_TY_VOID for shapes
    int     shape_size_bytes;  // 0 for scalar
} LlvmDbgBasicType;

typedef struct {
    int         md_id;
    int         scope_md_id;   // !DISubprogram id of the enclosing fn
    int         file_md_id;
    int         line;
    int         type_md_id;    // !DIBasicType
    int         arg_index;     // 1-based for params; 0 for locals
    const char *name;
} LlvmDbgVar;

typedef struct {
    FILE *out;

    // Debug info (Phase 6). Off by default.
    bool        debug;
    const char *unit_filename;        // primary source path for !DICompileUnit
    const char *unit_directory;
    int         unit_cu_md_id;
    int         next_md_id;
    int         current_sp_md_id;     // active subprogram while emitting a fn

    LlvmDbgFile *files;
    int          file_count;
    int          file_capacity;

    LlvmDbgFn   *fns;
    int          fn_count;
    int          fn_capacity;

    LlvmDbgLoc  *locs;
    int          loc_count;
    int          loc_capacity;

    LlvmDbgBasicType *basic_types;
    int               basic_type_count;
    int               basic_type_capacity;

    LlvmDbgVar  *vars;
    int          var_count;
    int          var_capacity;

    bool         needs_dbg_declare_decl;   // emit `declare void @llvm.dbg.declare(...)` once
} LlvmEmitter;

LlvmEmitter llvm_emitter_create(FILE *out);
void        llvm_emitter_enable_debug(LlvmEmitter *emit, const char *source_path);
void        llvm_emit_module(LlvmEmitter *emit, LirModule *mod);

#endif // LLVM_EMIT_H
