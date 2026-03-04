#ifndef MIX_H
#define MIX_H

#define MIX_VERSION "0.1.0"
#define MIX_VERSION_DATE "2026-03-04"

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
