#include "lsp_transport.h"
#include <inttypes.h>

bool lsp_read_message(LspMessage *msg) {
    int content_length = -1;
    char line[1024];

    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
        if (strncmp(line, "Content-Length:", 15) == 0) {
            content_length = atoi(line + 15);
        }
    }

    if (content_length < 0) return false;

    msg->content = malloc(content_length + 1);
    if (!msg->content) return false;

    int total_read = 0;
    while (total_read < content_length) {
        int n = (int)fread(msg->content + total_read, 1,
                           content_length - total_read, stdin);
        if (n <= 0) { free(msg->content); return false; }
        total_read += n;
    }
    msg->content[content_length] = '\0';
    msg->content_length = content_length;
    return true;
}

void lsp_send_message(const char *json, int length) {
    fprintf(stdout, "Content-Length: %d\r\n\r\n", length);
    fwrite(json, 1, length, stdout);
    fflush(stdout);
}

void lsp_send_response(int64_t id, const char *result_json) {
    char header[128];
    int hlen = snprintf(header, sizeof(header),
        "{\"jsonrpc\":\"2.0\",\"id\":%" PRId64 ",\"result\":", id);

    int rlen = (int)strlen(result_json);
    int total = hlen + rlen + 1; // +1 for closing }
    char *buf = malloc(total + 1);
    memcpy(buf, header, hlen);
    memcpy(buf + hlen, result_json, rlen);
    buf[hlen + rlen] = '}';
    buf[total] = '\0';

    lsp_send_message(buf, total);
    free(buf);
}

void lsp_send_notification(const char *method, const char *params_json) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":", method);

    int plen = (int)strlen(params_json);
    int total = hlen + plen + 1;
    char *buf = malloc(total + 1);
    memcpy(buf, header, hlen);
    memcpy(buf + hlen, params_json, plen);
    buf[hlen + plen] = '}';
    buf[total] = '\0';

    lsp_send_message(buf, total);
    free(buf);
}

void lsp_send_error(int64_t id, int code, const char *message) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%" PRId64
        ",\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        id, code, message);
    lsp_send_message(buf, n);
}
