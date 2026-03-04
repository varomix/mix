#include "errors.h"

static const char *g_source = NULL;
static const char *g_filename = NULL;
static int g_error_count = 0;
static DiagnosticCallback g_diag_callback = NULL;
static void *g_diag_userdata = NULL;

void errors_set_callback(DiagnosticCallback cb, void *userdata) {
    g_diag_callback = cb;
    g_diag_userdata = userdata;
}

void errors_reset(void) {
    g_error_count = 0;
}

void errors_set_source(const char *source, const char *filename) {
    g_source = source;
    g_filename = filename;
}

// Find the start of line number `line` (1-based) in source
static const char *find_line_start(int line) {
    if (!g_source) return NULL;
    const char *p = g_source;
    int current_line = 1;
    while (*p && current_line < line) {
        if (*p == '\n') current_line++;
        p++;
    }
    return (current_line == line) ? p : NULL;
}

static void print_source_line(SrcLoc loc) {
    const char *line_start = find_line_start(loc.line);
    if (!line_start) return;

    // Find end of line
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;

    // Print the source line
    fprintf(stderr, "  %.*s\n", (int)(line_end - line_start), line_start);

    // Print the caret
    fprintf(stderr, "  ");
    for (int i = 1; i < loc.col; i++) fprintf(stderr, " ");
    fprintf(stderr, "^\n");
}

static void report(const char *level, DiagSeverity severity, SrcLoc loc,
                   const char *fmt, va_list args) {
    if (g_diag_callback) {
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        g_diag_callback(severity, loc, buf, g_diag_userdata);
        return;
    }
    const char *fname = loc.filename ? loc.filename : (g_filename ? g_filename : "<unknown>");
    fprintf(stderr, "%s:%d:%d: %s: ", fname, loc.line, loc.col, level);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    print_source_line(loc);
}

void mix_error(SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    report("error", DIAG_ERROR, loc, fmt, args);
    va_end(args);
    g_error_count++;
}

void mix_warning(SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    report("warning", DIAG_WARNING, loc, fmt, args);
    va_end(args);
}

void mix_note(SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    report("note", DIAG_NOTE, loc, fmt, args);
    va_end(args);
}

int mix_error_count(void) {
    return g_error_count;
}
