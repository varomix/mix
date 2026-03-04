#ifndef LSP_TRANSPORT_H
#define LSP_TRANSPORT_H

#include "../mix.h"

typedef struct {
    char *content;
    int content_length;
} LspMessage;

// Read one LSP message from stdin. Returns false on EOF.
bool lsp_read_message(LspMessage *msg);

// Send raw JSON as an LSP message to stdout.
void lsp_send_message(const char *json, int length);

// Send a JSON-RPC response with the given id and result JSON.
void lsp_send_response(int64_t id, const char *result_json);

// Send a JSON-RPC notification (no id).
void lsp_send_notification(const char *method, const char *params_json);

// Send a JSON-RPC error response.
void lsp_send_error(int64_t id, int code, const char *message);

#endif // LSP_TRANSPORT_H
