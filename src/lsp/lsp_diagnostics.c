#include "lsp_diagnostics.h"
#include "lsp_transport.h"
#include "lsp_json.h"

void diag_list_init(DiagnosticList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void diag_list_clear(DiagnosticList *list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].message);
    }
    list->count = 0;
}

void diag_list_destroy(DiagnosticList *list) {
    diag_list_clear(list);
    free(list->items);
    list->items = NULL;
    list->capacity = 0;
}

void lsp_diagnostic_callback(DiagSeverity severity, SrcLoc loc,
                              const char *message, void *userdata) {
    DiagnosticList *list = (DiagnosticList *)userdata;
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 32;
        list->items = realloc(list->items, sizeof(LspDiagnostic) * list->capacity);
    }
    LspDiagnostic *d = &list->items[list->count++];
    d->severity = severity;
    d->line = loc.line;
    d->col = loc.col;
    d->message = strdup(message);
}

void lsp_publish_diagnostics(const char *uri, DiagnosticList *list) {
    JsonWriter w;
    jw_init(&w);
    jw_object_start(&w);
    jw_key(&w, "uri"); jw_string(&w, uri);
    jw_key(&w, "diagnostics");
    jw_array_start(&w);
    for (int i = 0; i < list->count; i++) {
        LspDiagnostic *d = &list->items[i];
        jw_object_start(&w);

        // severity: 1=Error, 2=Warning, 3=Info
        jw_key(&w, "severity");
        jw_int(&w, d->severity == DIAG_ERROR ? 1 :
                    d->severity == DIAG_WARNING ? 2 : 3);

        // range
        int line = d->line > 0 ? d->line - 1 : 0;
        int col = d->col > 0 ? d->col - 1 : 0;
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
            jw_key(&w, "character"); jw_int(&w, col + 1);
          jw_object_end(&w);
        jw_object_end(&w);

        jw_key(&w, "message"); jw_string(&w, d->message);
        jw_key(&w, "source"); jw_string(&w, "mix");

        jw_object_end(&w);
    }
    jw_array_end(&w);
    jw_object_end(&w);

    lsp_send_notification("textDocument/publishDiagnostics", w.buf);
    jw_free(&w);
}
