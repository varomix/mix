#ifndef LSP_SERVER_H
#define LSP_SERVER_H

#include "lsp_document.h"
#include "lsp_json.h"
#include "lsp_workspace.h"

typedef struct {
    DocumentStore documents;
    WorkspaceCache workspace;
    bool initialized;
    bool shutdown_requested;
    char *root_uri;
    char *root_path;
    char exe_dir[1024];
} LspServer;

void lsp_server_init(LspServer *server, const char *exe_dir);
void lsp_server_destroy(LspServer *server);
void lsp_server_dispatch(LspServer *server, JsonValue *msg, Arena *scratch);

#endif // LSP_SERVER_H
