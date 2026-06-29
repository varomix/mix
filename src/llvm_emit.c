#include "llvm_emit.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// Phase 3 LLVM emitter
//
// Renders a LirModule to textual LLVM IR. All language-level decisions
// live in lowering — this file only knows how to render LIR.
//
// Supported (Phase 3):
//   ALLOCA / LOAD / STORE / CALL / RET
//   BIN (arith + cmp), UN (neg, not), CONV (sext/zext/trunc/sitofp/fptosi)
//   BR / BR_COND / LABEL
//
// Phase 4A/4B will add aggregate ops (memcpy for shape copies,
// field-address GEPs, larger-than-scalar params/returns).
// ============================================================================

LlvmEmitter llvm_emitter_create(FILE *out) {
    LlvmEmitter e = {0};
    e.out = out;
    return e;
}

// ---- Debug-info bookkeeping (Phase 6) -------------------------------------

static char *dbg_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

// Split `path` into (dir, base) using the last '/' or '\\'. If `path` has
// no separator, returns dir="." and base=path. Both halves are
// freshly-allocated; caller frees.
static void dbg_split_path(const char *path, char **dir_out, char **base_out) {
    if (!path || !*path) { *dir_out = dbg_strdup("."); *base_out = dbg_strdup("?"); return; }
    const char *slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') slash = p;
    if (!slash) {
        *dir_out = dbg_strdup(".");
        *base_out = dbg_strdup(path);
        return;
    }
    int dir_len = (int)(slash - path);
    char *dir = malloc(dir_len + 1);
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';
    *dir_out = dir;
    *base_out = dbg_strdup(slash + 1);
}

// Make `path` absolute. If it's already absolute, return a copy. If not,
// prepend cwd. Caller frees.
static char *dbg_absolute(const char *path) {
    if (!path || !*path) return dbg_strdup(".");
    if (path[0] == '/') return dbg_strdup(path);
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return dbg_strdup(path);
    size_t need = strlen(cwd) + 1 + strlen(path) + 1;
    char *p = malloc(need);
    snprintf(p, need, "%s/%s", cwd, path);
    return p;
}

static int dbg_next_id(LlvmEmitter *e) { return e->next_md_id++; }

// Intern a !DIFile entry by the (filename, directory) pair derived from
// `source_path`. Returns the metadata id. New entry on first occurrence.
static int dbg_file_for(LlvmEmitter *e, const char *source_path) {
    if (!source_path) source_path = e->unit_filename ? e->unit_filename : "<unknown>";
    char *dir = NULL; char *base = NULL;
    if (source_path[0] == '/') {
        dbg_split_path(source_path, &dir, &base);
    } else {
        char *abs = dbg_absolute(source_path);
        dbg_split_path(abs, &dir, &base);
        free(abs);
    }
    for (int i = 0; i < e->file_count; i++) {
        if (strcmp(e->files[i].filename, base) == 0 &&
            strcmp(e->files[i].directory, dir) == 0) {
            free(dir); free(base);
            return e->files[i].file_md_id;
        }
    }
    if (e->file_count >= e->file_capacity) {
        int cap = e->file_capacity ? e->file_capacity * 2 : 8;
        e->files = realloc(e->files, cap * sizeof(*e->files));
        e->file_capacity = cap;
    }
    int id = dbg_next_id(e);
    e->files[e->file_count++] = (LlvmDbgFile){
        .file_md_id = id, .filename = base, .directory = dir,
    };
    return id;
}

static int dbg_record_subprogram(LlvmEmitter *e, const char *fn_name,
                                  int file_md_id, int line) {
    if (e->fn_count >= e->fn_capacity) {
        int cap = e->fn_capacity ? e->fn_capacity * 2 : 16;
        e->fns = realloc(e->fns, cap * sizeof(*e->fns));
        e->fn_capacity = cap;
    }
    int sp = dbg_next_id(e);
    int sty = dbg_next_id(e);
    e->fns[e->fn_count++] = (LlvmDbgFn){
        .sp_md_id = sp, .subroutine_type_md_id = sty,
        .file_md_id = file_md_id, .line = line,
        .name = dbg_strdup(fn_name),
    };
    return sp;
}

static int dbg_record_loc(LlvmEmitter *e, int line, int col, int scope_md_id) {
    if (e->loc_count >= e->loc_capacity) {
        int cap = e->loc_capacity ? e->loc_capacity * 2 : 256;
        e->locs = realloc(e->locs, cap * sizeof(*e->locs));
        e->loc_capacity = cap;
    }
    int id = dbg_next_id(e);
    e->locs[e->loc_count++] = (LlvmDbgLoc){
        .md_id = id, .line = line, .col = col, .scope_md_id = scope_md_id,
    };
    return id;
}

// Intern a !DIBasicType (or shape blob) by (lir_type, shape_size). Scalars
// share by lir_type; shape blobs share by size only since we don't yet
// emit field-level debug info for them.
static int dbg_basic_type_for(LlvmEmitter *e, LirType lt, int shape_size) {
    for (int i = 0; i < e->basic_type_count; i++) {
        const LlvmDbgBasicType *b = &e->basic_types[i];
        if (b->lir_type == lt && b->shape_size_bytes == shape_size) return b->md_id;
    }
    if (e->basic_type_count >= e->basic_type_capacity) {
        int cap = e->basic_type_capacity ? e->basic_type_capacity * 2 : 16;
        e->basic_types = realloc(e->basic_types, cap * sizeof(*e->basic_types));
        e->basic_type_capacity = cap;
    }
    int id = dbg_next_id(e);
    e->basic_types[e->basic_type_count++] = (LlvmDbgBasicType){
        .md_id = id, .lir_type = lt, .shape_size_bytes = shape_size,
    };
    return id;
}

static int dbg_record_var(LlvmEmitter *e, const char *name, int scope_md_id,
                           int file_md_id, int line, int type_md_id, int arg_index) {
    if (e->var_count >= e->var_capacity) {
        int cap = e->var_capacity ? e->var_capacity * 2 : 64;
        e->vars = realloc(e->vars, cap * sizeof(*e->vars));
        e->var_capacity = cap;
    }
    int id = dbg_next_id(e);
    e->vars[e->var_count++] = (LlvmDbgVar){
        .md_id = id, .scope_md_id = scope_md_id, .file_md_id = file_md_id,
        .line = line, .type_md_id = type_md_id,
        .arg_index = arg_index,
        .name = name,                  // arena-owned, stable for module lifetime
    };
    return id;
}

void llvm_emitter_enable_debug(LlvmEmitter *e, const char *source_path) {
    e->debug = true;
    // Keep the path as the LIR/loc records will use it. dbg_file_for will
    // intern it; both the !DICompileUnit and per-instruction !DILocations
    // share the same !DIFile that way.
    e->unit_filename = source_path ? source_path : "<unknown>";
    e->next_md_id = 0;
}

// ---- Type and operand rendering -------------------------------------------

static const char *llvm_type(LirType t) {
    switch (t) {
        case LIR_TY_VOID: return "void";
        case LIR_TY_I1:   return "i1";
        case LIR_TY_I8:   return "i8";
        case LIR_TY_I32:  return "i32";
        case LIR_TY_I64:  return "i64";
        case LIR_TY_F32:  return "float";
        case LIR_TY_F64:  return "double";
        case LIR_TY_PTR:  return "ptr";
    }
    return "void";
}

// Render a parameter reference. Lower spilled every param to an alloca,
// so this name only ever appears as the source of a STORE at function
// entry. We use %p<idx> to keep params separate from %t<id> SSA temps.
static void render_param(FILE *out, int idx) {
    fprintf(out, "%%p%d", idx);
}

// Render a double constant. LLVM IR requires double constants in
// hexadecimal IEEE-754 bit form (NOT C99 hex-float format) — the
// expected syntax is `0x` followed by 16 hex digits encoding the
// raw 64-bit pattern. `as_f32` rounds through float first so the
// emitted hex represents an f32-representable double (LLVM rejects
// any f64 hex that isn't exactly representable as f32 when it's
// being assigned to a `float` slot).
static void render_float(FILE *out, double v, bool as_f32) {
    if (as_f32) v = (double)(float)v;
    union { double d; unsigned long long u; } pun;
    pun.d = v;
    fprintf(out, "0x%016llX", pun.u);
}

static void render_operand(FILE *out, LirOpnd op) {
    switch (op.kind) {
        case LIR_OPND_STRING: fprintf(out, "@.mix_str_%d", op.id); break;
        case LIR_OPND_PARAM:  render_param(out, op.id);            break;
        case LIR_OPND_VALUE:  fprintf(out, "%%t%d", op.id);        break;
        case LIR_OPND_I64:
            // 0 with ptr type renders as `null` — needed when MIX `none`
            // appears in a ptr-typed slot (e.g. `str?` return).
            if (op.type == LIR_TY_PTR && op.imm == 0) fputs("null", out);
            else fprintf(out, "%lld", op.imm);
            break;
        case LIR_OPND_BOOL:   fputs(op.imm ? "1" : "0", out);      break;
        case LIR_OPND_F64:    render_float(out, op.fimm, op.type == LIR_TY_F32); break;
        case LIR_OPND_NONE:   fputs("undef", out);                 break;
        case LIR_OPND_FN_REF: fprintf(out, "@%s", op.fn_name);     break;
    }
}

// ---- String globals --------------------------------------------------------

static void emit_string_global(FILE *out, const LirString *s) {
    // Resolve MIX escape sequences (`\n`, `\t`, `\\`, `\"`, `\r`, `\0`)
    // before counting bytes so the LLVM `[N x i8]` length matches the
    // emitted constant. Lexer keeps the raw source bytes; QBE emits its
    // own escape syntax that asm preserves, but LLVM IR encodes
    // non-printables as `\HH` hex pairs, so we have to translate.
    int n = s->byte_count;
    int decoded_len = 0;
    for (int i = 0; i < n; i++) {
        if (s->bytes[i] == '\\' && i + 1 < n) {
            char nx = s->bytes[i+1];
            if (nx == 'n' || nx == 't' || nx == 'r' || nx == '0' ||
                nx == '\\' || nx == '"') { decoded_len++; i++; continue; }
        }
        decoded_len++;
    }
    int total = decoded_len + 1;
    fprintf(out, "@.mix_str_%d = private unnamed_addr constant [%d x i8] c\"",
            s->id, total);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s->bytes[i];
        if (c == '\\' && i + 1 < n) {
            char nx = s->bytes[i+1];
            unsigned char out_c = c;
            bool ate = true;
            switch (nx) {
                case 'n':  out_c = '\n'; break;
                case 't':  out_c = '\t'; break;
                case 'r':  out_c = '\r'; break;
                case '0':  out_c = '\0'; break;
                case '\\': out_c = '\\'; break;
                case '"':  out_c = '"';  break;
                default:   ate = false; break;
            }
            if (ate) { i++; c = out_c; }
        }
        if (c == '\\' || c == '"') {
            fprintf(out, "\\%02X", c);
        } else if (c >= 0x20 && c < 0x7F) {
            fputc(c, out);
        } else {
            fprintf(out, "\\%02X", c);
        }
    }
    fputs("\\00\"\n", out);
}

// ---- Per-instruction emission ---------------------------------------------

static void emit_call(FILE *out, const LirInstr *ins) {
    bool indirect = (ins->indirect_callee.kind != LIR_OPND_NONE);
    fputs("  ", out);
    if (indirect) {
        // Indirect call: `%t = call <ret> %fp(args)` or `call void %fp(args)`
        if (ins->result >= 0) {
            fprintf(out, "%%t%d = call %s ", ins->result, llvm_type(ins->result_type));
        } else {
            fputs("call void ", out);
        }
        render_operand(out, ins->indirect_callee);
        fputc('(', out);
    } else if (ins->result >= 0) {
        fprintf(out, "%%t%d = call %s @%s(",
                ins->result, llvm_type(ins->result_type), ins->callee);
    } else {
        fprintf(out, "call void @%s(", ins->callee);
    }
    for (int i = 0; i < ins->arg_count; i++) {
        if (i > 0) fputs(", ", out);
        fprintf(out, "%s ", llvm_type(ins->args[i].type));
        render_operand(out, ins->args[i]);
    }
    fputs(")\n", out);
}

static void emit_ret(FILE *out, const LirInstr *ins) {
    if (!ins->has_ret_value) { fputs("  ret void\n", out); return; }
    fprintf(out, "  ret %s ", llvm_type(ins->ret_value.type));
    render_operand(out, ins->ret_value);
    fputc('\n', out);
}

static void emit_alloca(FILE *out, const LirInstr *ins) {
    fprintf(out, "  %%t%d = alloca %s\n",
            ins->result, llvm_type(ins->alloca_type));
}

static void emit_alloca_bytes(FILE *out, const LirInstr *ins) {
    // [N x i8] keeps the type system happy without inventing a new
    // LIR-level "byte array" scalar type. The align attribute carries
    // the shape's required alignment.
    fprintf(out, "  %%t%d = alloca [%d x i8], align %d\n",
            ins->result, ins->alloca_byte_size, ins->alloca_byte_align);
}

static void emit_ptr_offset(FILE *out, const LirInstr *ins) {
    fprintf(out, "  %%t%d = getelementptr inbounds i8, ptr ", ins->result);
    render_operand(out, ins->ptr_base);
    fprintf(out, ", i64 %d\n", ins->ptr_offset);
}

static void emit_memcpy(FILE *out, const LirInstr *ins) {
    fprintf(out, "  call void @llvm.memcpy.p0.p0.i64(ptr align %d ", ins->mem_align);
    render_operand(out, ins->mem_addr);
    fprintf(out, ", ptr align %d ", ins->mem_align);
    render_operand(out, ins->mem_value);
    fprintf(out, ", i64 %d, i1 false)\n", ins->mem_size_bytes);
}

static void emit_load(FILE *out, const LirInstr *ins) {
    fprintf(out, "  %%t%d = load %s, ptr ",
            ins->result, llvm_type(ins->mem_type));
    render_operand(out, ins->mem_addr);
    fputc('\n', out);
}

static void emit_store(FILE *out, const LirInstr *ins) {
    fprintf(out, "  store %s ", llvm_type(ins->mem_type));
    render_operand(out, ins->mem_value);
    fputs(", ptr ", out);
    render_operand(out, ins->mem_addr);
    fputc('\n', out);
}

static const char *bin_arith_int_op(LirBinOp op) {
    switch (op) {
        case LIR_BIN_ADD: return "add";
        case LIR_BIN_SUB: return "sub";
        case LIR_BIN_MUL: return "mul";
        case LIR_BIN_DIV: return "sdiv";
        case LIR_BIN_MOD: return "srem";
        case LIR_BIN_AND: return "and";
        case LIR_BIN_OR:  return "or";
        default: return NULL;
    }
}

static const char *bin_arith_float_op(LirBinOp op) {
    switch (op) {
        case LIR_BIN_ADD: return "fadd";
        case LIR_BIN_SUB: return "fsub";
        case LIR_BIN_MUL: return "fmul";
        case LIR_BIN_DIV: return "fdiv";
        case LIR_BIN_MOD: return "frem";
        default: return NULL;
    }
}

static const char *icmp_pred(LirBinOp op) {
    switch (op) {
        case LIR_BIN_EQ: return "eq";
        case LIR_BIN_NE: return "ne";
        case LIR_BIN_LT: return "slt";
        case LIR_BIN_LE: return "sle";
        case LIR_BIN_GT: return "sgt";
        case LIR_BIN_GE: return "sge";
        default: return NULL;
    }
}

static const char *fcmp_pred(LirBinOp op) {
    switch (op) {
        case LIR_BIN_EQ: return "oeq";
        case LIR_BIN_NE: return "one";
        case LIR_BIN_LT: return "olt";
        case LIR_BIN_LE: return "ole";
        case LIR_BIN_GT: return "ogt";
        case LIR_BIN_GE: return "oge";
        default: return NULL;
    }
}

static bool ty_is_int(LirType t) {
    return t == LIR_TY_I1 || t == LIR_TY_I8 || t == LIR_TY_I32 || t == LIR_TY_I64;
}

static void emit_bin(FILE *out, const LirInstr *ins) {
    bool is_cmp = (ins->bin_op >= LIR_BIN_EQ && ins->bin_op <= LIR_BIN_GE);
    bool is_float = (ins->bin_type == LIR_TY_F64 || ins->bin_type == LIR_TY_F32);

    // Pointer comparisons are valid (e.g., compare two refs). Force
    // ptr-typed render so an i64 0 immediate prints as `null`.
    if (is_cmp && ins->bin_type == LIR_TY_PTR) {
        LirOpnd a = ins->bin_a;
        LirOpnd b = ins->bin_b;
        if (a.kind == LIR_OPND_I64) a.type = LIR_TY_PTR;
        if (b.kind == LIR_OPND_I64) b.type = LIR_TY_PTR;
        fprintf(out, "  %%t%d = icmp %s ptr ", ins->result, icmp_pred(ins->bin_op));
        render_operand(out, a);
        fputs(", ", out);
        render_operand(out, b);
        fputc('\n', out);
        return;
    }

    if (!is_cmp && !is_float && !ty_is_int(ins->bin_type)) {
        // Fallback for unsupported types (e.g. ptr arithmetic that
        // didn't get rewritten to PTR_OFFSET). Emit a placeholder so
        // the IR remains valid even if semantically wrong; mix_error
        // should have already fired upstream.
        fprintf(out, "  %%t%d = add i64 0, 0  ; FALLBACK for unsupported bin op type\n",
                ins->result);
        return;
    }

    fprintf(out, "  %%t%d = ", ins->result);
    if (is_cmp) {
        fprintf(out, "%s %s ",
                is_float ? "fcmp" : "icmp",
                is_float ? fcmp_pred(ins->bin_op) : icmp_pred(ins->bin_op));
    } else if (is_float) {
        fprintf(out, "%s ", bin_arith_float_op(ins->bin_op));
    } else {
        fprintf(out, "%s ", bin_arith_int_op(ins->bin_op));
    }
    fprintf(out, "%s ", llvm_type(ins->bin_type));
    render_operand(out, ins->bin_a);
    fputs(", ", out);
    render_operand(out, ins->bin_b);
    fputc('\n', out);
}

static void emit_un(FILE *out, const LirInstr *ins) {
    if (ins->un_op == LIR_UN_NEG) {
        if (ins->un_type == LIR_TY_F64 || ins->un_type == LIR_TY_F32) {
            // fneg
            fprintf(out, "  %%t%d = fneg %s ", ins->result, llvm_type(ins->un_type));
            render_operand(out, ins->un_a);
            fputc('\n', out);
        } else {
            // 0 - x for ints
            fprintf(out, "  %%t%d = sub %s 0, ", ins->result, llvm_type(ins->un_type));
            render_operand(out, ins->un_a);
            fputc('\n', out);
        }
    } else { // LIR_UN_NOT
        // For i1 operands: xor with true. For wider ints (e.g. an i32 from
        // a bool-returning runtime), emit `icmp eq <ty>, 0` so the result
        // stays i1.
        if (ins->un_type == LIR_TY_I1) {
            fprintf(out, "  %%t%d = xor i1 ", ins->result);
            render_operand(out, ins->un_a);
            fputs(", true\n", out);
        } else {
            fprintf(out, "  %%t%d = icmp eq %s ", ins->result, llvm_type(ins->un_type));
            render_operand(out, ins->un_a);
            fputs(", 0\n", out);
        }
    }
}

static void emit_conv(FILE *out, const LirInstr *ins) {
    const char *op = "bitcast";
    switch (ins->conv_kind) {
        case LIR_CONV_SEXT:     op = "sext";     break;
        case LIR_CONV_ZEXT:     op = "zext";     break;
        case LIR_CONV_TRUNC:    op = "trunc";    break;
        case LIR_CONV_SITOFP:   op = "sitofp";   break;
        case LIR_CONV_FPTOSI:   op = "fptosi";   break;
        case LIR_CONV_FPEXT:    op = "fpext";    break;
        case LIR_CONV_FPTRUNC:  op = "fptrunc";  break;
        case LIR_CONV_BITCAST:  op = "bitcast";  break;
        case LIR_CONV_PTRTOINT: op = "ptrtoint"; break;
        case LIR_CONV_INTTOPTR: op = "inttoptr"; break;
    }
    fprintf(out, "  %%t%d = %s %s ", ins->result, op, llvm_type(ins->conv_src_type));
    render_operand(out, ins->conv_a);
    fprintf(out, " to %s\n", llvm_type(ins->conv_dst_type));
}

static void emit_br(FILE *out, const LirInstr *ins) {
    fprintf(out, "  br label %%L%d\n", ins->br_target);
}

static void emit_br_cond(FILE *out, const LirInstr *ins) {
    fputs("  br i1 ", out);
    render_operand(out, ins->br_cond);
    fprintf(out, ", label %%L%d, label %%L%d\n", ins->br_then, ins->br_else);
}

static void emit_label(FILE *out, const LirInstr *ins) {
    fprintf(out, "L%d:\n", ins->label_id);
}

static void emit_instr_raw(FILE *out, const LirInstr *ins);

// Emit one instruction to `out`. In debug mode, attach `, !dbg !N` to
// instructions that take a debug location (skip labels, which are block
// headers, and skip allocas that have no source loc — typically the
// entry-block hoisted ones from compiler-synthesized temps).
static void emit_instr(FILE *out, LlvmEmitter *emit, const LirInstr *ins) {
    if (!emit || !emit->debug || ins->op == LIR_OP_LABEL ||
        emit->current_sp_md_id <= 0 || ins->loc.line <= 0) {
        emit_instr_raw(out, ins);
        return;
    }
    // Capture the inner emit, then splice in the dbg suffix before the
    // trailing newline.
    char *buf = NULL; size_t bufsize = 0;
    FILE *mem = open_memstream(&buf, &bufsize);
    if (!mem) { emit_instr_raw(out, ins); return; }
    emit_instr_raw(mem, ins);
    fclose(mem);
    int n = (int)bufsize;
    while (n > 0 && buf[n - 1] == '\n') n--;
    int loc_id = dbg_record_loc(emit, ins->loc.line, ins->loc.col,
                                  emit->current_sp_md_id);
    fwrite(buf, 1, n, out);
    fprintf(out, ", !dbg !%d\n", loc_id);
    free(buf);
}

static void emit_instr_raw(FILE *out, const LirInstr *ins) {
    switch (ins->op) {
        case LIR_OP_CALL:         emit_call        (out, ins); break;
        case LIR_OP_RET:          emit_ret         (out, ins); break;
        case LIR_OP_ALLOCA:       emit_alloca      (out, ins); break;
        case LIR_OP_ALLOCA_BYTES: emit_alloca_bytes(out, ins); break;
        case LIR_OP_LOAD:         emit_load        (out, ins); break;
        case LIR_OP_STORE:        emit_store       (out, ins); break;
        case LIR_OP_PTR_OFFSET:   emit_ptr_offset  (out, ins); break;
        case LIR_OP_MEMCPY:       emit_memcpy      (out, ins); break;
        case LIR_OP_BIN:          emit_bin         (out, ins); break;
        case LIR_OP_UN:           emit_un          (out, ins); break;
        case LIR_OP_CONV:         emit_conv        (out, ins); break;
        case LIR_OP_BR:           emit_br          (out, ins); break;
        case LIR_OP_BR_COND:      emit_br_cond     (out, ins); break;
        case LIR_OP_LABEL:        emit_label       (out, ins); break;
    }
}

// ---- Function emission ----------------------------------------------------

static void emit_function_signature(FILE *out, LlvmEmitter *emit, const LirFunc *fn) {
    if (fn->is_main) {
        if (emit && emit->wasm_main) {
            fputs("define hidden i32 @__main_argc_argv(i32 noundef %argc, ptr noundef %argv)", out);
        } else {
            fputs("define i32 @main(i32 %argc, ptr %argv)", out);
        }
        return;
    }
    fprintf(out, "define %s @%s(", llvm_type(fn->return_type), fn->name);
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) fputs(", ", out);
        fprintf(out, "%s %%p%d", llvm_type(fn->param_types[i]), i);
    }
    fputc(')', out);
}

static void emit_function(FILE *out, LlvmEmitter *emit, const LirFunc *fn) {
    fputs("\n", out);
    emit_function_signature(out, emit, fn);

    // Phase 6: register a !DISubprogram for this function. Use the
    // first instruction with a non-zero loc to pick the file and line.
    // (LIR ops carry source loc; the entry/setup ops sometimes have a
    // zero loc, so scan forward for the first real one.)
    if (emit->debug) {
        int line = 0;
        const char *file = NULL;
        for (int i = 0; i < fn->instr_count; i++) {
            if (fn->instrs[i].loc.line > 0) {
                line = fn->instrs[i].loc.line;
                file = fn->instrs[i].loc.filename;
                break;
            }
        }
        if (line == 0) line = 1;
        int file_md = dbg_file_for(emit, file);
        const char *fname = fn->is_main ? "main" : fn->name;
        emit->current_sp_md_id = dbg_record_subprogram(emit, fname, file_md, line);
        fprintf(out, " !dbg !%d", emit->current_sp_md_id);
    } else {
        emit->current_sp_md_id = 0;
    }
    fputs(" {\n", out);
    fputs("entry:\n", out);

    if (fn->is_main) {
        fputs("  call void @mix_set_args(i32 %argc, ptr %argv)\n", out);
    }

    // Hoist all allocas into the entry block. Without this, allocas
    // emitted inside a loop body keep growing the stack frame on every
    // iteration (the space is only reclaimed at function return), so
    // long-running game loops eventually fault in `___chkstk_darwin`.
    // QBE buffers all allocas into a single @start block for the same
    // reason. Hoisting is safe: alloca operands are constants (type or
    // byte size + alignment), so position doesn't change SSA semantics.
    //
    // Phase 6+: in debug mode, emit `llvm.dbg.declare` after each alloca
    // that has a registered LirDbgLocal — gives lldb's `frame variable`
    // a name + type for the slot. The dbg.declare itself is decorated
    // with the variable's source loc so it doesn't lose its scope.
    int sp_file_md = 0;
    int sp_line = 0;
    if (emit->debug && emit->current_sp_md_id > 0) {
        for (int i = 0; i < emit->fn_count; i++) {
            if (emit->fns[i].sp_md_id == emit->current_sp_md_id) {
                sp_file_md = emit->fns[i].file_md_id;
                sp_line = emit->fns[i].line;
                break;
            }
        }
    }
    int param_seen = 0;
    for (int i = 0; i < fn->instr_count; i++) {
        const LirInstr *ins = &fn->instrs[i];
        if (ins->op == LIR_OP_ALLOCA)             emit_alloca      (out, ins);
        else if (ins->op == LIR_OP_ALLOCA_BYTES)  emit_alloca_bytes(out, ins);
        else continue;

        if (!emit->debug || emit->current_sp_md_id <= 0) continue;
        // Look up a LirDbgLocal for this alloca id.
        const LirDbgLocal *dl = NULL;
        for (int k = 0; k < fn->dbg_local_count; k++) {
            if (fn->dbg_locals[k].alloca_value_id == ins->result) {
                dl = &fn->dbg_locals[k];
                break;
            }
        }
        if (!dl) continue;
        int type_md = dbg_basic_type_for(emit,
            dl->scalar_type != LIR_TY_VOID ? dl->scalar_type : LIR_TY_VOID,
            dl->shape_size_bytes);
        int line = dl->loc.line > 0 ? dl->loc.line : sp_line;
        int col  = dl->loc.col  > 0 ? dl->loc.col  : 1;
        int arg_index = dl->is_param ? ++param_seen : 0;
        int var_md = dbg_record_var(emit, dl->name, emit->current_sp_md_id,
                                      sp_file_md, line, type_md, arg_index);
        int loc_md = dbg_record_loc(emit, line, col, emit->current_sp_md_id);
        fprintf(out,
            "  call void @llvm.dbg.declare(metadata ptr %%t%d, metadata !%d, "
            "metadata !DIExpression()), !dbg !%d\n",
            ins->result, var_md, loc_md);
        emit->needs_dbg_declare_decl = true;
    }

    for (int i = 0; i < fn->instr_count; i++) {
        const LirInstr *ins = &fn->instrs[i];
        if (ins->op == LIR_OP_ALLOCA || ins->op == LIR_OP_ALLOCA_BYTES) continue;
        emit_instr(out, emit, ins);
    }

    fputs("}\n", out);
}

// ---- Module emission ------------------------------------------------------

static void emit_callee_decl(FILE *out, const LirCalleeDecl *c) {
    fprintf(out, "declare %s @%s(", llvm_type(c->return_type), c->name);
    for (int i = 0; i < c->param_count; i++) {
        if (i > 0) fputs(", ", out);
        fputs(llvm_type(c->param_types[i]), out);
    }
    fputs(")\n", out);
}

void llvm_emit_module(LlvmEmitter *emit, LirModule *mod) {
    if (!mod) return;
    FILE *out = emit->out;

    fputs("; MIX LLVM backend\n", out);
    // No `target triple = ...` line: clang -c will fill in the host
    // triple. Hardcoding arm64-apple-darwin tripped clang's
    // -Woverride-module on every macOS build, and was outright wrong
    // for any non-Apple host (Linux x86_64 / aarch64 / Windows).

    if (mod->uses_memcpy) {
        // Standard LLVM memcpy intrinsic — declare once per module when
        // any LIR_OP_MEMCPY appears.
        fputs("declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg)\n\n", out);
    }

    if (emit->debug) {
        // Always declare the dbg intrinsic in --debug mode. It's free if
        // unused — but the per-function alloca-walk decides emission, so
        // declaring up front keeps the order at the top of the file.
        fputs("declare void @llvm.dbg.declare(metadata, metadata, metadata) #0\n", out);
        fputs("attributes #0 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }\n\n", out);
    }

    // Runtime / module-level callee declarations. Lowering registered
    // each one as it was used. User-defined functions in the same module
    // are also defined here, so we should NOT emit a `declare` for them.
    for (int i = 0; i < mod->callee_count; i++) {
        const LirCalleeDecl *c = &mod->callees[i];
        bool is_local_def = false;
        for (int j = 0; j < mod->func_count; j++) {
            if (strcmp(c->name, mod->funcs[j]->name) == 0) {
                is_local_def = true;
                break;
            }
        }
        if (!is_local_def) emit_callee_decl(out, c);
    }
    if (mod->callee_count > 0) fputs("\n", out);

    // Module-level globals.
    for (int i = 0; i < mod->global_count; i++) {
        const LirGlobal *g = &mod->globals[i];
        if (g->is_external) {
            fprintf(out, "@%s = external global %s\n",
                    g->name, llvm_type(g->type));
            continue;
        }
        const char *ty = llvm_type(g->type);
        if (g->type == LIR_TY_PTR) {
            fprintf(out, "@%s = global ptr null\n", g->name);
        } else if (g->type == LIR_TY_F64) {
            fprintf(out, "@%s = global double 0.0\n", g->name);
        } else if (g->type == LIR_TY_F32) {
            fprintf(out, "@%s = global float 0.0\n", g->name);
        } else if (g->has_const_init) {
            fprintf(out, "@%s = global %s %lld\n", g->name, ty, g->const_init);
        } else {
            fprintf(out, "@%s = global %s 0\n", g->name, ty);
        }
    }
    if (mod->global_count > 0) fputs("\n", out);

    // String literal globals.
    for (int i = 0; i < mod->string_count; i++) {
        emit_string_global(out, &mod->strings[i]);
    }

    // Function definitions.
    for (int i = 0; i < mod->func_count; i++) {
        emit_function(out, emit, mod->funcs[i]);
    }

    // Phase 6: debug-info metadata footer. Emit !llvm.module.flags,
    // !llvm.dbg.cu, the !DICompileUnit, !DIFile entries, !DISubroutineType
    // entries, !DISubprogram entries, and !DILocation entries. lldb only
    // needs files + subprograms + locations to step by line; we don't yet
    // emit !DILocalVariable / llvm.dbg.declare for individual locals.
    if (emit->debug) {
        // Reserve module-flag ids and CU id at the top of the namespace.
        int dwarf_ver_id  = dbg_next_id(emit);
        int debug_ver_id  = dbg_next_id(emit);
        int wchar_id      = dbg_next_id(emit);
        int flags_list_id = dbg_next_id(emit);
        int cu_list_id    = dbg_next_id(emit);
        emit->unit_cu_md_id = dbg_next_id(emit);
        int unit_file_id   = dbg_file_for(emit, emit->unit_filename);

        fputs("\n", out);
        fprintf(out, "!llvm.module.flags = !{!%d, !%d, !%d}\n",
                dwarf_ver_id, debug_ver_id, wchar_id);
        fprintf(out, "!llvm.dbg.cu = !{!%d}\n\n", emit->unit_cu_md_id);

        fprintf(out, "!%d = !{i32 7, !\"Dwarf Version\", i32 4}\n", dwarf_ver_id);
        fprintf(out, "!%d = !{i32 2, !\"Debug Info Version\", i32 3}\n", debug_ver_id);
        fprintf(out, "!%d = !{i32 1, !\"wchar_size\", i32 4}\n", wchar_id);
        (void)flags_list_id; (void)cu_list_id;

        fprintf(out,
            "!%d = distinct !DICompileUnit("
            "language: DW_LANG_C99, file: !%d, "
            "producer: \"mix\", isOptimized: false, runtimeVersion: 0, "
            "emissionKind: FullDebug)\n",
            emit->unit_cu_md_id, unit_file_id);

        for (int i = 0; i < emit->file_count; i++) {
            const LlvmDbgFile *f = &emit->files[i];
            fprintf(out, "!%d = !DIFile(filename: \"%s\", directory: \"%s\")\n",
                    f->file_md_id, f->filename, f->directory);
        }

        for (int i = 0; i < emit->fn_count; i++) {
            const LlvmDbgFn *f = &emit->fns[i];
            fprintf(out, "!%d = !DISubroutineType(types: !{null})\n",
                    f->subroutine_type_md_id);
            fprintf(out,
                "!%d = distinct !DISubprogram(name: \"%s\", scope: !%d, "
                "file: !%d, line: %d, type: !%d, scopeLine: %d, "
                "spFlags: DISPFlagDefinition, unit: !%d)\n",
                f->sp_md_id, f->name ? f->name : "?",
                f->file_md_id, f->file_md_id, f->line,
                f->subroutine_type_md_id, f->line, emit->unit_cu_md_id);
        }

        for (int i = 0; i < emit->basic_type_count; i++) {
            const LlvmDbgBasicType *b = &emit->basic_types[i];
            const char *name = "void"; int bits = 0; const char *enc = NULL;
            switch (b->lir_type) {
                case LIR_TY_I1:  name = "bool";    bits = 8;  enc = "DW_ATE_boolean";   break;
                case LIR_TY_I8:  name = "byte";    bits = 8;  enc = "DW_ATE_unsigned";  break;
                case LIR_TY_I32: name = "int32";   bits = 32; enc = "DW_ATE_signed";    break;
                case LIR_TY_I64: name = "int";     bits = 64; enc = "DW_ATE_signed";    break;
                case LIR_TY_F32: name = "float32"; bits = 32; enc = "DW_ATE_float";     break;
                case LIR_TY_F64: name = "float";   bits = 64; enc = "DW_ATE_float";     break;
                case LIR_TY_PTR: name = "ptr";     bits = 64; enc = "DW_ATE_address";   break;
                default: break;
            }
            if (b->shape_size_bytes > 0) {
                // Shape blob: opaque byte array of the right size. lldb
                // shows the address; field-level decode is a future step.
                fprintf(out,
                    "!%d = !DIBasicType(name: \"shape\", size: %d, encoding: DW_ATE_unsigned)\n",
                    b->md_id, b->shape_size_bytes * 8);
            } else if (enc) {
                fprintf(out,
                    "!%d = !DIBasicType(name: \"%s\", size: %d, encoding: %s)\n",
                    b->md_id, name, bits, enc);
            } else {
                // Fallback for unmapped types — emit a generic pointer.
                fprintf(out,
                    "!%d = !DIBasicType(name: \"ptr\", size: 64, encoding: DW_ATE_address)\n",
                    b->md_id);
            }
        }

        for (int i = 0; i < emit->var_count; i++) {
            const LlvmDbgVar *v = &emit->vars[i];
            if (v->arg_index > 0) {
                fprintf(out,
                    "!%d = !DILocalVariable(name: \"%s\", arg: %d, scope: !%d, "
                    "file: !%d, line: %d, type: !%d)\n",
                    v->md_id, v->name, v->arg_index, v->scope_md_id,
                    v->file_md_id, v->line, v->type_md_id);
            } else {
                fprintf(out,
                    "!%d = !DILocalVariable(name: \"%s\", scope: !%d, "
                    "file: !%d, line: %d, type: !%d)\n",
                    v->md_id, v->name, v->scope_md_id,
                    v->file_md_id, v->line, v->type_md_id);
            }
        }

        for (int i = 0; i < emit->loc_count; i++) {
            const LlvmDbgLoc *l = &emit->locs[i];
            fprintf(out,
                "!%d = !DILocation(line: %d, column: %d, scope: !%d)\n",
                l->md_id, l->line, l->col > 0 ? l->col : 1, l->scope_md_id);
        }
    }
}
