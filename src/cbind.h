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

#endif // CBIND_H
