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
    // When true, `%v.<name>` names an alloca-backed storage slot rather than
    // an SSA value. QBE uses this to choose a load even for read-only loop
    // bindings whose language-level mutability is false.
    bool is_stack_slot;
    // For SHAPE-typed variables only: when true, %v.<name> is an alloca slot
    // holding a pointer to the shape (e.g. a for-each loop var over a shape
    // list). When false (the default), %v.<name> is the pointer itself —
    // the QBE NODE_VAR_DECL `=l copy` aliasing pattern. The QBE NODE_IDENT
    // path uses this flag to choose between `loadl %v.x` and `copy %v.x`.
    bool is_pointer_slot;
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
