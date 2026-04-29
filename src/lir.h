// LIR — MIX Lowered IR.
//
// Phase 3 vocabulary: scalar arithmetic, comparisons, locals via
// alloca/load/store, conditional and unconditional branches, multiple
// functions with parameters and returns. Phase 4A/4B will add explicit
// shape copies, field-address ops, and aggregate ABI lowering.
//
// Scope fence (from LLVM_MIGRATION_PLAN.md, Decision #4):
//   LIR DOES represent: allocas, loads/stores, shape copies, field
//   addresses, explicit control flow, loop writeback points, runtime calls.
//   LIR DOES NOT represent: optimization passes, generic instantiation,
//   type inference, long-lived frontend type metadata, register
//   allocation, target-specific foreign ABI policy.

#ifndef LIR_H
#define LIR_H

#include "mix.h"

// ---- Types -----------------------------------------------------------------

typedef enum {
    LIR_TY_VOID,
    LIR_TY_I1,     // Phase 3: result of comparisons
    LIR_TY_I8,
    LIR_TY_I32,
    LIR_TY_I64,
    LIR_TY_F64,
    LIR_TY_PTR,    // catch-all pointer (covers str, ref, box, zone, *T)
} LirType;

// ---- Operand kinds ---------------------------------------------------------

typedef enum {
    LIR_OPND_NONE,
    LIR_OPND_STRING,    // .id is the module string-table id
    LIR_OPND_PARAM,     // .id is the param index in the enclosing function
    LIR_OPND_VALUE,     // .id is an SSA result id from an earlier instr
    LIR_OPND_I64,       // .imm is the immediate int value
    LIR_OPND_F64,       // .fimm is the immediate float value
    LIR_OPND_BOOL,      // .imm is 0 or 1
    LIR_OPND_FN_REF,    // Phase 5: function pointer constant (renders as @<name>)
} LirOpndKind;

typedef struct {
    LirOpndKind kind;
    LirType     type;       // type carried with operand for codegen convenience
    int         id;         // string id / param index / value id
    long long   imm;        // immediate int / bool
    double      fimm;       // immediate float
    const char *fn_name;    // FN_REF: function symbol name (interned in arena)
} LirOpnd;

// ---- Instruction kinds -----------------------------------------------------

typedef enum {
    LIR_OP_CALL,    // call callee(args) → optional result
    LIR_OP_RET,     // return (with optional value) — terminator

    // Memory
    LIR_OP_ALLOCA,        // result = alloca <type>            (returns ptr)
    LIR_OP_ALLOCA_BYTES,  // Phase 4A: alloca [<bytes> x i8], align <align>
    LIR_OP_LOAD,          // result = load  <type>, ptr <addr>
    LIR_OP_STORE,         // store <type> <value>, ptr <addr>  (no result)
    LIR_OP_PTR_OFFSET,    // Phase 4A: result = base + <byte_offset>  (typed ptr)
    LIR_OP_MEMCPY,        // Phase 4A: copy <bytes> from src to dst (no result)

    // Arithmetic / comparison / logic
    LIR_OP_BIN,     // result = <bin_op> <type> <a>, <b>
    LIR_OP_UN,      // result = <un_op>  <type> <a>

    // Type conversions (between scalar types)
    LIR_OP_CONV,    // result = <conv_kind> <src_ty> <a> to <dst_ty>

    // Control flow (terminators)
    LIR_OP_BR,         // br label %target
    LIR_OP_BR_COND,    // br i1 <cond>, label %then, label %else

    // Pseudo: marks the start of a new basic block. The emitter renders
    // a label header. Lowering inserts a LABEL whenever it begins a new
    // block (after a branch, at loop entry, at merge points, etc).
    LIR_OP_LABEL,
} LirOpKind;

typedef enum {
    // Arithmetic
    LIR_BIN_ADD, LIR_BIN_SUB, LIR_BIN_MUL, LIR_BIN_DIV, LIR_BIN_MOD,
    // Comparisons (signed for int, ordered for float — emitter picks)
    LIR_BIN_EQ,  LIR_BIN_NE,  LIR_BIN_LT,  LIR_BIN_LE,  LIR_BIN_GT, LIR_BIN_GE,
    // Logical (operate on i1; do NOT short-circuit — lowering decomposes
    // short-circuit into branches when needed)
    LIR_BIN_AND, LIR_BIN_OR,
} LirBinOp;

typedef enum {
    LIR_UN_NEG,   // -x  (int or float)
    LIR_UN_NOT,   // logical not (i1 → i1)
} LirUnOp;

typedef enum {
    LIR_CONV_SEXT,     // sign-extend integer
    LIR_CONV_ZEXT,     // zero-extend integer
    LIR_CONV_TRUNC,    // truncate integer
    LIR_CONV_SITOFP,   // signed int → float
    LIR_CONV_FPTOSI,   // float → signed int
    LIR_CONV_BITCAST,  // reinterpret bits between same-size types (double ↔ i64)
    LIR_CONV_PTRTOINT, // pointer → integer
    LIR_CONV_INTTOPTR, // integer → pointer
} LirConvKind;

typedef struct {
    LirOpKind op;

    int       result;          // -1 if no result
    LirType   result_type;

    SrcLoc    loc;             // for error reporting / debug info

    // -- CALL --
    const char *callee;
    LirOpnd    *args;
    int         arg_count;
    // Phase 5: indirect call. When .kind != LIR_OPND_NONE, the call goes
    // through this function-pointer operand instead of `callee`. The
    // emitter must still know the declared signature to render the call
    // type prefix; param_types/return_type carried alongside.
    LirOpnd     indirect_callee;
    LirType    *indirect_param_types;
    int         indirect_param_count;

    // -- RET --
    bool        has_ret_value;
    LirOpnd     ret_value;

    // -- ALLOCA --
    LirType     alloca_type;   // type to allocate (size derived from this)

    // -- ALLOCA_BYTES (Phase 4A) --
    int         alloca_byte_size;
    int         alloca_byte_align;

    // -- LOAD / STORE / MEMCPY --
    LirOpnd     mem_addr;      // pointer operand (LOAD/STORE: addr; MEMCPY: dst)
    LirOpnd     mem_value;     // STORE: value to store; MEMCPY: src ptr
    LirType     mem_type;      // value type (LOAD/STORE only)
    int         mem_size_bytes;// MEMCPY only: byte count
    int         mem_align;     // MEMCPY only: alignment

    // -- PTR_OFFSET (Phase 4A) --
    LirOpnd     ptr_base;      // base pointer
    int         ptr_offset;    // byte offset (constant)

    // -- BIN --
    LirBinOp    bin_op;
    LirOpnd     bin_a;
    LirOpnd     bin_b;
    LirType     bin_type;      // operand type (both sides match); result type
                               // is derived (cmp → I1, arith → bin_type)

    // -- UN --
    LirUnOp     un_op;
    LirOpnd     un_a;
    LirType     un_type;

    // -- CONV --
    LirConvKind conv_kind;
    LirOpnd     conv_a;
    LirType     conv_src_type;
    LirType     conv_dst_type;

    // -- BR --
    int         br_target;     // label id

    // -- BR_COND --
    LirOpnd     br_cond;
    int         br_then;
    int         br_else;

    // -- LABEL --
    int         label_id;
} LirInstr;

// ---- Function --------------------------------------------------------------

typedef struct LirModule LirModule;

typedef struct {
    LirModule  *mod;
    const char *name;
    bool        is_main;            // emit mix_set_args() preamble
    LirType     return_type;
    int         param_count;
    LirType    *param_types;
    char      **param_names;        // interned in arena (used for %p<name> in IR)
    bool       *param_is_mutable;   // currently unused; reserved for later phases

    int         next_value_id;
    int         next_label_id;

    LirInstr   *instrs;
    int         instr_count;
    int         instr_capacity;
} LirFunc;

// ---- Module ----------------------------------------------------------------

typedef struct {
    int         id;
    const char *bytes;
    int         byte_count;
} LirString;

// Runtime helpers used by the program. Built up by lowering as it
// encounters calls. The emitter renders a `declare` for each unique entry.
typedef struct {
    const char *name;
    LirType     return_type;
    LirType    *param_types;
    int         param_count;
    bool        is_variadic;     // not used yet; reserved
} LirCalleeDecl;

// Module-level global. Lowered for `pub x! = ...` and friends.
typedef struct {
    const char *name;
    LirType     type;
    bool        has_const_init;
    long long   const_init;     // for I64/I32/I8/I1 immediate inits
    bool        is_external;    // declared elsewhere (cross-module pub use)
} LirGlobal;

struct LirModule {
    Arena      *arena;
    LirString  *strings;
    int         string_count;
    int         string_capacity;
    LirFunc   **funcs;
    int         func_count;
    int         func_capacity;
    LirCalleeDecl *callees;
    int         callee_count;
    int         callee_capacity;
    LirGlobal  *globals;
    int         global_count;
    int         global_capacity;
    // Phase 4A: tracks whether the module emits any MEMCPY ops, so the
    // emitter knows to declare @llvm.memcpy.p0.p0.i64.
    bool        uses_memcpy;
};

// ---- Construction helpers --------------------------------------------------

LirModule *lir_module_new(Arena *arena);
int        lir_intern_string(LirModule *mod, const char *bytes, int byte_count);

// Register a module-level global. Idempotent on name (first wins).
void       lir_module_add_global(LirModule *mod, const char *name, LirType type,
                                  bool has_const_init, long long const_init,
                                  bool is_external);

// Register a runtime helper signature. Idempotent on (name, signature) —
// duplicate calls with matching params are a no-op. Mismatched signatures
// for the same name fire mix_error.
void       lir_register_callee(LirModule *mod, SrcLoc loc, const char *name,
                                LirType return_type, LirType *param_types,
                                int param_count);

LirFunc   *lir_module_add_func(LirModule *mod, const char *name,
                                bool is_main, LirType return_type);
void       lir_func_add_param(LirFunc *fn, const char *name, LirType type,
                               bool is_mutable);

int        lir_func_new_value(LirFunc *fn);
int        lir_func_new_label(LirFunc *fn);

// Emit helpers. Each appends to fn->instrs and returns the result SSA id
// (or -1 if the op produces no value). Any helper that ends a basic block
// (RET, BR, BR_COND) requires the next emit to be LABEL or it will
// produce invalid LLVM IR — lowering enforces this.

int  lir_emit_call    (LirFunc *fn, SrcLoc loc, const char *callee,
                       LirType result_type, LirOpnd *args, int arg_count);

// Indirect call: callee is a function-pointer operand (LIR_OPND_FN_REF or a
// loaded value). param_types describes the call signature so the emitter
// can render a typed `call` instruction.
int  lir_emit_call_indirect(LirFunc *fn, SrcLoc loc, LirOpnd callee_ptr,
                              LirType result_type, LirType *param_types,
                              LirOpnd *args, int arg_count);

void lir_emit_ret_void (LirFunc *fn, SrcLoc loc);
void lir_emit_ret_value(LirFunc *fn, SrcLoc loc, LirOpnd value);

int  lir_emit_alloca   (LirFunc *fn, SrcLoc loc, LirType type);
int  lir_emit_alloca_bytes(LirFunc *fn, SrcLoc loc, int byte_size, int byte_align);
int  lir_emit_load     (LirFunc *fn, SrcLoc loc, LirType type, LirOpnd addr);
void lir_emit_store    (LirFunc *fn, SrcLoc loc, LirType type,
                        LirOpnd value, LirOpnd addr);
int  lir_emit_ptr_offset(LirFunc *fn, SrcLoc loc, LirOpnd base, int byte_offset);
void lir_emit_memcpy   (LirFunc *fn, SrcLoc loc, LirOpnd dst, LirOpnd src,
                         int byte_size, int byte_align);

int  lir_emit_bin      (LirFunc *fn, SrcLoc loc, LirBinOp op,
                        LirType operand_type, LirOpnd a, LirOpnd b);
int  lir_emit_un       (LirFunc *fn, SrcLoc loc, LirUnOp op,
                        LirType operand_type, LirOpnd a);

int  lir_emit_conv     (LirFunc *fn, SrcLoc loc, LirConvKind kind,
                        LirType src_type, LirType dst_type, LirOpnd a);

void lir_emit_br       (LirFunc *fn, SrcLoc loc, int target_label);
void lir_emit_br_cond  (LirFunc *fn, SrcLoc loc, LirOpnd cond,
                        int then_label, int else_label);
void lir_emit_label    (LirFunc *fn, SrcLoc loc, int label_id);

// Operand constructors.
LirOpnd lir_opnd_string    (int string_id);
LirOpnd lir_opnd_param     (int param_index, LirType type);
LirOpnd lir_opnd_value     (int value_id, LirType type);
LirOpnd lir_opnd_i64       (long long imm);
LirOpnd lir_opnd_int_typed (long long imm, LirType type);
LirOpnd lir_opnd_f64       (double imm);
LirOpnd lir_opnd_bool      (bool imm);
LirOpnd lir_opnd_none      (void);
LirOpnd lir_opnd_fn_ref    (const char *name);  // ptr-typed; name MUST be arena-owned

#endif // LIR_H
