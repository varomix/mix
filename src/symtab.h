#ifndef SYMTAB_H
#define SYMTAB_H

#include "mix.h"
#include "types.h"

typedef struct Symbol {
    char *name;
    char *c_name;       // optional C symbol name for aliased extern functions
    MixType *type;
    bool is_mutable;
    bool is_global;     // module-level mutable; emit/read as global storage
    bool has_mutation;  // fn/method declared with trailing `!`
    // When true, the binding lives in an alloca-backed storage slot
    // rather than an SSA value — read sites must load. C backend uses
    // this to choose load vs copy even for read-only loop bindings.
    bool is_stack_slot;
    // For SHAPE-typed variables only: when true, the slot holds a
    // pointer to the shape (e.g. a for-each loop var over a shape
    // list); when false the binding IS the shape pointer.
    bool is_pointer_slot;
    // True for symbols introduced by `extern "lib" { ... }` blocks (in
    // user source or from cbind output). Distinguishes C-ABI callees
    // from MIX-defined functions that share the symbol-table TYPE_FUNC
    // shape — the lowerer needs this to decide whether to apply
    // small-struct-by-int / float32 coercions at call sites for callees
    // it has not pre-registered.
    bool is_extern;
    struct Symbol *next;
} Symbol;

typedef struct Scope {
    Symbol *symbols;
    struct Scope *parent;
} Scope;

typedef struct {
    Scope *current;
    Arena *arena;
} SymTab;

SymTab symtab_create(Arena *arena);
void symtab_push_scope(SymTab *st);
void symtab_pop_scope(SymTab *st);
void symtab_insert(SymTab *st, const char *name, MixType *type, bool is_mutable);
Symbol *symtab_lookup(SymTab *st, const char *name);
Symbol *symtab_lookup_current(SymTab *st, const char *name);

#endif // SYMTAB_H
