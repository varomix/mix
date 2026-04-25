#ifndef FMT_H
#define FMT_H

#include <stdio.h>

// Format `source` (a complete .mix program) and write the result to `out`.
// Returns 0 on success, non-zero if the input fails to lex.
//
// The formatter is token-based: it lexes the input, then emits each token
// with normalized surrounding whitespace and indentation derived from
// INDENT/DEDENT. Standalone `// ...` comments are preserved on their own
// lines; trailing comments stay glued to the line they were on.
//
// Idempotent: running it twice produces the same output as running it once.
int mix_format(const char *source, const char *filename, FILE *out);

#endif // FMT_H
