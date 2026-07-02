#ifndef CBIND_H
#define CBIND_H

#include <stdbool.h>

// Generate a .mix binding file from C headers.
// header_path: path to a .h file or directory containing .h files
// out_path:    output .mix file path
// lib_name:    extern library name (e.g. "SDL3"), NULL to derive from path
// verbose:     print commands to stderr
// Returns 0 on success, 1 on failure.
int cbind_generate(const char *header_path, const char *out_path,
                   const char *lib_name, bool verbose);

// Generate MIX binding source as an in-memory string.
// source_dir: directory of the .mix file (NULL to skip), forwarded to
// resolve_header_path.
// Returns a malloc'd string (caller must free), or NULL on failure.
char *cbind_generate_string(const char *header_path, const char *lib_name,
                            bool verbose, const char *source_dir);

// Resolve a C header path using source directory, CPPFLAGS -I directories,
// and vendor paths. source_dir: directory of the .mix file (NULL to skip).
// Returns a malloc'd absolute path string, or NULL if not found.
char *resolve_header_path(const char *path, const char *source_dir);

// Set the project root directory (exe_dir/..) so vendor paths resolve correctly
// even when the compiler is run from a subdirectory.
void cbind_set_project_root(const char *root);

#endif // CBIND_H
