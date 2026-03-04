#ifndef ERRORS_H
#define ERRORS_H

#include "mix.h"

// Diagnostic severity levels
typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE
} DiagSeverity;

// Callback for intercepting diagnostics (used by LSP)
typedef void (*DiagnosticCallback)(DiagSeverity severity, SrcLoc loc,
                                    const char *message, void *userdata);

// Set a diagnostic callback. Pass NULL to restore default stderr behavior.
void errors_set_callback(DiagnosticCallback cb, void *userdata);

// Reset the global error count (needed for re-analysis in LSP)
void errors_reset(void);

// Set the source text for error reporting (enables source line display)
void errors_set_source(const char *source, const char *filename);

// Report an error with source location
void mix_error(SrcLoc loc, const char *fmt, ...);

// Report a warning
void mix_warning(SrcLoc loc, const char *fmt, ...);

// Report a note (additional context)
void mix_note(SrcLoc loc, const char *fmt, ...);

// Get total error count
int mix_error_count(void);

#endif // ERRORS_H
