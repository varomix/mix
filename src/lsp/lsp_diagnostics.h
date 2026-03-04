#ifndef LSP_DIAGNOSTICS_H
#define LSP_DIAGNOSTICS_H

#include "../errors.h"

typedef struct {
    DiagSeverity severity;
    int line;
    int col;
    char *message;
} LspDiagnostic;

typedef struct {
    LspDiagnostic *items;
    int count;
    int capacity;
} DiagnosticList;

void diag_list_init(DiagnosticList *list);
void diag_list_clear(DiagnosticList *list);
void diag_list_destroy(DiagnosticList *list);

// Callback to install into the error system
void lsp_diagnostic_callback(DiagSeverity severity, SrcLoc loc,
                              const char *message, void *userdata);

// Emit publishDiagnostics notification to the editor
void lsp_publish_diagnostics(const char *uri, DiagnosticList *list);

#endif // LSP_DIAGNOSTICS_H
