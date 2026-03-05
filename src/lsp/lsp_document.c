#include "lsp_document.h"
#include "../errors.h"

#define DOC_BUCKETS 64
#define ARENA_DEFAULT_CAP (1024 * 1024)

static unsigned int hash_string(const char *s) {
    unsigned int h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

// Convert file:// URI to filesystem path
static char *uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) == 0) {
        return strdup(uri + 7);
    }
    return strdup(uri);
}

DocumentStore docstore_create(void) {
    DocumentStore store;
    store.bucket_count = DOC_BUCKETS;
    store.buckets = calloc(DOC_BUCKETS, sizeof(LspDocument *));
    return store;
}

void docstore_destroy(DocumentStore *store) {
    for (int i = 0; i < store->bucket_count; i++) {
        LspDocument *doc = store->buckets[i];
        while (doc) {
            LspDocument *next = doc->next;
            free(doc->uri);
            free(doc->filepath);
            free(doc->source);
            if (doc->tokens) free(doc->tokens);
            arena_destroy(&doc->doc_arena);
            diag_list_destroy(&doc->diagnostics);
            symbol_index_destroy(&doc->symbols);
            free(doc);
            doc = next;
        }
    }
    free(store->buckets);
}

LspDocument *docstore_find(DocumentStore *store, const char *uri) {
    unsigned int idx = hash_string(uri) % store->bucket_count;
    LspDocument *doc = store->buckets[idx];
    while (doc) {
        if (strcmp(doc->uri, uri) == 0) return doc;
        doc = doc->next;
    }
    return NULL;
}

LspDocument *docstore_open(DocumentStore *store, const char *uri,
                           const char *text, int version) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup(uri);
    doc->filepath = uri_to_path(uri);
    doc->source = strdup(text);
    doc->version = version;
    doc->doc_arena = arena_create(ARENA_DEFAULT_CAP);
    diag_list_init(&doc->diagnostics);
    symbol_index_init(&doc->symbols);

    unsigned int idx = hash_string(uri) % store->bucket_count;
    doc->next = store->buckets[idx];
    store->buckets[idx] = doc;

    document_analyze(doc);
    return doc;
}

void docstore_update(DocumentStore *store, const char *uri,
                     const char *text, int version) {
    LspDocument *doc = docstore_find(store, uri);
    if (!doc) return;

    free(doc->source);
    doc->source = strdup(text);
    doc->version = version;
    doc->analysis_valid = false;

    document_analyze(doc);
}

void docstore_close(DocumentStore *store, const char *uri) {
    unsigned int idx = hash_string(uri) % store->bucket_count;
    LspDocument **pp = &store->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->uri, uri) == 0) {
            LspDocument *doc = *pp;
            *pp = doc->next;
            free(doc->uri);
            free(doc->filepath);
            free(doc->source);
            if (doc->tokens) free(doc->tokens);
            arena_destroy(&doc->doc_arena);
            diag_list_destroy(&doc->diagnostics);
            symbol_index_destroy(&doc->symbols);
            free(doc);
            return;
        }
        pp = &(*pp)->next;
    }
}

// No-op diagnostic callback to silently discard sema errors
static void discard_diagnostic(DiagSeverity sev, SrcLoc loc, const char *msg, void *ud) {
    (void)sev; (void)loc; (void)msg; (void)ud;
}

void document_analyze(LspDocument *doc) {
    // Destroy old arena, create fresh
    arena_destroy(&doc->doc_arena);
    doc->doc_arena = arena_create(ARENA_DEFAULT_CAP);

    // Free old tokens
    if (doc->tokens) { free(doc->tokens); doc->tokens = NULL; }
    doc->token_count = 0;
    doc->ast = NULL;

    // Clear previous diagnostics
    diag_list_clear(&doc->diagnostics);

    // Clear previous symbol index
    symbol_index_clear(&doc->symbols);

    // Reset error count
    errors_reset();

    // Install diagnostic callback
    errors_set_callback(lsp_diagnostic_callback, &doc->diagnostics);
    errors_set_source(doc->source, doc->filepath);

    // Lex
    Lexer lexer = lexer_create(doc->source, doc->filepath, &doc->doc_arena);
    lexer_tokenize(&lexer);
    doc->tokens = lexer.tokens;
    doc->token_count = lexer.token_count;

    // Parse (even if lex errors, try for partial results)
    Parser parser = parser_create(doc->tokens, doc->token_count,
                                   &doc->doc_arena, doc->filepath);
    doc->ast = parser_parse(&parser);

    // Sema — try even with parse errors for best-effort type info
    // Note: sema runs without module context, so it will report false errors
    // for imported functions. We capture the error count before sema and only
    // keep parse errors (before sema) as diagnostics.
    if (doc->ast && doc->ast->kind == NODE_PROGRAM) {
        int parse_diag_count = doc->diagnostics.count;
        Sema sema = sema_create(&doc->doc_arena);

        // Suppress sema error output — we only want it for type info
        errors_set_callback(discard_diagnostic, NULL);
        sema_analyze(&sema, doc->ast);

        // Restore callback and trim diagnostics to parse-only errors
        errors_set_callback(lsp_diagnostic_callback, &doc->diagnostics);
        doc->diagnostics.count = parse_diag_count;

        // Build symbol index from analyzed AST (includes imported modules)
        symbol_index_build_with_imports(&doc->symbols, doc->ast, doc->filepath);
    }

    // Restore default error behavior
    errors_set_callback(NULL, NULL);

    doc->analysis_valid = true;

    // Publish diagnostics
    lsp_publish_diagnostics(doc->uri, &doc->diagnostics);
}
