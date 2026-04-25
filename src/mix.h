#ifndef MIX_H
#define MIX_H

#define MIX_VERSION "0.1.0"
/* MIX_VERSION_DATE is normally injected by the Makefile via
 * -DMIX_VERSION_DATE="..." so it tracks the build date. The fallback
 * below only kicks in when building outside the Makefile. */
#ifndef MIX_VERSION_DATE
#define MIX_VERSION_DATE "0000-00-00"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

// Source location
typedef struct {
    const char *filename;
    int line;
    int col;
} SrcLoc;

// Forward declarations
typedef struct Arena Arena;
typedef struct Token Token;
typedef struct AstNode AstNode;
typedef struct MixType MixType;

#endif // MIX_H
