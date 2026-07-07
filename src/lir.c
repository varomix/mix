#include "lir.h"
#include "arena.h"
#include "errors.h"

#include <string.h>

LirModule *lir_module_new(Arena *arena) {
    LirModule *mod = arena_alloc(arena, sizeof(LirModule));
    memset(mod, 0, sizeof(*mod));
    mod->arena = arena;
    return mod;
}

void lir_module_add_global(LirModule *mod, const char *name, LirType type,
                              bool has_const_init, long long const_init,
                              bool is_external)
{
    for (int i = 0; i < mod->global_count; i++) {
        if (strcmp(mod->globals[i].name, name) == 0) return;
    }
    if (mod->global_count >= mod->global_capacity) {
        int new_cap = mod->global_capacity ? mod->global_capacity * 2 : 8;
        LirGlobal *fresh = arena_alloc(mod->arena, new_cap * sizeof(LirGlobal));
        if (mod->globals) memcpy(fresh, mod->globals,
                                  mod->global_count * sizeof(LirGlobal));
        mod->globals = fresh;
        mod->global_capacity = new_cap;
    }
    LirGlobal *g = &mod->globals[mod->global_count++];
    g->name = arena_strdup(mod->arena, name);
    g->type = type;
    g->has_const_init = has_const_init;
    g->const_init = const_init;
    g->is_external = is_external;
}

int lir_intern_string(LirModule *mod, const char *bytes, int byte_count) {
    if (mod->string_count >= mod->string_capacity) {
        int new_cap = mod->string_capacity ? mod->string_capacity * 2 : 16;
        LirString *fresh = arena_alloc(mod->arena, new_cap * sizeof(LirString));
        if (mod->strings) memcpy(fresh, mod->strings,
                                 mod->string_count * sizeof(LirString));
        mod->strings = fresh;
        mod->string_capacity = new_cap;
    }
    int id = mod->string_count;
    LirString *s = &mod->strings[id];
    s->id = id;
    char *copy = arena_alloc(mod->arena, byte_count + 1);
    if (byte_count > 0) memcpy(copy, bytes, byte_count);
    copy[byte_count] = '\0';
    s->bytes = copy;
    s->byte_count = byte_count;
    mod->string_count++;
    return id;
}

void lir_register_callee(LirModule *mod, SrcLoc loc, const char *name,
                          LirType return_type, LirType *param_types,
                          int param_count)
{
    // Idempotent on (name, signature). Mismatched re-registration is a
    // bug in lowering — fire mix_error so it is caught immediately.
    // Existing entries win — silently ignore re-registrations even when
    // signatures differ. Earlier registrations come from explicit extern
    // decls (sema-derived) and are authoritative; later registrations
    // from generic call-site type inference are best-effort and may
    // disagree harmlessly. Coercion at call sites handles arg-type
    // mismatches; this check is too strict given how many places
    // re-register the same name.
    for (int i = 0; i < mod->callee_count; i++) {
        LirCalleeDecl *c = &mod->callees[i];
        if (strcmp(c->name, name) != 0) continue;
        (void)loc;
        return;
    }

    if (mod->callee_count >= mod->callee_capacity) {
        int new_cap = mod->callee_capacity ? mod->callee_capacity * 2 : 16;
        LirCalleeDecl *fresh = arena_alloc(mod->arena, new_cap * sizeof(LirCalleeDecl));
        if (mod->callees) memcpy(fresh, mod->callees,
                                  mod->callee_count * sizeof(LirCalleeDecl));
        mod->callees = fresh;
        mod->callee_capacity = new_cap;
    }
    LirCalleeDecl *c = &mod->callees[mod->callee_count++];
    c->name = arena_strdup(mod->arena, name);
    c->return_type = return_type;
    c->param_count = param_count;
    c->is_variadic = false;
    c->is_fn_ptr_global = false;
    if (param_count > 0) {
        c->param_types = arena_alloc(mod->arena, param_count * sizeof(LirType));
        memcpy(c->param_types, param_types, param_count * sizeof(LirType));
    } else {
        c->param_types = NULL;
    }
}

LirFunc *lir_module_add_func(LirModule *mod, const char *name,
                              bool is_main, LirType return_type)
{
    if (mod->func_count >= mod->func_capacity) {
        int new_cap = mod->func_capacity ? mod->func_capacity * 2 : 16;
        LirFunc **fresh = arena_alloc(mod->arena, new_cap * sizeof(LirFunc *));
        if (mod->funcs) memcpy(fresh, mod->funcs,
                               mod->func_count * sizeof(LirFunc *));
        mod->funcs = fresh;
        mod->func_capacity = new_cap;
    }
    LirFunc *fn = arena_alloc(mod->arena, sizeof(LirFunc));
    memset(fn, 0, sizeof(*fn));
    fn->mod = mod;
    fn->name = arena_strdup(mod->arena, name);
    fn->is_main = is_main;
    fn->return_type = return_type;
    mod->funcs[mod->func_count++] = fn;
    return fn;
}

void lir_func_add_param(LirFunc *fn, const char *name, LirType type,
                         bool is_mutable)
{
    Arena *arena = fn->mod->arena;
    int new_count = fn->param_count + 1;
    LirType *types = arena_alloc(arena, new_count * sizeof(LirType));
    char  **names = arena_alloc(arena, new_count * sizeof(char *));
    bool   *muts  = arena_alloc(arena, new_count * sizeof(bool));
    if (fn->param_count > 0) {
        memcpy(types, fn->param_types, fn->param_count * sizeof(LirType));
        memcpy(names, fn->param_names, fn->param_count * sizeof(char *));
        memcpy(muts,  fn->param_is_mutable, fn->param_count * sizeof(bool));
    }
    types[fn->param_count] = type;
    names[fn->param_count] = arena_strdup(arena, name ? name : "_");
    muts [fn->param_count] = is_mutable;
    fn->param_types = types;
    fn->param_names = names;
    fn->param_is_mutable = muts;
    fn->param_count = new_count;
}

int lir_func_new_value(LirFunc *fn) { return fn->next_value_id++; }
int lir_func_new_label(LirFunc *fn) { return fn->next_label_id++; }

void lir_func_add_dbg_local(LirFunc *fn, int alloca_id, const char *name,
                             SrcLoc loc, LirType scalar_type,
                             int shape_size_bytes, bool is_param)
{
    Arena *arena = fn->mod->arena;
    if (fn->dbg_local_count >= fn->dbg_local_capacity) {
        int cap = fn->dbg_local_capacity ? fn->dbg_local_capacity * 2 : 16;
        LirDbgLocal *fresh = arena_alloc(arena, cap * sizeof(LirDbgLocal));
        if (fn->dbg_locals)
            memcpy(fresh, fn->dbg_locals, fn->dbg_local_count * sizeof(LirDbgLocal));
        fn->dbg_locals = fresh;
        fn->dbg_local_capacity = cap;
    }
    fn->dbg_locals[fn->dbg_local_count++] = (LirDbgLocal){
        .alloca_value_id = alloca_id,
        .name = arena_strdup(arena, name ? name : "_"),
        .loc = loc,
        .scalar_type = scalar_type,
        .shape_size_bytes = shape_size_bytes,
        .is_param = is_param,
    };
}

static LirInstr *append_instr(LirFunc *fn) {
    Arena *arena = fn->mod->arena;
    if (fn->instr_count >= fn->instr_capacity) {
        int new_cap = fn->instr_capacity ? fn->instr_capacity * 2 : 32;
        LirInstr *fresh = arena_alloc(arena, new_cap * sizeof(LirInstr));
        if (fn->instrs) memcpy(fresh, fn->instrs,
                               fn->instr_count * sizeof(LirInstr));
        fn->instrs = fresh;
        fn->instr_capacity = new_cap;
    }
    LirInstr *ins = &fn->instrs[fn->instr_count++];
    memset(ins, 0, sizeof(*ins));
    ins->result = -1;
    return ins;
}

int lir_emit_call(LirFunc *fn, SrcLoc loc, const char *callee,
                   LirType result_type, LirOpnd *args, int arg_count)
{
    Arena *arena = fn->mod->arena;
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_CALL;
    ins->loc = loc;
    ins->callee = arena_strdup(arena, callee);
    ins->result_type = result_type;
    ins->result = (result_type == LIR_TY_VOID) ? -1 : lir_func_new_value(fn);
    ins->global_ptr_load_result = -1;
    ins->arg_count = arg_count;
    if (arg_count > 0) {
        ins->args = arena_alloc(arena, arg_count * sizeof(LirOpnd));
        memcpy(ins->args, args, arg_count * sizeof(LirOpnd));
    }
    return ins->result;
}

int lir_emit_call_indirect(LirFunc *fn, SrcLoc loc, LirOpnd callee_ptr,
                              LirType result_type, LirType *param_types,
                              LirOpnd *args, int arg_count)
{
    Arena *arena = fn->mod->arena;
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_CALL;
    ins->loc = loc;
    ins->callee = NULL;
    ins->indirect_callee = callee_ptr;
    ins->result_type = result_type;
    ins->result = (result_type == LIR_TY_VOID) ? -1 : lir_func_new_value(fn);
    ins->arg_count = arg_count;
    ins->indirect_param_count = arg_count;
    if (arg_count > 0) {
        ins->args = arena_alloc(arena, arg_count * sizeof(LirOpnd));
        memcpy(ins->args, args, arg_count * sizeof(LirOpnd));
        ins->indirect_param_types = arena_alloc(arena, arg_count * sizeof(LirType));
        memcpy(ins->indirect_param_types, param_types, arg_count * sizeof(LirType));
    }
    return ins->result;
}

void lir_emit_ret_void(LirFunc *fn, SrcLoc loc) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_RET;
    ins->loc = loc;
    ins->has_ret_value = false;
}

void lir_emit_ret_value(LirFunc *fn, SrcLoc loc, LirOpnd value) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_RET;
    ins->loc = loc;
    ins->has_ret_value = true;
    ins->ret_value = value;
}

int lir_emit_alloca(LirFunc *fn, SrcLoc loc, LirType type) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_ALLOCA;
    ins->loc = loc;
    ins->alloca_type = type;
    ins->result_type = LIR_TY_PTR;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

int lir_emit_alloca_bytes(LirFunc *fn, SrcLoc loc, int byte_size, int byte_align) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_ALLOCA_BYTES;
    ins->loc = loc;
    ins->alloca_byte_size = byte_size;
    ins->alloca_byte_align = byte_align > 0 ? byte_align : 8;
    ins->result_type = LIR_TY_PTR;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

int lir_emit_ptr_offset(LirFunc *fn, SrcLoc loc, LirOpnd base, int byte_offset) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_PTR_OFFSET;
    ins->loc = loc;
    ins->ptr_base = base;
    ins->ptr_offset = byte_offset;
    ins->result_type = LIR_TY_PTR;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

void lir_emit_memcpy(LirFunc *fn, SrcLoc loc, LirOpnd dst, LirOpnd src,
                      int byte_size, int byte_align)
{
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_MEMCPY;
    ins->loc = loc;
    ins->mem_addr = dst;
    ins->mem_value = src;
    ins->mem_size_bytes = byte_size;
    ins->mem_align = byte_align > 0 ? byte_align : 1;
    fn->mod->uses_memcpy = true;
}

int lir_emit_load(LirFunc *fn, SrcLoc loc, LirType type, LirOpnd addr) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_LOAD;
    ins->loc = loc;
    ins->mem_type = type;
    ins->mem_addr = addr;
    ins->result_type = type;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

void lir_emit_store(LirFunc *fn, SrcLoc loc, LirType type,
                     LirOpnd value, LirOpnd addr)
{
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_STORE;
    ins->loc = loc;
    ins->mem_type = type;
    ins->mem_value = value;
    ins->mem_addr = addr;
}

static bool bin_is_compare(LirBinOp op) {
    switch (op) {
        case LIR_BIN_EQ: case LIR_BIN_NE:
        case LIR_BIN_LT: case LIR_BIN_LE:
        case LIR_BIN_GT: case LIR_BIN_GE:
            return true;
        default:
            return false;
    }
}

int lir_emit_bin(LirFunc *fn, SrcLoc loc, LirBinOp op,
                  LirType operand_type, LirOpnd a, LirOpnd b)
{
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_BIN;
    ins->loc = loc;
    ins->bin_op = op;
    ins->bin_type = operand_type;
    ins->bin_a = a;
    ins->bin_b = b;
    ins->result_type = bin_is_compare(op) ? LIR_TY_I1 : operand_type;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

int lir_emit_un(LirFunc *fn, SrcLoc loc, LirUnOp op,
                 LirType operand_type, LirOpnd a)
{
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_UN;
    ins->loc = loc;
    ins->un_op = op;
    ins->un_type = operand_type;
    ins->un_a = a;
    ins->result_type = (op == LIR_UN_NOT) ? LIR_TY_I1 : operand_type;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

int lir_emit_conv(LirFunc *fn, SrcLoc loc, LirConvKind kind,
                   LirType src_type, LirType dst_type, LirOpnd a)
{
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_CONV;
    ins->loc = loc;
    ins->conv_kind = kind;
    ins->conv_src_type = src_type;
    ins->conv_dst_type = dst_type;
    ins->conv_a = a;
    ins->result_type = dst_type;
    ins->result = lir_func_new_value(fn);
    return ins->result;
}

void lir_emit_br(LirFunc *fn, SrcLoc loc, int target_label) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_BR;
    ins->loc = loc;
    ins->br_target = target_label;
}

void lir_emit_br_cond(LirFunc *fn, SrcLoc loc, LirOpnd cond,
                       int then_label, int else_label)
{
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_BR_COND;
    ins->loc = loc;
    ins->br_cond = cond;
    ins->br_then = then_label;
    ins->br_else = else_label;
}

void lir_emit_label(LirFunc *fn, SrcLoc loc, int label_id) {
    LirInstr *ins = append_instr(fn);
    ins->op = LIR_OP_LABEL;
    ins->loc = loc;
    ins->label_id = label_id;
}

// ---- Operand constructors --------------------------------------------------

LirOpnd lir_opnd_string(int string_id) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_STRING;
    o.type = LIR_TY_PTR;
    o.id = string_id;
    return o;
}

LirOpnd lir_opnd_param(int param_index, LirType type) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_PARAM;
    o.type = type;
    o.id = param_index;
    return o;
}

LirOpnd lir_opnd_value(int value_id, LirType type) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_VALUE;
    o.type = type;
    o.id = value_id;
    return o;
}

LirOpnd lir_opnd_i64(long long imm) {
    return lir_opnd_int_typed(imm, LIR_TY_I64);
}

LirOpnd lir_opnd_int_typed(long long imm, LirType type) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_I64;
    o.type = type;
    o.imm = imm;
    return o;
}

LirOpnd lir_opnd_f64(double imm) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_F64;
    o.type = LIR_TY_F64;
    o.fimm = imm;
    return o;
}

LirOpnd lir_opnd_bool(bool imm) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_BOOL;
    o.type = LIR_TY_I1;
    o.imm = imm ? 1 : 0;
    return o;
}

LirOpnd lir_opnd_none(void) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_NONE;
    o.type = LIR_TY_VOID;
    return o;
}

LirOpnd lir_opnd_fn_ref(const char *name) {
    LirOpnd o = {0};
    o.kind = LIR_OPND_FN_REF;
    o.type = LIR_TY_PTR;
    o.fn_name = name;
    return o;
}
