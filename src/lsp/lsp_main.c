#include "lsp_server.h"
#include "lsp_transport.h"
#include "lsp_json.h"
#include "../arena.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    fprintf(stderr, "mix-lsp: starting\n");

    LspServer server;
    lsp_server_init(&server);

    LspMessage msg;
    while (lsp_read_message(&msg)) {
        Arena scratch = arena_create(64 * 1024);
        JsonValue *json = json_parse(msg.content, msg.content_length, &scratch);

        if (json) {
            lsp_server_dispatch(&server, json, &scratch);
        }

        arena_destroy(&scratch);
        free(msg.content);
    }

    lsp_server_destroy(&server);
    fprintf(stderr, "mix-lsp: stopped\n");
    return 0;
}
