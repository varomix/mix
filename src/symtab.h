#ifndef SYMTAB_H
#define SYMTAB_H

#include "mix.h"
#include "types.h"

typedef struct Symbol {
    char *name;
    MixType *type;
    bool is_mutable;
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
