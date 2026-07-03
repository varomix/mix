#include "lsp_server.h"
#include "lsp_transport.h"
#include "lsp_json.h"
#include "../arena.h"
#include "../cbind.h"
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    fprintf(stderr, "mix-lsp: starting\n");

    // Get directory of the mix-lsp executable
    char exe_dir[1024] = "";
    {
        char exe_path[1024];
        #ifdef __APPLE__
        uint32_t exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) == 0) {
            char *resolved = realpath(exe_path, NULL);
            if (resolved) {
                char *d = dirname(resolved);
                snprintf(exe_dir, sizeof(exe_dir), "%s", d);
                free(resolved);
            }
        }
        #elif defined(__linux__)
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            char *d = dirname(exe_path);
            snprintf(exe_dir, sizeof(exe_dir), "%s", d);
        }
        #endif
    }

    // Let cbind.c resolve vendor paths relative to the project root
    cbind_set_project_root(exe_dir);

    LspServer server;
    lsp_server_init(&server, exe_dir);

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
