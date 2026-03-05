#ifndef LSP_SYMBOLS_H
#define LSP_SYMBOLS_H

#include "../ast.h"
#include "../types.h"

typedef struct SymbolEntry {
    char *name;
    SrcLoc def_loc;
    MixType *type;
    NodeKind decl_kind;
    char *container;        // shape name for methods/fields, or NULL
    // Function parameter names and type strings (for signature help)
    char **param_names;
    char **param_type_strs;  // type as string when MixType not available
    int param_name_count;
    char *return_type_str;   // return type as string when MixType not available
    struct SymbolEntry *next;
} SymbolEntry;

typedef struct {
    SymbolEntry **buckets;
    int bucket_count;
    // Flat list for iteration (completions)
    SymbolEntry **all;
    int all_count;
    int all_cap;
} SymbolIndex;

void symbol_index_init(SymbolIndex *idx);
void symbol_index_clear(SymbolIndex *idx);
void symbol_index_destroy(SymbolIndex *idx);

// Build the index by walking the AST
void symbol_index_build(SymbolIndex *idx, AstNode *program);

// Build index with imported module symbols
void symbol_index_build_with_imports(SymbolIndex *idx, AstNode *program,
                                      const char *filepath);

// Look up a symbol by name (returns first match)
SymbolEntry *symbol_index_lookup(SymbolIndex *idx, const char *name);

#endif // LSP_SYMBOLS_H
