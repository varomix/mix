#include "symtab.h"
#include "arena.h"

SymTab symtab_create(Arena *arena) {
    SymTab st = {0};
    st.arena = arena;
    st.current = arena_alloc(arena, sizeof(Scope));
    st.current->symbols = NULL;
    st.current->parent = NULL;
    return st;
}

void symtab_push_scope(SymTab *st) {
    Scope *scope = arena_alloc(st->arena, sizeof(Scope));
    scope->symbols = NULL;
    scope->parent = st->current;
    st->current = scope;
}

void symtab_pop_scope(SymTab *st) {
    if (st->current->parent) {
        st->current = st->current->parent;
    }
}

void symtab_insert(SymTab *st, const char *name, MixType *type, bool is_mutable) {
    Symbol *sym = arena_alloc(st->arena, sizeof(Symbol));
    sym->name = arena_strdup(st->arena, name);
    sym->c_name = NULL;
    sym->type = type;
    sym->is_mutable = is_mutable;
    sym->is_global = false;
    sym->has_mutation = false;
    sym->is_stack_slot = false;
    sym->is_pointer_slot = false;
    sym->is_extern = false;
    sym->next = st->current->symbols;
    st->current->symbols = sym;
}

Symbol *symtab_lookup(SymTab *st, const char *name) {
    for (Scope *scope = st->current; scope; scope = scope->parent) {
        for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    return NULL;
}

Symbol *symtab_lookup_current(SymTab *st, const char *name) {
    for (Symbol *sym = st->current->symbols; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}
