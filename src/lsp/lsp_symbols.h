#ifndef LSP_SYMBOLS_H
#define LSP_SYMBOLS_H

#include "../ast.h"
#include "../types.h"

// One occurrence of a symbol — used by find references and document highlight.
typedef struct Reference {
    SrcLoc loc;        // 1-based line/col of the identifier
    int name_len;      // length of the identifier (for end column)
    bool is_write;     // true if this is the LHS of an assignment
    struct Reference *next;
} Reference;

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
    // All use sites in the indexed document(s). Owned linked list, freed in
    // symbol_index_clear. Definition itself is NOT included — caller may
    // include def_loc separately when answering find-references.
    Reference *uses;
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
                                      const char *filepath,
                                      const char *exe_dir,
                                      const char *root_path);

// Look up a symbol by name (returns first match)
SymbolEntry *symbol_index_lookup(SymbolIndex *idx, const char *name);

// Clear process-wide imported module/header symbol caches.
void lsp_symbol_cache_clear(void);

// Resolve a `use a.b.c` declaration to an absolute filesystem path. Returns
// strdup'd path on hit, NULL if no .mix file exists at the resolved location.
// Caller frees. exe_dir is the directory containing the mix-lsp binary.
char *lsp_resolve_use_path(const char *main_filepath, const char *module_path,
                           const char *exe_dir, const char *root_path);

#endif // LSP_SYMBOLS_H
