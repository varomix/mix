#include "lsp_server.h"
#include "lsp_transport.h"
#include "lsp_position.h"
#include "lsp_hover.h"
#include <ctype.h>

void lsp_server_init(LspServer *server) {
    memset(server, 0, sizeof(LspServer));
    server->documents = docstore_create();
}

void lsp_server_destroy(LspServer *server) {
    docstore_destroy(&server->documents);
    free(server->root_uri);
    free(server->root_path);
}

// --- Handlers ---

static void handle_initialize(LspServer *server, int64_t id, JsonValue *params) {
    JsonValue *root = json_get(params, "rootUri");
    if (root && root->kind == JSON_STRING) {
        server->root_uri = strdup(root->string.data);
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
    if (!doc || !doc->analysis_valid) {
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

    char hover_md[1024];
    snprintf(hover_md, sizeof(hover_md), "```mix\n%s: %s\n```", name, type_str);

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
    if (!doc || !doc->analysis_valid) {
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
            "break", "continue", "done", "shape", "extern",
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
    }

    } // end interp_mode else

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
    }
    // Unknown
    else {
        if (id >= 0) lsp_send_error(id, -32601, "Method not found");
    }
}
