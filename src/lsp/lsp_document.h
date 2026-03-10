#ifndef LSP_DOCUMENT_H
#define LSP_DOCUMENT_H

#include "../arena.h"
#include "../lexer.h"
#include "../parser.h"
#include "../sema.h"
#include "lsp_diagnostics.h"
#include "lsp_symbols.h"

typedef struct LspDocument {
    char *uri;
    char *filepath;
    char *source;
    int version;

    // Analysis results
    Arena doc_arena;
    Token *tokens;
    int token_count;
    AstNode *ast;
    bool analysis_valid;

    // Diagnostics
    DiagnosticList diagnostics;

    // Symbol index for go-to-def and completions
    SymbolIndex symbols;

    struct LspDocument *next;
} LspDocument;

// Document store: hash map by URI
typedef struct {
    LspDocument **buckets;
    int bucket_count;
} DocumentStore;

DocumentStore docstore_create(void);
void docstore_destroy(DocumentStore *store);

LspDocument *docstore_open(DocumentStore *store, const char *uri,
                           const char *text, int version);
void docstore_update(DocumentStore *store, const char *uri,
                     const char *text, int version);
void docstore_close(DocumentStore *store, const char *uri);
LspDocument *docstore_find(DocumentStore *store, const char *uri);

// Run full analysis pipeline on a document
void document_analyze(LspDocument *doc);

// Ensure document is analyzed (lazy analysis — only runs if dirty)
static inline void document_ensure_analyzed(LspDocument *doc) {
    if (doc && !doc->analysis_valid) document_analyze(doc);
}

#endif // LSP_DOCUMENT_H
