#include "errors.h"
#include <unistd.h>

static const char *g_source = NULL;
static const char *g_filename = NULL;
static int g_error_count = 0;
static DiagnosticCallback g_diag_callback = NULL;
static void *g_diag_userdata = NULL;

// Color support
static bool use_color = false;

// Error limit
#define MAX_ERRORS 10
static bool error_limit_hit = false;

// ANSI escape codes
#define BOLD      "\033[1m"
#define RED       "\033[31m"
#define YELLOW    "\033[33m"
#define CYAN      "\033[36m"
#define RESET     "\033[0m"
#define BOLD_RED  "\033[1;31m"
#define BOLD_YEL  "\033[1;33m"
#define BOLD_CYAN "\033[1;36m"

void errors_init(void) {
    // Respect NO_COLOR convention (https://no-color.org/)
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0') {
        use_color = false;
        return;
    }
    // Enable color only if stderr is a terminal
    use_color = isatty(STDERR_FILENO) != 0;
}

void errors_set_callback(DiagnosticCallback cb, void *userdata) {
    g_diag_callback = cb;
    g_diag_userdata = userdata;
}

void errors_reset(void) {
    g_error_count = 0;
    error_limit_hit = false;
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

    // Calculate gutter width for line number
    int line_num = loc.line;
    int gutter_width = 1;
    { int n = line_num; while (n >= 10) { gutter_width++; n /= 10; } }

    // Print the source line with line number gutter
    if (use_color) {
        fprintf(stderr, " %*d " BOLD CYAN "|" RESET " %.*s\n",
                gutter_width, line_num, (int)(line_end - line_start), line_start);
        // Print the caret line
        fprintf(stderr, " %*s " BOLD CYAN "|" RESET " ", gutter_width, "");
    } else {
        fprintf(stderr, " %*d | %.*s\n",
                gutter_width, line_num, (int)(line_end - line_start), line_start);
        fprintf(stderr, " %*s | ", gutter_width, "");
    }
    for (int i = 1; i < loc.col; i++) fprintf(stderr, " ");
    if (use_color)
        fprintf(stderr, BOLD_RED "^" RESET "\n");
    else
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

    if (use_color) {
        // Bold location
        fprintf(stderr, BOLD "%s:%d:%d: " RESET, fname, loc.line, loc.col);
        // Colored severity label
        switch (severity) {
            case DIAG_ERROR:   fprintf(stderr, BOLD_RED "error: " RESET); break;
            case DIAG_WARNING: fprintf(stderr, BOLD_YEL "warning: " RESET); break;
            case DIAG_NOTE:    fprintf(stderr, BOLD_CYAN "note: " RESET); break;
        }
    } else {
        fprintf(stderr, "%s:%d:%d: %s: ", fname, loc.line, loc.col, level);
    }
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    print_source_line(loc);
}

void mix_error(SrcLoc loc, const char *fmt, ...) {
    if (error_limit_hit) return;
    va_list args;
    va_start(args, fmt);
    report("error", DIAG_ERROR, loc, fmt, args);
    va_end(args);
    g_error_count++;
    if (g_error_count >= MAX_ERRORS) {
        error_limit_hit = true;
        fprintf(stderr, "mix: too many errors emitted, stopping\n");
    }
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

bool mix_error_limit_reached(void) {
    return error_limit_hit;
}
