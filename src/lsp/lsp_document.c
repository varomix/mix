#include "lsp_document.h"
#include "../cbind.h"
#include "../errors.h"
#include <libgen.h>
#include <sys/stat.h>

#define DOC_BUCKETS 64
#define ARENA_DEFAULT_CAP (1024 * 1024)
#define CBIND_SRC_CACHE_MAX 8

typedef struct {
    char *key;
    char *filename;
    char *source;
    time_t mtime;
} CbindSourceCacheEntry;

static CbindSourceCacheEntry cbind_src_cache[CBIND_SRC_CACHE_MAX];
static int cbind_src_cache_count = 0;

static unsigned int hash_string(const char *s) {
    unsigned int h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

// Convert file:// URI to filesystem path
char *lsp_uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) == 0) {
        return strdup(uri + 7);
    }
    return strdup(uri);
}

// Convert filesystem path to file:// URI. No URL-encoding for now.
char *lsp_path_to_uri(const char *path) {
    size_t n = strlen(path);
    char *out = malloc(n + 8);
    snprintf(out, n + 8, "file://%s", path);
    return out;
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
            free(doc->root_path);
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
                           const char *text, int version,
                           const char *exe_dir,
                           const char *root_path) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup(uri);
    doc->filepath = lsp_uri_to_path(uri);
    doc->source = strdup(text);
    doc->version = version;
    if (exe_dir) snprintf(doc->exe_dir, sizeof(doc->exe_dir), "%s", exe_dir);
    doc->root_path = root_path ? strdup(root_path) : NULL;
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
            free(doc->root_path);
            arena_destroy(&doc->doc_arena);
            diag_list_destroy(&doc->diagnostics);
            symbol_index_destroy(&doc->symbols);
            free(doc);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Discard sema diagnostics from imported modules — they would point to lines
// the user isn't editing and would clutter the editor.
static void discard_diagnostic(DiagSeverity sev, SrcLoc loc, const char *msg, void *ud) {
    (void)sev; (void)loc; (void)msg; (void)ud;
}

// Read entire file into a malloc'd buffer. Returns NULL on error.
static char *read_file_contents(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static void cbind_src_cache_free_entry(CbindSourceCacheEntry *entry) {
    free(entry->key);
    free(entry->filename);
    free(entry->source);
    memset(entry, 0, sizeof(*entry));
}

void lsp_document_cbind_cache_clear(void) {
    for (int i = 0; i < cbind_src_cache_count; i++) {
        cbind_src_cache_free_entry(&cbind_src_cache[i]);
    }
    cbind_src_cache_count = 0;
}

static CbindSourceCacheEntry *cbind_src_cache_find(const char *key) {
    for (int i = 0; i < cbind_src_cache_count; i++) {
        if (cbind_src_cache[i].key && strcmp(cbind_src_cache[i].key, key) == 0) {
            return &cbind_src_cache[i];
        }
    }
    return NULL;
}

static CbindSourceCacheEntry *cbind_src_cache_slot(void) {
    if (cbind_src_cache_count < CBIND_SRC_CACHE_MAX) {
        return &cbind_src_cache[cbind_src_cache_count++];
    }

    cbind_src_cache_free_entry(&cbind_src_cache[0]);
    memmove(&cbind_src_cache[0], &cbind_src_cache[1],
            sizeof(CbindSourceCacheEntry) * (CBIND_SRC_CACHE_MAX - 1));
    memset(&cbind_src_cache[CBIND_SRC_CACHE_MAX - 1], 0, sizeof(CbindSourceCacheEntry));
    return &cbind_src_cache[CBIND_SRC_CACHE_MAX - 1];
}

static const char *cached_cbind_source(const char *header_path, const char *lib_name,
                                       const char *source_dir,
                                       const char **out_filename) {
    char *resolved = resolve_header_path(header_path, source_dir);
    const char *filename = resolved ? resolved : header_path;

    struct stat st;
    time_t mtime = (stat(filename, &st) == 0) ? st.st_mtime : 1;

    char key[3072];
    snprintf(key, sizeof(key), "%s|%s", filename, lib_name ? lib_name : "");

    CbindSourceCacheEntry *cached = cbind_src_cache_find(key);
    if (cached && cached->mtime == mtime) {
        *out_filename = cached->filename;
        free(resolved);
        return cached->source;
    }

    char *source = cbind_generate_string(header_path, lib_name, false, source_dir);
    if (!source) {
        free(resolved);
        return NULL;
    }

    cached = cbind_src_cache_slot();
    cached->key = strdup(key);
    cached->filename = strdup(filename);
    cached->source = source;
    cached->mtime = mtime;

    *out_filename = cached->filename;
    free(resolved);
    return cached->source;
}

// Walk the main file's AST for NODE_USE_DECLs. For each, lex+parse+sema the
// module file into the SHARED sema so its pub symbols become visible. Module
// diagnostics are silently discarded.
//
// Recursion depth is shallow in practice (modules importing modules is
// uncommon for MIX); we cap at 16 to avoid runaway includes.
static void load_module_imports(AstNode *program, const char *filepath,
                                Sema *sema, Arena *arena,
                                const char *exe_dir,
                                const char *root_path) {
    static int depth = 0;
    if (depth >= 16) return;
    if (!program || program->kind != NODE_PROGRAM) return;

    DiagnosticCallback prev_cb = NULL;
    void *prev_ud = NULL;
    errors_get_callback(&prev_cb, &prev_ud);
    errors_set_callback(discard_diagnostic, NULL);

    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_USE_C_DECL) {
            // Resolve relative headers against the .mix file's directory
            char *doc_dir = strdup(filepath);
            char *source_dir = dirname(doc_dir);

            const char *bind_filename = NULL;
            const char *bind_src = cached_cbind_source(decl->use_c_decl.header_path,
                                                       decl->use_c_decl.lib_name,
                                                       source_dir, &bind_filename);
            if (!bind_src) { free(doc_dir); continue; }

            errors_set_source(bind_src, bind_filename);
            Lexer bl = lexer_create(bind_src, bind_filename, arena);
            lexer_tokenize(&bl);
            Parser bp = parser_create(bl.tokens, bl.token_count, arena, bind_filename);
            AstNode *bprog = parser_parse(&bp);
            if (bprog && bprog->kind == NODE_PROGRAM) {
                sema_analyze(sema, bprog);
            }
            free(doc_dir);
            continue;
        }

        if (decl->kind != NODE_USE_DECL) continue;

        char *mod_path = lsp_resolve_use_path(filepath, decl->use_decl.module_path,
                                              exe_dir, root_path);
        if (!mod_path) continue;

        char *src = read_file_contents(mod_path);
        if (!src) { free(mod_path); continue; }

        errors_set_source(src, mod_path);
        Lexer lex = lexer_create(src, mod_path, arena);
        lexer_tokenize(&lex);
        Parser ps = parser_create(lex.tokens, lex.token_count, arena, mod_path);
        AstNode *mod_prog = parser_parse(&ps);

        if (mod_prog && mod_prog->kind == NODE_PROGRAM) {
            // Recurse first so transitive pub symbols populate the symtab
            // before this module's own sema runs.
            depth++;
            load_module_imports(mod_prog, mod_path, sema, arena, exe_dir, root_path);
            depth--;
            errors_set_source(src, mod_path);
            sema_analyze(sema, mod_prog);
        }

        free(src);
        free(mod_path);
    }

    errors_set_callback(prev_cb, prev_ud);
}

void document_analyze(LspDocument *doc) {
    // Destroy old arena, create fresh
    arena_destroy(&doc->doc_arena);
    doc->doc_arena = arena_create(ARENA_DEFAULT_CAP);

    doc->tokens = NULL;
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

    // If the current buffer has syntax errors, stop here. Continuing into
    // module imports and C binding generation while the user is mid-edit can
    // repeatedly expand large vendored headers and stream noisy derived
    // diagnostics into the editor.
    if (mix_error_count() > 0) {
        errors_set_callback(NULL, NULL);
        doc->analysis_valid = true;
        lsp_publish_diagnostics(doc->uri, &doc->diagnostics);
        return;
    }

    // Sema — only after a clean parse. Broken buffers should stay cheap while
    // the user is typing.
    //
    // We let sema diagnostics through to the editor so quick fixes like
    // "did you mean" can offer code actions. Before analyzing the main
    // file, we run sema on each `use`d module's AST into the SAME sema
    // (with a discard callback so module diagnostics don't leak), so the
    // main file's sema sees imported pub symbols. Stdlib resolution walks
    // up the file's directory tree looking for `lib/std/`; if not found,
    // the file falls back to single-file analysis (false positives for
    // imports remain in that case).
    if (doc->ast && doc->ast->kind == NODE_PROGRAM) {
        Sema sema = sema_create(&doc->doc_arena);

        load_module_imports(doc->ast, doc->filepath, &sema, &doc->doc_arena,
                            doc->exe_dir, doc->root_path);

        // Restore the publishing callback for the main file's analysis and
        // re-set the source so error gutters point to the right buffer.
        errors_set_callback(lsp_diagnostic_callback, &doc->diagnostics);
        errors_set_source(doc->source, doc->filepath);
        sema_analyze(&sema, doc->ast);

        // Build symbol index from analyzed AST (includes imported modules)
        symbol_index_build_with_imports(&doc->symbols, doc->ast, doc->filepath,
                                        doc->exe_dir, doc->root_path);
    }

    // Restore default error behavior
    errors_set_callback(NULL, NULL);

    doc->analysis_valid = true;

    // Publish diagnostics
    lsp_publish_diagnostics(doc->uri, &doc->diagnostics);
}
