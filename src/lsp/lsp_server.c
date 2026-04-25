#include "lsp_server.h"
#include "lsp_transport.h"
#include "lsp_position.h"
#include "lsp_hover.h"
#include "../fmt.h"
#include <ctype.h>

void lsp_server_init(LspServer *server) {
    memset(server, 0, sizeof(LspServer));
    server->documents = docstore_create();
    workspace_init(&server->workspace);
}

void lsp_server_destroy(LspServer *server) {
    docstore_destroy(&server->documents);
    workspace_destroy(&server->workspace);
    free(server->root_uri);
    free(server->root_path);
}

// --- Handlers ---

#define WORKSPACE_FILE_CAP 5000

static void handle_initialize(LspServer *server, int64_t id, JsonValue *params) {
    JsonValue *root = json_get(params, "rootUri");
    if (root && root->kind == JSON_STRING) {
        server->root_uri = strdup(root->string.data);
        server->root_path = lsp_uri_to_path(server->root_uri);
        // Eagerly index the workspace so the first workspace/symbol query is
        // fast. For very large projects this may need to be made async, but
        // for typical MIX projects (<100 files) it's well under 50ms.
        workspace_scan(&server->workspace, server->root_path, WORKSPACE_FILE_CAP);
    }

    server->initialized = true;

    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
      jw_key(&w, "capabilities");
      jw_object_start(&w);
        // Full text sync
        jw_key(&w, "textDocumentSync");
        jw_object_start(&w);
          jw_key(&w, "openClose"); jw_bool(&w, true);
          jw_key(&w, "change"); jw_int(&w, 1); // Full = 1
          jw_key(&w, "save");
          jw_object_start(&w);
            jw_key(&w, "includeText"); jw_bool(&w, false);
          jw_object_end(&w);
        jw_object_end(&w);

        // Hover
        jw_key(&w, "hoverProvider"); jw_bool(&w, true);

        // Go-to-definition
        jw_key(&w, "definitionProvider"); jw_bool(&w, true);

        // Completion
        jw_key(&w, "completionProvider");
        jw_object_start(&w);
          jw_key(&w, "triggerCharacters");
          jw_array_start(&w);
            jw_string(&w, ".");
          jw_array_end(&w);
        jw_object_end(&w);

        // Signature Help
        jw_key(&w, "signatureHelpProvider");
        jw_object_start(&w);
          jw_key(&w, "triggerCharacters");
          jw_array_start(&w);
            jw_string(&w, "(");
            jw_string(&w, ",");
          jw_array_end(&w);
        jw_object_end(&w);

        // Phase B: document outline (functions, shapes, methods, consts)
        jw_key(&w, "documentSymbolProvider"); jw_bool(&w, true);

        // Phase A stubs — advertised so default keymaps stop being silent.
        // Handlers return null/empty until their phase ships.
        jw_key(&w, "referencesProvider"); jw_bool(&w, true);
        jw_key(&w, "workspaceSymbolProvider"); jw_bool(&w, true);
        jw_key(&w, "renameProvider");
        jw_object_start(&w);
          jw_key(&w, "prepareProvider"); jw_bool(&w, true);
        jw_object_end(&w);
        jw_key(&w, "documentHighlightProvider"); jw_bool(&w, true);
        jw_key(&w, "inlayHintProvider"); jw_bool(&w, true);
        jw_key(&w, "documentFormattingProvider"); jw_bool(&w, true);
        jw_key(&w, "codeActionProvider"); jw_bool(&w, true);

      jw_object_end(&w); // capabilities

      jw_key(&w, "serverInfo");
      jw_object_start(&w);
        jw_key(&w, "name"); jw_string(&w, "mix-lsp");
        jw_key(&w, "version"); jw_string(&w, "0.1.0");
      jw_object_end(&w);
    jw_object_end(&w);

    lsp_send_response(id, w.buf);
    jw_free(&w);
}

static void handle_did_open(LspServer *server, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(td, "uri");
    const char *text = json_get_string(td, "text");
    int version = (int)json_get_int(td, "version");

    if (uri && text) {
        docstore_open(&server->documents, uri, text, version);
    }
}

static void handle_did_change(LspServer *server, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(td, "uri");
    int version = (int)json_get_int(td, "version");

    // Full text sync: first content change has the full text
    JsonValue *changes = json_get(params, "contentChanges");
    if (!changes || changes->kind != JSON_ARRAY || changes->array.count == 0) return;

    const char *text = json_get_string(changes->array.items[0], "text");
    if (uri && text) {
        docstore_update(&server->documents, uri, text, version);
        // Mark workspace cache for this file stale; next workspace_get
        // re-analyzes from the saved-on-disk text. (We could also
        // analyze in-memory text, but that diverges from saved state.)
        char *path = lsp_uri_to_path(uri);
        workspace_invalidate(&server->workspace, path);
        free(path);
    }
}

static void handle_did_close(LspServer *server, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(td, "uri");
    if (uri) {
        // Publish empty diagnostics to clear them in the editor
        DiagnosticList empty = {0};
        lsp_publish_diagnostics(uri, &empty);

        docstore_close(&server->documents, uri);
    }
}

static void handle_did_save(LspServer *server, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(td, "uri");
    if (!uri) return;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (doc) {
        document_analyze(doc);
    }
}

// --- Hover ---

static void handle_hover_request(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) {
        lsp_send_response(id, "null");
        return;
    }
    document_ensure_analyzed(doc);
    if (!doc->analysis_valid) {
        lsp_send_response(id, "null");
        return;
    }

    Token *tok = tokens_find_at(doc->tokens, doc->token_count, line, col);
    if (!tok || (tok->kind != TOK_IDENT && tok->kind != TOK_IDENT_MUT)) {
        lsp_send_response(id, "null");
        return;
    }

    char name[256];
    if (!token_ident_name(tok, name, sizeof(name))) {
        lsp_send_response(id, "null");
        return;
    }

    PositionResult pr = {0};
    ast_find_node_at(doc->ast, line, col, &pr);

    MixType *type = NULL;
    if (pr.node && pr.node->resolved_type) {
        type = pr.node->resolved_type;
    }

    if (!type) {
        lsp_send_response(id, "null");
        return;
    }

    char type_str[512];
    mix_type_to_string(type, type_str, sizeof(type_str));

    char hover_md[4096];
    int hlen = 0;

    // For shape/union types, show field list
    if (type->kind == TYPE_SHAPE && type->shape.field_count > 0) {
        const char *kw = type->shape.is_union ? "union" : "shape";
        hlen += snprintf(hover_md + hlen, sizeof(hover_md) - hlen,
                         "```mix\n%s %s\n", kw, type->shape.name);
        for (int i = 0; i < type->shape.field_count && i < 20; i++) {
            char ft[128];
            mix_type_to_string(type->shape.fields[i].type, ft, sizeof(ft));
            hlen += snprintf(hover_md + hlen, sizeof(hover_md) - hlen,
                             "    %s: %s\n", type->shape.fields[i].name, ft);
        }
        if (type->shape.field_count > 20) {
            hlen += snprintf(hover_md + hlen, sizeof(hover_md) - hlen,
                             "    ... (%d more)\n", type->shape.field_count - 20);
        }
        hlen += snprintf(hover_md + hlen, sizeof(hover_md) - hlen, "```");
    } else if (type->kind == TYPE_FUNC && type->func.return_type) {
        // For functions, show signature
        hlen += snprintf(hover_md + hlen, sizeof(hover_md) - hlen,
                         "```mix\n%s: %s\n```", name, type_str);
    } else {
        hlen += snprintf(hover_md + hlen, sizeof(hover_md) - hlen,
                         "```mix\n%s: %s\n```", name, type_str);
    }

    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
      jw_key(&w, "contents");
      jw_object_start(&w);
        jw_key(&w, "kind"); jw_string(&w, "markdown");
        jw_key(&w, "value"); jw_string(&w, hover_md);
      jw_object_end(&w);
    jw_object_end(&w);

    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Go-to-Definition ---

static void handle_goto_definition(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) {
        lsp_send_response(id, "null");
        return;
    }
    document_ensure_analyzed(doc);
    if (!doc->analysis_valid) {
        lsp_send_response(id, "null");
        return;
    }

    Token *tok = tokens_find_at(doc->tokens, doc->token_count, line, col);
    if (!tok || (tok->kind != TOK_IDENT && tok->kind != TOK_IDENT_MUT)) {
        lsp_send_response(id, "null");
        return;
    }

    char name[256];
    if (!token_ident_name(tok, name, sizeof(name))) {
        lsp_send_response(id, "null");
        return;
    }

    SymbolEntry *entry = symbol_index_lookup(&doc->symbols, name);
    if (!entry || entry->def_loc.line == 0) {
        lsp_send_response(id, "null");
        return;
    }

    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
      jw_key(&w, "uri"); jw_string(&w, uri);
      jw_key(&w, "range");
      jw_object_start(&w);
        jw_key(&w, "start");
        jw_object_start(&w);
          jw_key(&w, "line"); jw_int(&w, entry->def_loc.line - 1);
          jw_key(&w, "character"); jw_int(&w, entry->def_loc.col - 1);
        jw_object_end(&w);
        jw_key(&w, "end");
        jw_object_start(&w);
          jw_key(&w, "line"); jw_int(&w, entry->def_loc.line - 1);
          jw_key(&w, "character"); jw_int(&w, entry->def_loc.col - 1 + (int)strlen(name));
        jw_object_end(&w);
      jw_object_end(&w);
    jw_object_end(&w);

    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Completion ---

static void emit_completion(JsonWriter *w, const char *label, int kind,
                            const char *detail) {
    jw_object_start(w);
      jw_key(w, "label"); jw_string(w, label);
      jw_key(w, "kind"); jw_int(w, kind);
      if (detail) {
          jw_key(w, "detail"); jw_string(w, detail);
      }
    jw_object_end(w);
}

static void add_builtin_methods(JsonWriter *w, MixType *type) {
    if (!type) return;
    switch (type->kind) {
        case TYPE_LIST:
            emit_completion(w, "push", 2, "push!(value)");
            emit_completion(w, "len", 5, "int");
            break;
        case TYPE_STR:
            emit_completion(w, "len", 5, "int");
            emit_completion(w, "upper", 2, "() -> str");
            emit_completion(w, "lower", 2, "() -> str");
            emit_completion(w, "trim", 2, "() -> str");
            emit_completion(w, "split", 2, "(sep: str) -> [str]");
            emit_completion(w, "contains", 2, "(s: str) -> bool");
            emit_completion(w, "starts_with", 2, "(s: str) -> bool");
            emit_completion(w, "replace", 2, "(old: str, new: str) -> str");
            break;
        case TYPE_MAP:
            emit_completion(w, "len", 5, "int");
            emit_completion(w, "keys", 5, "[str]");
            emit_completion(w, "values", 5, "[any]");
            emit_completion(w, "has", 2, "(key) -> bool");
            emit_completion(w, "remove", 2, "(key)");
            break;
        case TYPE_SHARED:
            emit_completion(w, "read", 2, "() -> value");
            emit_completion(w, "update", 2, "(fn)");
            break;
        default:
            break;
    }
}

// Check if cursor is inside {expr} in a string interpolation.
// Returns: 0 = not in interp, 1 = bare ident, 2 = after dot (obj_name filled)
static int check_string_interp(const char *source, int line, int col,
                                char *ident_out, int ident_size,
                                char *obj_out, int obj_size) {
    const char *p = source;
    int cur_line = 1;
    while (*p && cur_line < line) {
        if (*p == '\n') cur_line++;
        p++;
    }
    if (cur_line != line) return 0;

    int line_len = 0;
    while (p[line_len] && p[line_len] != '\n') line_len++;
    int cursor = col - 1;
    if (cursor > line_len) cursor = line_len;

    // Scan backward from cursor to find an unmatched '{'
    int scan_from = cursor - 1;
    if (scan_from >= 0 && p[scan_from] == '}') scan_from--;

    int brace_depth = 0;
    int brace_pos = -1;
    for (int i = scan_from; i >= 0; i--) {
        if (p[i] == '}') brace_depth++;
        else if (p[i] == '{') {
            if (brace_depth > 0) brace_depth--;
            else { brace_pos = i; break; }
        }
    }
    if (brace_pos < 0) return 0;

    // Check there's a quote before the brace (we're inside a string)
    bool in_string = false;
    for (int i = brace_pos - 1; i >= 0; i--) {
        if (p[i] == '"') { in_string = true; break; }
        if (p[i] == '{' || p[i] == '}') break;
    }
    if (!in_string) return 0;

    // Extract the text between { and cursor
    int frag_start = brace_pos + 1;
    int frag_len = cursor - frag_start;
    if (frag_len <= 0) {
        ident_out[0] = '\0';
        return 1;
    }

    char frag[256];
    if (frag_len >= (int)sizeof(frag)) frag_len = (int)sizeof(frag) - 1;
    memcpy(frag, p + frag_start, frag_len);
    frag[frag_len] = '\0';

    char *dot = strrchr(frag, '.');
    if (dot) {
        *dot = '\0';
        snprintf(obj_out, obj_size, "%s", frag);
        snprintf(ident_out, ident_size, "%s", dot + 1);
        return 2;
    }

    snprintf(ident_out, ident_size, "%s", frag);
    return 1;
}

static void handle_completion_request(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) {
        lsp_send_response(id, "null");
        return;
    }
    document_ensure_analyzed(doc);

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);

    // Check if cursor is inside string interpolation {expr}
    char interp_ident[256] = "";
    char interp_obj[256] = "";
    int interp_mode = doc->source
        ? check_string_interp(doc->source, line, col, interp_ident, sizeof(interp_ident),
                              interp_obj, sizeof(interp_obj))
        : 0;

    if (interp_mode == 2) {
        // After dot inside interpolation: "text {obj.|}"
        SymbolEntry *entry = symbol_index_lookup(&doc->symbols, interp_obj);
        MixType *obj_type = entry ? entry->type : NULL;

        if (obj_type && obj_type->kind == TYPE_SHAPE) {
            for (int i = 0; i < obj_type->shape.field_count; i++) {
                char detail[256];
                mix_type_to_string(obj_type->shape.fields[i].type, detail, sizeof(detail));
                emit_completion(&w, obj_type->shape.fields[i].name, 5, detail);
            }
            for (int i = 0; i < doc->symbols.all_count; i++) {
                SymbolEntry *se = doc->symbols.all[i];
                if (se->container && strcmp(se->container, obj_type->shape.name) == 0) {
                    char detail[256];
                    mix_type_to_string(se->type, detail, sizeof(detail));
                    emit_completion(&w, se->name, 2, detail);
                }
            }
        }
        if (obj_type) add_builtin_methods(&w, obj_type);
    } else if (interp_mode == 1) {
        // Bare ident inside interpolation: "text {fr|}"
        for (int i = 0; i < doc->symbols.all_count; i++) {
            SymbolEntry *se = doc->symbols.all[i];
            int kind = (se->decl_kind == NODE_FN_DECL) ? 3 : 6;
            char detail[256] = "";
            if (se->type) mix_type_to_string(se->type, detail, sizeof(detail));
            emit_completion(&w, se->name, kind, detail[0] ? detail : NULL);
        }
    } else {

    // Check if we're after a dot (field/method completion)
    Token *dot_tok = (col > 1) ? tokens_find_at(doc->tokens, doc->token_count, line, col - 1) : NULL;
    if (dot_tok && dot_tok->kind == TOK_DOT) {
        Token *prev_tok = NULL;
        for (int i = 0; i < doc->token_count; i++) {
            if (&doc->tokens[i] == dot_tok && i > 0) {
                prev_tok = &doc->tokens[i - 1];
                break;
            }
        }

        if (prev_tok && (prev_tok->kind == TOK_IDENT || prev_tok->kind == TOK_IDENT_MUT)) {
            char obj_name[256];
            token_ident_name(prev_tok, obj_name, sizeof(obj_name));

            SymbolEntry *entry = symbol_index_lookup(&doc->symbols, obj_name);
            MixType *obj_type = entry ? entry->type : NULL;

            if (obj_type && obj_type->kind == TYPE_SHAPE) {
                for (int i = 0; i < obj_type->shape.field_count; i++) {
                    char detail[256];
                    mix_type_to_string(obj_type->shape.fields[i].type, detail, sizeof(detail));
                    emit_completion(&w, obj_type->shape.fields[i].name, 5, detail);
                }
                for (int i = 0; i < doc->symbols.all_count; i++) {
                    SymbolEntry *se = doc->symbols.all[i];
                    if (se->container && strcmp(se->container, obj_type->shape.name) == 0) {
                        char detail[256];
                        mix_type_to_string(se->type, detail, sizeof(detail));
                        emit_completion(&w, se->name, 2, detail);
                    }
                }
            }

            if (obj_type) {
                add_builtin_methods(&w, obj_type);
            }
        }
    } else {
        // General completion: symbols + keywords
        for (int i = 0; i < doc->symbols.all_count; i++) {
            SymbolEntry *se = doc->symbols.all[i];
            int kind;
            switch (se->decl_kind) {
                case NODE_FN_DECL:    kind = 3; break;
                case NODE_VAR_DECL:   kind = 6; break;
                case NODE_CONST_DECL: kind = 21; break;
                case NODE_SHAPE_DECL: kind = 22; break;
                case NODE_TYPE_ALIAS: kind = 22; break;
                default:              kind = 6; break;
            }
            char detail[256] = "";
            if (se->type) mix_type_to_string(se->type, detail, sizeof(detail));
            emit_completion(&w, se->name, kind, detail[0] ? detail : NULL);
        }

        static const char *keywords[] = {
            "if", "else", "while", "for", "in", "match",
            "break", "continue", "done", "shape", "union", "extern",
            "use", "pub", "type", "zone", "defer", "unsafe",
            "and", "or", "not", "go", "wait", "shared",
            "true", "false", "none", "fail", NULL
        };
        for (int i = 0; keywords[i]; i++) {
            emit_completion(&w, keywords[i], 14, NULL);
        }

        static const char *type_kws[] = {
            "int", "float", "bool", "byte", "str",
            "int8", "int16", "int32", "int64",
            "uint8", "uint16", "uint32", "uint64",
            "float32", "float64", NULL
        };
        for (int i = 0; type_kws[i]; i++) {
            emit_completion(&w, type_kws[i], 22, NULL);
        }

        // Built-in functions
        static const struct { const char *name; const char *detail; } builtins[] = {
            {"print", "(any)"},
            {"to_string", "(any) -> str"},
            {"to_int", "(any) -> int"},
            {"to_float", "(any) -> float"},
            {"to_set", "([any]) -> set"},
            {"ord", "(str) -> int"},
            {"chr", "(int) -> str"},
            {"len", "(str|list|map|set) -> int"},
            {"panic", "(str)"},
            {"assert", "(bool, str)"},
            {"sizeof", "(any) -> int"},
            {"type_of", "(any) -> str"},
            {"sqrt", "(float) -> float"},
            {"abs", "(float) -> float"},
            {"floor", "(float) -> float"},
            {"ceil", "(float) -> float"},
            {"round", "(float) -> float"},
            {"pow", "(float, float) -> float"},
            {"min", "(float, float) -> float"},
            {"max", "(float, float) -> float"},
            {"sin", "(float) -> float"},
            {"cos", "(float) -> float"},
            {"tan", "(float) -> float"},
            {"log", "(float) -> float"},
            {"asin", "(float) -> float"},
            {"acos", "(float) -> float"},
            {"atan2", "(float, float) -> float"},
            {"clamp", "(float, float, float) -> float"},
            {"lerp", "(float, float, float) -> float"},
            {"file_open", "(str, str) -> int"},
            {"file_read", "(int) -> str"},
            {"file_write", "(int, str)"},
            {"file_close", "(int)"},
            {"file_read_all", "(str) -> str"},
            {"file_write_all", "(str, str)"},
            {"file_exists", "(str) -> bool"},
            {"list_dir", "(str) -> [str]"},
            {"shell", "(str) -> int"},
            {"shell_output", "(str) -> str"},
            {"env", "(str) -> str"},
            {"exit", "(int)"},
            {"getcwd", "() -> str"},
            {"mkdir", "(str) -> bool"},
            {"args", "() -> [str]"},
            {"str_reverse", "(str) -> str"},
            {"str_count", "(str, str) -> int"},
            {"alloc", "(int) -> *byte"},
            {"bytes", "(int) -> *byte"},
            {"peek_u32", "(*byte) -> uint32"},
            {"free_mem", "(*byte)"},
            {NULL, NULL}
        };
        for (int i = 0; builtins[i].name; i++) {
            emit_completion(&w, builtins[i].name, 3, builtins[i].detail);
        }
    }

    } // end interp_mode else

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Document Symbols (outline) ---

// Emit one DocumentSymbol JSON object. range is a single-line span starting
// at def_loc; selectionRange is the same. children is optional.
static void emit_doc_symbol(JsonWriter *w, const char *name,
                            const char *detail, int kind, SrcLoc loc,
                            int name_len) {
    int line = loc.line > 0 ? loc.line - 1 : 0;
    int col = loc.col > 0 ? loc.col - 1 : 0;
    jw_object_start(w);
      jw_key(w, "name"); jw_string(w, name);
      if (detail) { jw_key(w, "detail"); jw_string(w, detail); }
      jw_key(w, "kind"); jw_int(w, kind);
      jw_key(w, "range");
      jw_object_start(w);
        jw_key(w, "start");
        jw_object_start(w);
          jw_key(w, "line"); jw_int(w, line);
          jw_key(w, "character"); jw_int(w, col);
        jw_object_end(w);
        jw_key(w, "end");
        jw_object_start(w);
          jw_key(w, "line"); jw_int(w, line);
          jw_key(w, "character"); jw_int(w, col + name_len);
        jw_object_end(w);
      jw_object_end(w);
      jw_key(w, "selectionRange");
      jw_object_start(w);
        jw_key(w, "start");
        jw_object_start(w);
          jw_key(w, "line"); jw_int(w, line);
          jw_key(w, "character"); jw_int(w, col);
        jw_object_end(w);
        jw_key(w, "end");
        jw_object_start(w);
          jw_key(w, "line"); jw_int(w, line);
          jw_key(w, "character"); jw_int(w, col + name_len);
        jw_object_end(w);
      jw_object_end(w);
}

// LSP SymbolKind values we use:
//   12 = Function, 7 = Method, 23 = Struct, 14 = Constant,
//   13 = Variable, 26 = TypeParameter (used here for type aliases)
static int symbol_kind_for(NodeKind k, const char *container) {
    if (k == NODE_FN_DECL) return container ? 7 : 12;
    if (k == NODE_SHAPE_DECL) return 23;
    if (k == NODE_CONST_DECL) return 14;
    if (k == NODE_TYPE_ALIAS) return 26;
    if (k == NODE_VAR_DECL) return 13;
    return 13;
}

static void handle_document_symbol(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) {
        lsp_send_response(id, "[]");
        return;
    }
    document_ensure_analyzed(doc);

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);

    // Two-pass emit: top-level shapes/functions/consts/aliases first, then
    // for each shape we emit its methods nested as `children`. Local var
    // decls are intentionally skipped — the outline is for navigable
    // declarations, and including every loop var would just be noise.
    for (int i = 0; i < doc->symbols.all_count; i++) {
        SymbolEntry *e = doc->symbols.all[i];
        if (e->container) continue;            // method — handled below
        if (e->decl_kind == NODE_VAR_DECL) continue;

        char detail[256] = "";
        if (e->type) mix_type_to_string(e->type, detail, sizeof(detail));
        const char *detail_ptr = detail[0] ? detail : NULL;
        int name_len = (int)strlen(e->name);
        int kind = symbol_kind_for(e->decl_kind, NULL);

        emit_doc_symbol(&w, e->name, detail_ptr, kind, e->def_loc, name_len);

        // For shapes, attach methods + fields as children
        if (e->decl_kind == NODE_SHAPE_DECL) {
            jw_key(&w, "children");
            jw_array_start(&w);
            // Fields (if we know the shape type)
            if (e->type && e->type->kind == TYPE_SHAPE) {
                MixType *st = e->type;
                for (int fi = 0; fi < st->shape.field_count; fi++) {
                    char ftype[128] = "";
                    mix_type_to_string(st->shape.fields[fi].type, ftype, sizeof(ftype));
                    // No usable field SrcLoc on MixType; reuse the shape's location
                    // so the outline can still expand without the editor erroring.
                    emit_doc_symbol(&w, st->shape.fields[fi].name,
                                    ftype[0] ? ftype : NULL,
                                    8, // 8 = Field
                                    e->def_loc,
                                    (int)strlen(st->shape.fields[fi].name));
                    jw_object_end(&w);
                }
            }
            // Methods: any symbol whose container matches this shape
            for (int j = 0; j < doc->symbols.all_count; j++) {
                SymbolEntry *m = doc->symbols.all[j];
                if (!m->container) continue;
                if (strcmp(m->container, e->name) != 0) continue;
                char mdetail[256] = "";
                if (m->type) mix_type_to_string(m->type, mdetail, sizeof(mdetail));
                emit_doc_symbol(&w, m->name, mdetail[0] ? mdetail : NULL,
                                7 /* Method */, m->def_loc,
                                (int)strlen(m->name));
                jw_object_end(&w);
            }
            jw_array_end(&w);
        }

        jw_object_end(&w);
    }

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Inlay Hints ---

// True when the init expression makes the type obvious enough that an inline
// `: T` hint would be noise.
static bool is_obvious_literal(AstNode *e) {
    if (!e) return true;
    switch (e->kind) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
        case NODE_STRING_INTERP:
        case NODE_BOOL_LIT:
        case NODE_NONE_LIT:
        case NODE_SHAPE_LIT:    // `Vec(x: 1, y: 2)` — type name is already there
        case NODE_LIST_LIT:     // `[1, 2, 3]`
        case NODE_MAP_LIT:
        case NODE_SET_LIT:
            return true;
        default:
            return false;
    }
}

static void emit_inlay_hint(JsonWriter *w, int line, int col, const char *text) {
    jw_object_start(w);
      jw_key(w, "position");
      jw_object_start(w);
        jw_key(w, "line"); jw_int(w, line);
        jw_key(w, "character"); jw_int(w, col);
      jw_object_end(w);
      jw_key(w, "label"); jw_string(w, text);
      jw_key(w, "kind"); jw_int(w, 1);  // 1 = Type
      jw_key(w, "paddingLeft"); jw_bool(w, false);
      jw_key(w, "paddingRight"); jw_bool(w, false);
    jw_object_end(w);
}

// Forward declarations for the recursive walker.
static void hint_block(JsonWriter *w, AstNode *block, int range_start, int range_end);
static void hint_stmt(JsonWriter *w, AstNode *s, int range_start, int range_end);

static void hint_stmt(JsonWriter *w, AstNode *s, int range_start, int range_end) {
    if (!s) return;
    int line0 = s->loc.line - 1;
    if (line0 < range_start || line0 > range_end) {
        // Out of requested range — but still recurse so nested decls are
        // covered when the block straddles the boundary.
    }

    switch (s->kind) {
        case NODE_VAR_DECL: {
            const char *name = s->var_decl.name;
            if (!name || strcmp(name, "_") == 0) break;
            if (s->var_decl.type_ann) break;            // user wrote a type
            if (is_obvious_literal(s->var_decl.init_expr)) break;

            MixType *t = s->resolved_type;
            if (!t || t->kind == TYPE_VOID || t->kind == TYPE_INFER) break;
            char tbuf[128];
            mix_type_to_string(t, tbuf, sizeof(tbuf));
            char label[160];
            snprintf(label, sizeof(label), ": %s", tbuf);

            int line = s->loc.line - 1;
            int col = s->loc.col - 1 + (int)strlen(name);
            // Mutable bindings (`x! = ...`) include the `!` in the source —
            // shift the hint past it.
            if (s->var_decl.is_mutable) col += 1;
            if (line >= range_start && line <= range_end) {
                emit_inlay_hint(w, line, col, label);
            }
            break;
        }
        case NODE_IF_STMT:
            hint_block(w, s->if_stmt.then_block, range_start, range_end);
            if (s->if_stmt.else_block) {
                if (s->if_stmt.else_block->kind == NODE_BLOCK)
                    hint_block(w, s->if_stmt.else_block, range_start, range_end);
                else
                    hint_stmt(w, s->if_stmt.else_block, range_start, range_end);
            }
            break;
        case NODE_WHILE_STMT:
            hint_block(w, s->while_stmt.body, range_start, range_end);
            break;
        case NODE_FOR_STMT:
            hint_block(w, s->for_stmt.body, range_start, range_end);
            break;
        case NODE_MATCH_STMT:
            for (int i = 0; i < s->match_stmt.arm_count; i++) {
                AstNode *body = s->match_stmt.arms[i].body;
                if (body) {
                    if (body->kind == NODE_BLOCK)
                        hint_block(w, body, range_start, range_end);
                    else
                        hint_stmt(w, body, range_start, range_end);
                }
            }
            break;
        case NODE_DEFER_STMT:
            hint_stmt(w, s->defer_stmt.stmt, range_start, range_end);
            break;
        case NODE_UNSAFE_BLOCK:
            hint_block(w, s->unsafe_block.body, range_start, range_end);
            break;
        case NODE_ZONE_STMT:
            hint_block(w, s->zone_stmt.body, range_start, range_end);
            break;
        default:
            break;
    }
}

static void hint_block(JsonWriter *w, AstNode *block, int range_start, int range_end) {
    if (!block || block->kind != NODE_BLOCK) return;
    for (int i = 0; i < block->block.stmt_count; i++)
        hint_stmt(w, block->block.stmts[i], range_start, range_end);
}

static void handle_inlay_hint(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *range = json_get(params, "range");
    int rs = 0, re = 1 << 30;
    if (range) {
        JsonValue *start = json_get(range, "start");
        JsonValue *end = json_get(range, "end");
        if (start) rs = (int)json_get_int(start, "line");
        if (end)   re = (int)json_get_int(end, "line");
    }

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) { lsp_send_response(id, "[]"); return; }
    document_ensure_analyzed(doc);
    if (!doc->ast) { lsp_send_response(id, "[]"); return; }

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);

    // Walk top-level decls; descend into function/method bodies.
    AstNode *prog = doc->ast;
    if (prog->kind == NODE_PROGRAM) {
        for (int i = 0; i < prog->program.decl_count; i++) {
            AstNode *d = prog->program.decls[i];
            if (d->kind == NODE_FN_DECL) {
                hint_block(&w, d->fn_decl.body, rs, re);
            } else if (d->kind == NODE_SHAPE_DECL) {
                for (int j = 0; j < d->shape_decl.method_count; j++)
                    hint_block(&w, d->shape_decl.methods[j]->fn_decl.body, rs, re);
            } else if (d->kind == NODE_COND_DECL && d->cond_decl.active) {
                for (int j = 0; j < d->cond_decl.decl_count; j++) {
                    AstNode *cd = d->cond_decl.decls[j];
                    if (cd->kind == NODE_FN_DECL)
                        hint_block(&w, cd->fn_decl.body, rs, re);
                }
            }
        }
    }

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Rename ---

// Forward decl — defined alongside the highlight/references handlers below.
static SymbolEntry *symbol_at_cursor(LspDocument *doc, int line, int col,
                                     char *out_name, int out_size);

// Reject names that would shadow a keyword, type keyword, builtin function,
// or aren't a valid identifier. Returns NULL on success, error message on
// failure (caller surfaces to the client).
static const char *validate_rename(const char *name) {
    if (!name || !*name) return "rename: empty name";
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_'))
        return "rename: name must start with a letter or underscore";
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            return "rename: name contains an invalid character";
    }
    static const char *reserved[] = {
        "if","else","while","for","in","match","break","continue","done",
        "shape","union","extern","use","pub","type","zone","defer","unsafe",
        "and","or","not","go","run","wait","stream","yield","shared","repeat",
        "as","then","set","true","false","none",
        "int","float","bool","byte","str",
        "int8","int16","int32","int64","uint8","uint16","uint32","uint64",
        "float32","float64",
        // Common builtins (subset — name collision will be reported at edit time too)
        "print","panic","assert","len","sizeof","type_of","ord","chr",
        "alloc","bytes","free_mem","peek_u32","peek_ptr","poke_u32","poke_ptr",
        NULL
    };
    for (int i = 0; reserved[i]; i++) {
        if (strcmp(name, reserved[i]) == 0) return "rename: name is reserved";
    }
    return NULL;
}

static void handle_prepare_rename(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) { lsp_send_response(id, "null"); return; }
    document_ensure_analyzed(doc);

    char name[256];
    SymbolEntry *e = symbol_at_cursor(doc, line, col, name, sizeof(name));
    if (!e) { lsp_send_response(id, "null"); return; }

    // Find the actual token range under cursor for the editor to highlight.
    Token *tok = tokens_find_at(doc->tokens, doc->token_count, line, col);
    if (!tok) { lsp_send_response(id, "null"); return; }
    int len = (int)strlen(name);

    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
      jw_key(&w, "start");
      jw_object_start(&w);
        jw_key(&w, "line"); jw_int(&w, tok->line - 1);
        jw_key(&w, "character"); jw_int(&w, tok->col - 1);
      jw_object_end(&w);
      jw_key(&w, "end");
      jw_object_start(&w);
        jw_key(&w, "line"); jw_int(&w, tok->line - 1);
        jw_key(&w, "character"); jw_int(&w, tok->col - 1 + len);
      jw_object_end(&w);
    jw_object_end(&w);

    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// Emit one TextEdit. Helper used to keep the rename handler readable.
static void emit_text_edit(JsonWriter *w, SrcLoc loc, int len, const char *new_text) {
    int line = loc.line > 0 ? loc.line - 1 : 0;
    int col = loc.col > 0 ? loc.col - 1 : 0;
    jw_object_start(w);
      jw_key(w, "range");
      jw_object_start(w);
        jw_key(w, "start");
        jw_object_start(w);
          jw_key(w, "line"); jw_int(w, line);
          jw_key(w, "character"); jw_int(w, col);
        jw_object_end(w);
        jw_key(w, "end");
        jw_object_start(w);
          jw_key(w, "line"); jw_int(w, line);
          jw_key(w, "character"); jw_int(w, col + len);
        jw_object_end(w);
      jw_object_end(w);
      jw_key(w, "newText"); jw_string(w, new_text);
    jw_object_end(w);
}

static void handle_rename(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    const char *new_name = json_get_string(params, "newName");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    const char *err = validate_rename(new_name);
    if (err) { lsp_send_error(id, -32602, err); return; }

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) { lsp_send_response(id, "null"); return; }
    document_ensure_analyzed(doc);

    char name[256];
    SymbolEntry *e = symbol_at_cursor(doc, line, col, name, sizeof(name));
    if (!e) { lsp_send_response(id, "null"); return; }
    int name_len = (int)strlen(name);

    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
      jw_key(&w, "changes");
      jw_object_start(&w);
        // Current document edits
        jw_key(&w, uri);
        jw_array_start(&w);
        if (e->def_loc.line > 0)
            emit_text_edit(&w, e->def_loc, name_len, new_name);
        for (Reference *r = e->uses; r; r = r->next)
            emit_text_edit(&w, r->loc, r->name_len, new_name);
        jw_array_end(&w);

        // Cross-file edits via the workspace cache. Same name-based matching
        // as references — see caveat there.
        char *cur_path = lsp_uri_to_path(uri);
        for (WorkspaceFile *wf = server->workspace.head; wf; wf = wf->next) {
            if (!wf->valid) continue;
            if (cur_path && strcmp(wf->path, cur_path) == 0) continue;
            SymbolEntry *we = symbol_index_lookup(&wf->symbols, name);
            if (!we) continue;
            jw_key(&w, wf->uri);
            jw_array_start(&w);
            if (we->def_loc.line > 0)
                emit_text_edit(&w, we->def_loc, name_len, new_name);
            for (Reference *r = we->uses; r; r = r->next)
                emit_text_edit(&w, r->loc, r->name_len, new_name);
            jw_array_end(&w);
        }
        free(cur_path);
      jw_object_end(&w);
    jw_object_end(&w);

    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Formatting ---

// textDocument/formatting → run mix_format on the document source, return a
// single TextEdit that replaces the whole buffer with the formatted text.
// Crude but correct; smarter diff-based edits would only be visible as
// fewer scrollbar marks in the editor.
static void handle_formatting(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc || !doc->source) { lsp_send_response(id, "[]"); return; }

    // Format into a memory buffer.
    char *buf = NULL;
    size_t buf_size = 0;
    FILE *mem = open_memstream(&buf, &buf_size);
    if (!mem) { lsp_send_response(id, "[]"); return; }
    int rc = mix_format(doc->source, doc->filepath, mem);
    fclose(mem);
    if (rc != 0 || !buf) { free(buf); lsp_send_response(id, "[]"); return; }

    // Compute end position of the original document.
    int end_line = 0;
    int end_col = 0;
    for (const char *p = doc->source; *p; p++) {
        if (*p == '\n') { end_line++; end_col = 0; }
        else end_col++;
    }

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);
      jw_object_start(&w);
        jw_key(&w, "range");
        jw_object_start(&w);
          jw_key(&w, "start");
          jw_object_start(&w);
            jw_key(&w, "line"); jw_int(&w, 0);
            jw_key(&w, "character"); jw_int(&w, 0);
          jw_object_end(&w);
          jw_key(&w, "end");
          jw_object_start(&w);
            jw_key(&w, "line"); jw_int(&w, end_line);
            jw_key(&w, "character"); jw_int(&w, end_col);
          jw_object_end(&w);
        jw_object_end(&w);
        jw_key(&w, "newText"); jw_string(&w, buf);
      jw_object_end(&w);
    jw_array_end(&w);

    free(buf);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Code Actions ---

// Extract the suggested name from a `did you mean 'X'?` diagnostic message.
// Returns true with `out` filled, or false if the message doesn't match.
static bool parse_did_you_mean(const char *msg, char *out, int out_size) {
    const char *needle = "did you mean '";
    const char *p = strstr(msg, needle);
    if (!p) return false;
    p += strlen(needle);
    int i = 0;
    while (*p && *p != '\'' && i < out_size - 1) out[i++] = *p++;
    if (*p != '\'') return false;
    out[i] = '\0';
    return true;
}

// Echo back a diagnostic record so the editor associates the action with
// the correct underline. We reconstruct from the fields we care about
// rather than serializing the whole JsonValue.
static void emit_diag_echo(JsonWriter *w, JsonValue *d) {
    if (!d) return;
    JsonValue *range = json_get(d, "range");
    JsonValue *start = range ? json_get(range, "start") : NULL;
    JsonValue *end = range ? json_get(range, "end") : NULL;
    const char *msg = json_get_string(d, "message");
    int sev = (int)json_get_int(d, "severity");

    jw_object_start(w);
      jw_key(w, "severity"); jw_int(w, sev);
      if (start && end) {
          jw_key(w, "range");
          jw_object_start(w);
            jw_key(w, "start");
            jw_object_start(w);
              jw_key(w, "line"); jw_int(w, json_get_int(start, "line"));
              jw_key(w, "character"); jw_int(w, json_get_int(start, "character"));
            jw_object_end(w);
            jw_key(w, "end");
            jw_object_start(w);
              jw_key(w, "line"); jw_int(w, json_get_int(end, "line"));
              jw_key(w, "character"); jw_int(w, json_get_int(end, "character"));
            jw_object_end(w);
          jw_object_end(w);
      }
      if (msg) { jw_key(w, "message"); jw_string(w, msg); }
    jw_object_end(w);
}

// Emit one CodeAction whose only effect is a single TextEdit on `uri`.
static void emit_quickfix(JsonWriter *w, const char *title, const char *uri,
                          int line, int col_start, int col_end,
                          const char *new_text, JsonValue *src_diag) {
    jw_object_start(w);
      jw_key(w, "title"); jw_string(w, title);
      jw_key(w, "kind"); jw_string(w, "quickfix");
      if (src_diag) {
          jw_key(w, "diagnostics");
          jw_array_start(w);
            emit_diag_echo(w, src_diag);
          jw_array_end(w);
      }
      jw_key(w, "edit");
      jw_object_start(w);
        jw_key(w, "changes");
        jw_object_start(w);
          jw_key(w, uri);
          jw_array_start(w);
            jw_object_start(w);
              jw_key(w, "range");
              jw_object_start(w);
                jw_key(w, "start");
                jw_object_start(w);
                  jw_key(w, "line"); jw_int(w, line);
                  jw_key(w, "character"); jw_int(w, col_start);
                jw_object_end(w);
                jw_key(w, "end");
                jw_object_start(w);
                  jw_key(w, "line"); jw_int(w, line);
                  jw_key(w, "character"); jw_int(w, col_end);
                jw_object_end(w);
              jw_object_end(w);
              jw_key(w, "newText"); jw_string(w, new_text);
            jw_object_end(w);
          jw_array_end(w);
        jw_object_end(w);
      jw_object_end(w);
    jw_object_end(w);
}

static void handle_code_action(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *ctx = json_get(params, "context");
    JsonValue *diags = ctx ? json_get(ctx, "diagnostics") : NULL;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc || !diags || diags->kind != JSON_ARRAY) {
        lsp_send_response(id, "[]");
        return;
    }
    document_ensure_analyzed(doc);

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);

    for (int i = 0; i < diags->array.count; i++) {
        JsonValue *d = diags->array.items[i];
        const char *msg = json_get_string(d, "message");
        if (!msg) continue;

        char suggestion[128];
        if (!parse_did_you_mean(msg, suggestion, sizeof(suggestion))) continue;

        // Diagnostic ranges from the LSP are 1-char wide (see
        // lsp_diagnostics.c). Find the identifier token at that position to
        // get the actual word length we need to replace.
        JsonValue *range = json_get(d, "range");
        JsonValue *start = range ? json_get(range, "start") : NULL;
        if (!start) continue;
        int line0 = (int)json_get_int(start, "line");
        int col0  = (int)json_get_int(start, "character");
        Token *tok = tokens_find_at(doc->tokens, doc->token_count,
                                    line0 + 1, col0 + 1);
        if (!tok || (tok->kind != TOK_IDENT && tok->kind != TOK_IDENT_MUT))
            continue;

        char title[256];
        snprintf(title, sizeof(title), "Replace with '%s'", suggestion);
        emit_quickfix(&w, title, uri,
                      tok->line - 1,
                      tok->col - 1,
                      tok->col - 1 + tok->length,
                      suggestion, d);
    }

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Workspace Symbols ---

// Case-insensitive substring check.
static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen) return true;
    }
    return false;
}

static void handle_workspace_symbol(LspServer *server, int64_t id, JsonValue *params) {
    const char *query = json_get_string(params, "query");
    if (!query) query = "";

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);

    int emitted = 0;
    const int LIMIT = 256;  // editors render large lists badly

    // Walk the workspace cache (analyzed files).
    for (WorkspaceFile *wf = server->workspace.head; wf && emitted < LIMIT; wf = wf->next) {
        if (!wf->valid) continue;
        for (int i = 0; i < wf->symbols.all_count && emitted < LIMIT; i++) {
            SymbolEntry *e = wf->symbols.all[i];
            if (e->decl_kind == NODE_VAR_DECL) continue;  // locals are noise here
            if (!str_contains_ci(e->name, query)) continue;

            int line = e->def_loc.line > 0 ? e->def_loc.line - 1 : 0;
            int col = e->def_loc.col > 0 ? e->def_loc.col - 1 : 0;
            int name_len = (int)strlen(e->name);

            // Use SymbolInformation (LSP 3.16+ also has WorkspaceSymbol; both work)
            jw_object_start(&w);
              jw_key(&w, "name"); jw_string(&w, e->name);
              jw_key(&w, "kind");
              if (e->container) jw_int(&w, 7);                      // Method
              else if (e->decl_kind == NODE_FN_DECL) jw_int(&w, 12); // Function
              else if (e->decl_kind == NODE_SHAPE_DECL) jw_int(&w, 23);
              else if (e->decl_kind == NODE_CONST_DECL) jw_int(&w, 14);
              else jw_int(&w, 13);                                   // Variable fallback
              if (e->container) {
                  jw_key(&w, "containerName"); jw_string(&w, e->container);
              }
              jw_key(&w, "location");
              jw_object_start(&w);
                jw_key(&w, "uri"); jw_string(&w, wf->uri);
                jw_key(&w, "range");
                jw_object_start(&w);
                  jw_key(&w, "start");
                  jw_object_start(&w);
                    jw_key(&w, "line"); jw_int(&w, line);
                    jw_key(&w, "character"); jw_int(&w, col);
                  jw_object_end(&w);
                  jw_key(&w, "end");
                  jw_object_start(&w);
                    jw_key(&w, "line"); jw_int(&w, line);
                    jw_key(&w, "character"); jw_int(&w, col + name_len);
                  jw_object_end(&w);
                jw_object_end(&w);
              jw_object_end(&w);
            jw_object_end(&w);
            emitted++;
        }
    }

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Document Highlight & Find References ---

// Both handlers do the same thing: find the symbol at the cursor and emit its
// uses. documentHighlight is current-doc, write-aware. references can include
// the def location and (later) cross-file uses from the workspace cache.

static void emit_range_obj(JsonWriter *w, int line0, int col0, int len) {
    jw_object_start(w);
      jw_key(w, "line"); jw_int(w, line0);
      jw_key(w, "character"); jw_int(w, col0);
    jw_object_end(w);
    (void)len;
}

static void emit_loc_range(JsonWriter *w, SrcLoc loc, int len) {
    int line = loc.line > 0 ? loc.line - 1 : 0;
    int col = loc.col > 0 ? loc.col - 1 : 0;
    jw_key(w, "range");
    jw_object_start(w);
      jw_key(w, "start"); emit_range_obj(w, line, col, len);
      jw_key(w, "end"); emit_range_obj(w, line, col + len, len);
    jw_object_end(w);
}

// Look up the identifier the cursor is on. Returns NULL if not on an ident.
static SymbolEntry *symbol_at_cursor(LspDocument *doc, int line, int col,
                                     char *out_name, int out_size) {
    if (!doc || !doc->analysis_valid) return NULL;
    Token *tok = tokens_find_at(doc->tokens, doc->token_count, line, col);
    if (!tok || (tok->kind != TOK_IDENT && tok->kind != TOK_IDENT_MUT)) return NULL;
    if (!token_ident_name(tok, out_name, out_size)) return NULL;
    return symbol_index_lookup(&doc->symbols, out_name);
}

static void handle_document_highlight(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) { lsp_send_response(id, "[]"); return; }
    document_ensure_analyzed(doc);

    char name[256];
    SymbolEntry *e = symbol_at_cursor(doc, line, col, name, sizeof(name));
    if (!e) { lsp_send_response(id, "[]"); return; }

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);
    int name_len = (int)strlen(e->name);

    // Definition
    if (e->def_loc.line > 0) {
        jw_object_start(&w);
        emit_loc_range(&w, e->def_loc, name_len);
        jw_key(&w, "kind"); jw_int(&w, 1); // 1 = Text
        jw_object_end(&w);
    }

    for (Reference *r = e->uses; r; r = r->next) {
        jw_object_start(&w);
        emit_loc_range(&w, r->loc, r->name_len);
        // 1=Text, 2=Read, 3=Write
        jw_key(&w, "kind"); jw_int(&w, r->is_write ? 3 : 2);
        jw_object_end(&w);
    }

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

static void handle_references(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    bool include_decl = false;
    JsonValue *ctx = json_get(params, "context");
    if (ctx) {
        JsonValue *idv = json_get(ctx, "includeDeclaration");
        if (idv && idv->kind == JSON_BOOL) include_decl = idv->boolean;
    }

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc) { lsp_send_response(id, "[]"); return; }
    document_ensure_analyzed(doc);

    char name[256];
    SymbolEntry *e = symbol_at_cursor(doc, line, col, name, sizeof(name));
    if (!e) { lsp_send_response(id, "[]"); return; }

    JsonWriter w;
    jw_init(&w);
    jw_array_start(&w);
    int name_len = (int)strlen(e->name);

    if (include_decl && e->def_loc.line > 0) {
        jw_object_start(&w);
          jw_key(&w, "uri"); jw_string(&w, uri);
          emit_loc_range(&w, e->def_loc, name_len);
        jw_object_end(&w);
    }
    for (Reference *r = e->uses; r; r = r->next) {
        jw_object_start(&w);
          jw_key(&w, "uri"); jw_string(&w, uri);
          emit_loc_range(&w, r->loc, r->name_len);
        jw_object_end(&w);
    }

    // Cross-file uses: walk the workspace cache for symbols with the same
    // name. Skip the file we already covered above. This is name-based
    // matching, not type-aware — same-named locals in unrelated files will
    // appear. Acceptable for an MVP; can be tightened later by scoping
    // top-level functions/shapes to the cached file's pub/use edges.
    char *cur_path = lsp_uri_to_path(uri);
    for (WorkspaceFile *wf = server->workspace.head; wf; wf = wf->next) {
        if (!wf->valid) continue;
        if (cur_path && strcmp(wf->path, cur_path) == 0) continue;
        SymbolEntry *we = symbol_index_lookup(&wf->symbols, name);
        if (!we) continue;
        if (include_decl && we->def_loc.line > 0) {
            jw_object_start(&w);
              jw_key(&w, "uri"); jw_string(&w, wf->uri);
              emit_loc_range(&w, we->def_loc, name_len);
            jw_object_end(&w);
        }
        for (Reference *r = we->uses; r; r = r->next) {
            jw_object_start(&w);
              jw_key(&w, "uri"); jw_string(&w, wf->uri);
              emit_loc_range(&w, r->loc, r->name_len);
            jw_object_end(&w);
        }
    }
    free(cur_path);

    jw_array_end(&w);
    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Signature Help ---

static void handle_signature_help(LspServer *server, int64_t id, JsonValue *params) {
    const char *uri = json_get_string(json_get(params, "textDocument"), "uri");
    JsonValue *pos = json_get(params, "position");
    int line = (int)json_get_int(pos, "line") + 1;
    int col = (int)json_get_int(pos, "character") + 1;

    LspDocument *doc = docstore_find(&server->documents, uri);
    if (!doc || !doc->source) {
        lsp_send_response(id, "null");
        return;
    }
    document_ensure_analyzed(doc);

    // Find the line in source
    const char *src = doc->source;
    int cur_line = 1;
    while (*src && cur_line < line) {
        if (*src == '\n') cur_line++;
        src++;
    }

    // Get the text up to cursor position
    int cursor = col - 1;
    char line_buf[1024];
    int lb = 0;
    while (src[lb] && src[lb] != '\n' && lb < cursor && lb < 1023) {
        line_buf[lb] = src[lb];
        lb++;
    }
    line_buf[lb] = '\0';

    // Scan backwards from cursor to find the function name and count commas for active param
    int paren_depth = 0;
    int comma_count = 0;
    int func_end = -1;

    for (int i = lb - 1; i >= 0; i--) {
        char c = line_buf[i];
        if (c == ')') paren_depth++;
        else if (c == '(') {
            if (paren_depth > 0) {
                paren_depth--;
            } else {
                // Found the opening paren — function name is before it
                func_end = i;
                break;
            }
        } else if (c == ',' && paren_depth == 0) {
            comma_count++;
        }
    }

    if (func_end < 0) {
        lsp_send_response(id, "null");
        return;
    }

    // Extract function name (scan back from func_end)
    int name_end = func_end - 1;
    while (name_end >= 0 && (line_buf[name_end] == ' ' || line_buf[name_end] == '\t')) name_end--;
    if (name_end < 0) {
        lsp_send_response(id, "null");
        return;
    }

    // Check for method call: obj.method(
    int name_start = name_end;
    while (name_start > 0 && (isalnum((unsigned char)line_buf[name_start - 1]) || line_buf[name_start - 1] == '_' || line_buf[name_start - 1] == '!'))
        name_start--;

    char func_name[256];
    int nlen = name_end - name_start + 1;
    if (nlen <= 0 || nlen > 255) {
        lsp_send_response(id, "null");
        return;
    }
    memcpy(func_name, line_buf + name_start, nlen);
    func_name[nlen] = '\0';

    // Strip trailing ! from method names for lookup
    int fnl = (int)strlen(func_name);
    if (fnl > 0 && func_name[fnl - 1] == '!') func_name[fnl - 1] = '\0';

    // Look up the function
    SymbolEntry *entry = symbol_index_lookup(&doc->symbols, func_name);
    if (!entry) {
        lsp_send_response(id, "null");
        return;
    }

    MixType *ft = (entry->type && entry->type->kind == TYPE_FUNC) ? entry->type : NULL;
    int pc = ft ? ft->func.param_count : entry->param_name_count;

    if (pc == 0 && !ft) {
        // No type info and no param names — can't show signature
        lsp_send_response(id, "null");
        return;
    }

    // Build the signature label: "func_name(param1: type1, param2: type2) -> ret"
    char sig_label[1024];
    int off = snprintf(sig_label, sizeof(sig_label), "%s(", entry->name);

    // Track parameter label offsets for highlighting
    int param_offsets[64][2]; // [start, end] for each param

    for (int i = 0; i < pc && i < 64; i++) {
        if (i > 0) off += snprintf(sig_label + off, sizeof(sig_label) - off, ", ");
        int pstart = off;

        const char *pname = (i < entry->param_name_count && entry->param_names[i])
            ? entry->param_names[i] : "p";

        // Prefer AST-derived type strings (always accurate), fall back to MixType
        if (entry->param_type_strs && i < entry->param_name_count && entry->param_type_strs[i]) {
            off += snprintf(sig_label + off, sizeof(sig_label) - off, "%s: %s", pname, entry->param_type_strs[i]);
        } else if (ft && i < ft->func.param_count && ft->func.param_types[i]) {
            char type_str[128];
            mix_type_to_string(ft->func.param_types[i], type_str, sizeof(type_str));
            off += snprintf(sig_label + off, sizeof(sig_label) - off, "%s: %s", pname, type_str);
        } else {
            off += snprintf(sig_label + off, sizeof(sig_label) - off, "%s", pname);
        }

        param_offsets[i][0] = pstart;
        param_offsets[i][1] = off;
    }

    off += snprintf(sig_label + off, sizeof(sig_label) - off, ")");

    // Prefer AST-derived return type string
    if (entry->return_type_str) {
        off += snprintf(sig_label + off, sizeof(sig_label) - off, " -> %s", entry->return_type_str);
    } else if (ft && ft->func.return_type && ft->func.return_type->kind != TYPE_VOID) {
        char ret_str[128];
        mix_type_to_string(ft->func.return_type, ret_str, sizeof(ret_str));
        off += snprintf(sig_label + off, sizeof(sig_label) - off, " -> %s", ret_str);
    }

    // Build JSON response
    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
      jw_key(&w, "signatures");
      jw_array_start(&w);
        jw_object_start(&w);
          jw_key(&w, "label"); jw_string(&w, sig_label);
          jw_key(&w, "parameters");
          jw_array_start(&w);
            for (int i = 0; i < pc && i < 64; i++) {
                jw_object_start(&w);
                  jw_key(&w, "label");
                  jw_array_start(&w);
                    jw_int(&w, param_offsets[i][0]);
                    jw_int(&w, param_offsets[i][1]);
                  jw_array_end(&w);
                jw_object_end(&w);
            }
          jw_array_end(&w);
        jw_object_end(&w);
      jw_array_end(&w);
      jw_key(&w, "activeSignature"); jw_int(&w, 0);
      jw_key(&w, "activeParameter"); jw_int(&w, comma_count < pc ? comma_count : pc - 1);
    jw_object_end(&w);

    lsp_send_response(id, w.buf);
    jw_free(&w);
}

// --- Dispatch ---

void lsp_server_dispatch(LspServer *server, JsonValue *msg, Arena *scratch) {
    (void)scratch;

    const char *method = json_get_string(msg, "method");
    JsonValue *id_val = json_get(msg, "id");
    JsonValue *params = json_get(msg, "params");
    int64_t id = (id_val && id_val->kind == JSON_INT) ? id_val->integer : -1;

    if (!method) {
        if (id >= 0) lsp_send_error(id, -32600, "Missing method");
        return;
    }

    // Lifecycle
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(server, id, params);
    } else if (strcmp(method, "initialized") == 0) {
        // no-op
    } else if (strcmp(method, "shutdown") == 0) {
        server->shutdown_requested = true;
        lsp_send_response(id, "null");
    } else if (strcmp(method, "exit") == 0) {
        exit(server->shutdown_requested ? 0 : 1);
    }
    // Document sync
    else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(server, params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(server, params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(server, params);
    } else if (strcmp(method, "textDocument/didSave") == 0) {
        handle_did_save(server, params);
    }
    // Language features
    else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover_request(server, id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_goto_definition(server, id, params);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion_request(server, id, params);
    } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
        handle_signature_help(server, id, params);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbol(server, id, params);
    } else if (strcmp(method, "textDocument/documentHighlight") == 0) {
        handle_document_highlight(server, id, params);
    } else if (strcmp(method, "textDocument/references") == 0) {
        handle_references(server, id, params);
    } else if (strcmp(method, "workspace/symbol") == 0) {
        handle_workspace_symbol(server, id, params);
    } else if (strcmp(method, "textDocument/inlayHint") == 0) {
        handle_inlay_hint(server, id, params);
    } else if (strcmp(method, "textDocument/prepareRename") == 0) {
        handle_prepare_rename(server, id, params);
    } else if (strcmp(method, "textDocument/rename") == 0) {
        handle_rename(server, id, params);
    } else if (strcmp(method, "textDocument/codeAction") == 0) {
        handle_code_action(server, id, params);
    } else if (strcmp(method, "textDocument/formatting") == 0) {
        handle_formatting(server, id, params);
    }
    // Unknown
    else {
        if (id >= 0) lsp_send_error(id, -32601, "Method not found");
    }
}
