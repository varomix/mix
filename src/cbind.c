#include "cbind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

// ---- Internal types ----

typedef struct {
    char name[256];
    char c_name[256];   // optional C symbol name (for aliased functions like glad_glClear)
    char ret_mix[64];
    char params_mix[2048];
    bool is_variadic;
} CFunc;

typedef struct {
    char name[256];
    long long int_value;
    double float_value;
    bool is_float;
    bool is_hex;
    bool is_shape_lit;
    char shape_name[128];       // e.g., "Color"
    char shape_values[512];     // e.g., "r: 200, g: 200, b: 200, a: 255"
} CConst;

typedef struct {
    char name[128];       // struct/typedef name (e.g., "Vector2", "Color")
    char fields[2048];    // MIX shape fields string
    char field_names[64][128]; // individual field names for shape-lit constants
    int field_name_count;
    bool is_union;        // true for typedef union
} CShape;

#define MAX_FUNCS  4096
#define MAX_CONSTS 16384
#define MAX_SHAPES 512
#define MAX_BUF    (16 * 1024 * 1024) // 16MB preprocessor output limit

static CFunc funcs[MAX_FUNCS];
static int func_count = 0;
static CConst consts[MAX_CONSTS];
static int const_count = 0;
static CShape shapes[MAX_SHAPES];
static int shape_count = 0;

// Typedef alias table: maps e.g. "Uint32" → "uint32_t" so c_type_to_mix resolves them
#define MAX_TYPEDEFS 256
static struct { char alias[128]; char target[128]; } typedefs[MAX_TYPEDEFS];
static int typedef_count = 0;

// Enum typedef names (for resolving enum types to int32 in c_type_to_mix)
#define MAX_ENUM_NAMES 1024
static char enum_names[MAX_ENUM_NAMES][256];
static int enum_name_count = 0;

// ---- Helpers ----

static void trim(char *s) {
    // trim leading
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    // trim trailing
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Check if a function name is already recorded
static bool func_exists(const char *name) {
    for (int i = 0; i < func_count; i++) {
        if (strcmp(funcs[i].name, name) == 0) return true;
    }
    return false;
}

// Check if a type name is a known shape (parsed struct)
static bool shape_exists(const char *name) {
    for (int i = 0; i < shape_count; i++) {
        if (strcmp(shapes[i].name, name) == 0) return true;
    }
    return false;
}

// Check if a parameter name is a MIX reserved word
static bool is_reserved_word(const char *name) {
    static const char *reserved[] = {
        "if", "else", "while", "for", "in", "match", "break", "continue",
        "done", "shape", "union", "extern", "use", "pub", "type", "zone", "defer",
        "unsafe", "and", "or", "not", "go", "run", "wait", "stream",
        "yield", "shared", "repeat", "as", "then", "set", "true", "false",
        "none", "int", "float", "bool", "byte", "str",
        "int8", "int16", "int32", "int64",
        "uint8", "uint16", "uint32", "uint64",
        "float32", "float64",
        NULL
    };
    for (int i = 0; reserved[i]; i++) {
        if (strcmp(name, reserved[i]) == 0) return true;
    }
    return false;
}

// ---- C type to MIX type mapping ----

// Strips qualifiers and maps a C type string to a MIX type string.
// Returns "" for void, NULL if the type can't be represented (skip the function).
static const char *c_type_to_mix(const char *ctype) {
    char buf[512];
    strncpy(buf, ctype, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);

    // Strip qualifiers
    const char *strip[] = {"const ", "volatile ", "restrict ", "__restrict__ ",
                           "__restrict ", "struct ", "enum ", "_Nonnull ", "_Nullable ",
                           "register ", NULL};
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; strip[i]; i++) {
            char *found;
            while ((found = strstr(buf, strip[i])) != NULL) {
                memmove(found, found + strlen(strip[i]),
                        strlen(found + strlen(strip[i])) + 1);
            }
        }
    }
    trim(buf);

    // Check for pointer (any form of *)
    if (strchr(buf, '*')) return "*byte";

    // Check for function pointer (contains '(')
    if (strchr(buf, '(')) return "*byte";

    // void
    if (strcmp(buf, "void") == 0) return "";

    // Bool
    if (strcmp(buf, "_Bool") == 0 || strcmp(buf, "bool") == 0) return "bool";

    // Exact stdint matches (check before generic int/long)
    if (strcmp(buf, "uint8_t") == 0) return "uint8";
    if (strcmp(buf, "uint16_t") == 0) return "uint16";
    if (strcmp(buf, "uint32_t") == 0) return "uint32";
    if (strcmp(buf, "uint64_t") == 0) return "uint64";
    if (strcmp(buf, "int8_t") == 0) return "int8";
    if (strcmp(buf, "int16_t") == 0) return "int16";
    if (strcmp(buf, "int32_t") == 0) return "int32";
    if (strcmp(buf, "int64_t") == 0) return "int";
    if (strcmp(buf, "size_t") == 0) return "int";
    if (strcmp(buf, "ssize_t") == 0) return "int";
    if (strcmp(buf, "uintptr_t") == 0) return "int";
    if (strcmp(buf, "intptr_t") == 0) return "int";
    if (strcmp(buf, "ptrdiff_t") == 0) return "int";

    // Unsigned types
    if (strcmp(buf, "unsigned char") == 0) return "uint8";
    if (strcmp(buf, "unsigned short") == 0 ||
        strcmp(buf, "unsigned short int") == 0) return "uint16";
    if (strcmp(buf, "unsigned int") == 0 ||
        strcmp(buf, "unsigned") == 0) return "uint32";
    if (strcmp(buf, "unsigned long") == 0) return "uint64";
    if (strcmp(buf, "unsigned long long") == 0 ||
        strcmp(buf, "unsigned long long int") == 0) return "uint64";

    // Signed types
    if (strcmp(buf, "signed char") == 0) return "int8";
    if (strcmp(buf, "short") == 0 ||
        strcmp(buf, "short int") == 0 ||
        strcmp(buf, "signed short") == 0) return "int16";
    if (strcmp(buf, "long long") == 0 ||
        strcmp(buf, "long long int") == 0 ||
        strcmp(buf, "signed long long") == 0) return "int";
    if (strcmp(buf, "long") == 0 ||
        strcmp(buf, "long int") == 0 ||
        strcmp(buf, "signed long") == 0) return "int";
    if (strcmp(buf, "int") == 0 ||
        strcmp(buf, "signed int") == 0 ||
        strcmp(buf, "signed") == 0) return "int32";

    // Float types
    if (strcmp(buf, "float") == 0) return "float32";
    if (strcmp(buf, "double") == 0) return "float";
    if (strcmp(buf, "long double") == 0) return "float"; // approximate

    // char (not pointer)
    if (strcmp(buf, "char") == 0) return "byte";

    // Check if this is a known struct/typedef that we parsed as a shape
    for (int i = 0; i < shape_count; i++) {
        if (strcmp(shapes[i].name, buf) == 0) return shapes[i].name;
    }

    // Check typedef alias table (e.g. Uint32 → uint32_t → uint32)
    for (int i = 0; i < typedef_count; i++) {
        if (strcmp(typedefs[i].alias, buf) == 0) {
            return c_type_to_mix(typedefs[i].target);
        }
    }

    // Check if this is a known enum type name
    for (int i = 0; i < enum_name_count; i++) {
        if (strcmp(enum_names[i], buf) == 0) return "int32";
    }

    // Unknown type — treat as opaque pointer (*byte)
    return "*byte";
}

// ---- Strip __attribute__ ----

static void strip_attributes(char *s) {
    char *p;
    while ((p = strstr(s, "__attribute__")) != NULL) {
        // Find the matching )) — attributes use ((...))
        char *end = p + strlen("__attribute__");
        int depth = 0;
        while (*end) {
            if (*end == '(') depth++;
            else if (*end == ')') {
                depth--;
                if (depth <= 0) { end++; break; }
            }
            end++;
        }
        memmove(p, end, strlen(end) + 1);
    }
    // Also strip __asm__(...) and __asm(...)
    while ((p = strstr(s, "__asm__")) != NULL || (p = strstr(s, "__asm")) != NULL) {
        char *end = strchr(p, ')');
        if (end) {
            end++;
            memmove(p, end, strlen(end) + 1);
        } else {
            *p = '\0';
            break;
        }
    }
}

// ---- Preprocessor ----

static char *run_command(const char *cmd, bool verbose) {
    if (verbose) fprintf(stderr, "mix: %s\n", cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char *buf = malloc(MAX_BUF);
    if (!buf) { pclose(fp); return NULL; }
    size_t total = 0;
    size_t n;
    while ((n = fread(buf + total, 1, MAX_BUF - total - 1, fp)) > 0) {
        total += n;
        if (total >= MAX_BUF - 1) break;
    }
    buf[total] = '\0';
    pclose(fp);
    return buf;
}

// Resolve a header path by searching the source directory, CPPFLAGS -I
// directories, and vendor dirs.
// source_dir: directory of the .mix file (NULL to skip), tries <dir>/<path> first.
// If no source_dir and path exists as-is, returns a copy. Otherwise tries each
// -I<dir>/<path>, then searches lib/vendor/*/include/<path>.
// Returns a malloc'd string or NULL if not found.
char *resolve_header_path(const char *path, const char *source_dir) {
    struct stat st;

    // Try relative to source directory first (handles use c "local.h" in a .mix file
    // whose directory differs from the LSP/compiler CWD).
    if (source_dir) {
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", source_dir, path);
        if (stat(full, &st) == 0) return strdup(full);
    }

    // If file exists directly, use it as-is
    if (stat(path, &st) == 0) return strdup(path);

    // Parse -I flags from CPPFLAGS
    const char *cppflags = getenv("CPPFLAGS");
    if (cppflags && cppflags[0]) {
        char *buf = strdup(cppflags);
        char *tok = strtok(buf, " \t");
        while (tok) {
            const char *dir = NULL;
            if (strncmp(tok, "-I", 2) == 0 && tok[2] != '\0') {
                dir = tok + 2;
            } else if (strcmp(tok, "-I") == 0) {
                tok = strtok(NULL, " \t");
                if (tok) dir = tok;
            }
            if (dir) {
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", dir, path);
                if (stat(full, &st) == 0) {
                    free(buf);
                    return strdup(full);
                }
            }
            tok = strtok(NULL, " \t");
        }
        free(buf);
    }

    // Search lib/vendor/*/include/<path> relative to CWD
    {
        DIR *vdir = opendir("lib/vendor");
        if (vdir) {
            struct dirent *entry;
            while ((entry = readdir(vdir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                char full[2048];
                snprintf(full, sizeof(full), "lib/vendor/%s/include/%s",
                         entry->d_name, path);
                if (stat(full, &st) == 0) {
                    closedir(vdir);
                    return strdup(full);
                }
            }
            closedir(vdir);
        }
    }

    return NULL;
}

// Build -I flags for all vendor include directories found in lib/vendor/*/include/.
// Writes into buf (space-separated, e.g. "-Ilib/vendor/glad/include").
static void build_vendor_iflags(char *buf, size_t buf_size) {
    buf[0] = '\0';
    DIR *vdir = opendir("lib/vendor");
    if (!vdir) return;
    struct dirent *entry;
    while ((entry = readdir(vdir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char inc_dir[512];
        snprintf(inc_dir, sizeof(inc_dir), "lib/vendor/%s/include", entry->d_name);
        struct stat st;
        if (stat(inc_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            char flag[540];
            snprintf(flag, sizeof(flag), " -I\"%s\"", inc_dir);
            if (strlen(buf) + strlen(flag) < buf_size - 1) {
                strcat(buf, flag);
            }
        }
    }
    closedir(vdir);
}

static char *filter_user_decls(char *raw);

static char *preprocess_header(const char *path, bool verbose) {
    // Derive parent include dir (e.g., /opt/homebrew/include from
    // /opt/homebrew/include/SDL3/SDL.h)
    char include_dir[1024] = "";
    const char *slash = strrchr(path, '/');
    if (slash && slash > path) {
        const char *prev_slash = slash - 1;
        while (prev_slash > path && *prev_slash != '/') prev_slash--;
        if (*prev_slash == '/') {
            int len = (int)(prev_slash - path);
            if (len > 0 && len < (int)sizeof(include_dir)) {
                memcpy(include_dir, path, len);
                include_dir[len] = '\0';
            }
        }
    }

    char cmd[4096];
    const char *cppflags = getenv("CPPFLAGS");
    if (!cppflags) cppflags = "";
    char vendor_flags[2048];
    build_vendor_iflags(vendor_flags, sizeof(vendor_flags));
    // -DGL_GLEXT_PROTOTYPES exposes GL 2.0+ function prototypes in glext.h
    const char *gl_ext = (strstr(path, "opengl") || strstr(path, "OpenGL") ||
                          strstr(path, "glext")) ? "-DGL_GLEXT_PROTOTYPES " : "";
    // No -P: keep `# linenum "file"` markers so we can drop declarations
    // that came from system headers (libc, libc++, SDK).
    if (include_dir[0])
        snprintf(cmd, sizeof(cmd), "cc -E %s%s%s -I\"%s\" -x c \"%s\" 2>/dev/null",
                 gl_ext, cppflags, vendor_flags, include_dir, path);
    else
        snprintf(cmd, sizeof(cmd), "cc -E %s%s%s -x c \"%s\" 2>/dev/null",
                 gl_ext, cppflags, vendor_flags, path);
    char *raw = run_command(cmd, verbose);
    if (!raw) return NULL;
    return filter_user_decls(raw);
}

// Predicate for "this path is a system header we should drop". The
// preprocessor inlines headers transitively from <stdio.h>, <string.h>,
// etc.; redeclaring those in the generated C output collides with the real
// libc declarations.
static bool is_system_header_path(const char *path) {
    if (!path || !path[0]) return true;
    // cc's synthetic locations
    if (path[0] == '<') return true;
    static const char *sys_prefixes[] = {
        "/usr/include/", "/usr/lib/", "/usr/local/include/",
        "/Library/Developer/", "/Applications/Xcode.app/",
        "/opt/homebrew/include/c++/", "/opt/homebrew/Cellar/",
        "/opt/homebrew/Library/", "/System/Library/",
        NULL
    };
    for (int i = 0; sys_prefixes[i]; i++) {
        size_t plen = strlen(sys_prefixes[i]);
        if (strncmp(path, sys_prefixes[i], plen) == 0) return true;
    }
    return false;
}

// Walk the preprocessor output, drop everything that came from a system
// header (per `# linenum "file" ...` markers). Returns a freshly malloc'd
// string the caller must free; the original is freed here.
static char *filter_user_decls(char *raw) {
    if (!raw) return NULL;
    size_t cap = strlen(raw) + 1;
    char *out = malloc(cap);
    if (!out) return raw;
    size_t out_len = 0;
    bool keep = true;  // start in "keep" state until first marker says otherwise
    char *line = raw;
    while (*line) {
        char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        // `# N "file" ...` — preprocessor line marker
        if (len > 3 && line[0] == '#' && line[1] == ' ' &&
            (line[2] >= '0' && line[2] <= '9')) {
            const char *q1 = memchr(line, '"', len);
            if (q1) {
                const char *q2 = memchr(q1 + 1, '"', len - (q1 + 1 - line));
                if (q2) {
                    char path[1024];
                    size_t plen = (size_t)(q2 - q1 - 1);
                    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
                    memcpy(path, q1 + 1, plen);
                    path[plen] = '\0';
                    keep = !is_system_header_path(path);
                }
            }
        } else if (keep) {
            memcpy(out + out_len, line, len);
            out_len += len;
            out[out_len++] = '\n';
        }
        if (!eol) break;
        line = eol + 1;
    }
    out[out_len] = '\0';
    free(raw);
    return out;
}

// ---- Extract #define constants ----

static int extract_defines(const char *path, bool verbose) {
    char cmd[4096];
    const char *cppflags = getenv("CPPFLAGS");
    if (!cppflags) cppflags = "";
    char vendor_flags[2048];
    build_vendor_iflags(vendor_flags, sizeof(vendor_flags));
    snprintf(cmd, sizeof(cmd), "cc -E -dM %s%s -x c \"%s\" 2>/dev/null", cppflags, vendor_flags, path);
    char *text = run_command(cmd, verbose);
    if (!text) return 0;

    int added = 0;
    char *line = strtok(text, "\n");
    while (line) {
        // Format: #define NAME VALUE
        if (!starts_with(line, "#define ")) { line = strtok(NULL, "\n"); continue; }

        char *p = line + 8; // skip "#define "
        // Get name
        char name[256];
        int ni = 0;
        while (*p && is_ident_char(*p) && ni < 255) name[ni++] = *p++;
        name[ni] = '\0';

        // Skip function-like macros (name immediately followed by '(')
        if (*p == '(') { line = strtok(NULL, "\n"); continue; }

        // Skip internal/compiler macros (start with _ or __)
        if (name[0] == '_') { line = strtok(NULL, "\n"); continue; }

        // Skip MIX reserved words (true, false, none, type, etc.)
        if (is_reserved_word(name)) { line = strtok(NULL, "\n"); continue; }

        // Skip well-known system constant prefixes
        if (starts_with(name, "INT") || starts_with(name, "UINT") ||
            starts_with(name, "TARGET_") || starts_with(name, "INTPTR") ||
            starts_with(name, "UINTPTR") || starts_with(name, "WCHAR_") ||
            starts_with(name, "WINT_") || starts_with(name, "SIG_") ||
            starts_with(name, "FLT_") || starts_with(name, "DBL_") ||
            starts_with(name, "LDBL_") || starts_with(name, "CHAR_") ||
            starts_with(name, "SCHAR_") || starts_with(name, "SHRT_") ||
            starts_with(name, "LONG_") || starts_with(name, "LLONG_") ||
            starts_with(name, "MB_") || starts_with(name, "PTRDIFF_") ||
            starts_with(name, "SIZE_") || starts_with(name, "SSIZE_")) {
            line = strtok(NULL, "\n"); continue;
        }

        // Get value
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') { line = strtok(NULL, "\n"); continue; }

        // Strip integer constant wrapper macros: SDL_UINT64_C(val), UINT64_C(val), etc.
        char val_buf[256];
        strncpy(val_buf, p, sizeof(val_buf) - 1);
        val_buf[sizeof(val_buf) - 1] = '\0';
        {
            char *wp = val_buf;
            // Match patterns like XXX_C(value) or XXXX(value)
            char *paren = strchr(wp, '(');
            if (paren && (strstr(wp, "_C(") || strstr(wp, "INT64_C(") || strstr(wp, "INT32_C("))) {
                // Extract the value inside parentheses
                char *inner = paren + 1;
                char *close = strrchr(inner, ')');
                if (close) {
                    *close = '\0';
                    memmove(wp, inner, strlen(inner) + 1);
                }
            }
        }
        p = val_buf;

        // Handle bit-shift expressions: (1u << N), (1 << N)
        {
            char *shift = strstr(val_buf, "<<");
            if (shift) {
                // Extract base and shift amount
                char *bp = val_buf;
                while (*bp && (*bp == '(' || isspace((unsigned char)*bp))) bp++;
                char *endp2;
                long long base = strtoll(bp, &endp2, 0);
                // Skip trailing u/U/l/L
                while (*endp2 == 'u' || *endp2 == 'U' || *endp2 == 'l' || *endp2 == 'L') endp2++;
                if (endp2 <= shift) {
                    char *sp = shift + 2;
                    while (*sp && isspace((unsigned char)*sp)) sp++;
                    long long amount = strtoll(sp, &endp2, 0);
                    if (endp2 != sp) {
                        long long result = base << amount;
                        snprintf(val_buf, sizeof(val_buf), "%lld", result);
                    }
                }
            }
        }
        p = val_buf;

        // Try integer parse
        char *endp;
        bool hex_prefix = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
        long long ival = strtoll(p, &endp, 0);
        if (endp != p && (*endp == '\0' || isspace((unsigned char)*endp) ||
                          *endp == 'L' || *endp == 'U' || *endp == 'l' || *endp == 'u')) {
            // Check for duplicate
            bool dup = false;
            for (int i = 0; i < const_count; i++) {
                if (strcmp(consts[i].name, name) == 0) { dup = true; break; }
            }
            if (!dup && const_count < MAX_CONSTS) {
                strncpy(consts[const_count].name, name, 255);
                consts[const_count].int_value = ival;
                consts[const_count].is_float = false;
                consts[const_count].is_hex = hex_prefix;
                const_count++;
                added++;
            }
        } else {
            // Try float parse
            double fval = strtod(p, &endp);
            if (endp != p && (*endp == '\0' || isspace((unsigned char)*endp) ||
                              *endp == 'f' || *endp == 'F')) {
                bool dup = false;
                for (int i = 0; i < const_count; i++) {
                    if (strcmp(consts[i].name, name) == 0) { dup = true; break; }
                }
                if (!dup && const_count < MAX_CONSTS) {
                    strncpy(consts[const_count].name, name, 255);
                    consts[const_count].float_value = fval;
                    consts[const_count].is_float = true;
                    const_count++;
                    added++;
                }
            }
            // Try CLITERAL(TypeName){ val, val, ... } or (TypeName){ val, ... }
            // This handles Raylib-style struct literal macros like color constants
            {
                const char *cp = p;
                // Skip CLITERAL prefix if present
                if (strncmp(cp, "CLITERAL(", 9) == 0) {
                    cp += 9;
                } else if (*cp == '(' && is_ident_char(cp[1])) {
                    cp++;
                } else {
                    goto skip_shape_lit;
                }

                // Extract type name
                char stype[128];
                int si = 0;
                while (*cp && is_ident_char(*cp) && si < 127) stype[si++] = *cp++;
                stype[si] = '\0';
                if (*cp != ')') goto skip_shape_lit;
                cp++; // skip ')'

                // Find matching shape
                CShape *shape = NULL;
                for (int i = 0; i < shape_count; i++) {
                    if (strcmp(shapes[i].name, stype) == 0) {
                        shape = &shapes[i];
                        break;
                    }
                }
                if (!shape || shape->field_name_count == 0) goto skip_shape_lit;

                // Skip whitespace and expect '{'
                while (*cp && isspace((unsigned char)*cp)) cp++;
                if (*cp != '{') goto skip_shape_lit;
                cp++; // skip '{'

                // Parse comma-separated integer values
                long long vals[64];
                int val_count = 0;
                while (*cp && *cp != '}' && val_count < 64) {
                    while (*cp && isspace((unsigned char)*cp)) cp++;
                    if (*cp == '}') break;
                    char *vend;
                    long long v = strtoll(cp, &vend, 0);
                    if (vend == cp) goto skip_shape_lit;
                    vals[val_count++] = v;
                    cp = vend;
                    // Skip suffixes and whitespace
                    while (*cp == 'U' || *cp == 'L' || *cp == 'u' || *cp == 'l' || *cp == 'f' || *cp == 'F') cp++;
                    while (*cp && isspace((unsigned char)*cp)) cp++;
                    if (*cp == ',') cp++;
                }

                if (val_count > 0 && val_count <= shape->field_name_count) {
                    // Build "field1: val1, field2: val2, ..." string
                    char shape_vals[512] = "";
                    int svoff = 0;
                    for (int i = 0; i < val_count; i++) {
                        if (i > 0) svoff += snprintf(shape_vals + svoff, sizeof(shape_vals) - svoff, ", ");
                        svoff += snprintf(shape_vals + svoff, sizeof(shape_vals) - svoff,
                                          "%s: %lld", shape->field_names[i], vals[i]);
                    }
                    // Check for duplicate
                    bool dup = false;
                    for (int i = 0; i < const_count; i++) {
                        if (strcmp(consts[i].name, name) == 0) { dup = true; break; }
                    }
                    if (!dup && const_count < MAX_CONSTS) {
                        strncpy(consts[const_count].name, name, 255);
                        consts[const_count].is_shape_lit = true;
                        strncpy(consts[const_count].shape_name, stype, 127);
                        strncpy(consts[const_count].shape_values, shape_vals,
                                sizeof(consts[const_count].shape_values) - 1);
                        const_count++;
                        added++;
                    }
                }
                skip_shape_lit: ;
            }
        }
        line = strtok(NULL, "\n");
    }

    free(text);
    return added;
}

// ---- Parse function prototypes ----

// Find the last identifier before the first '(' — this is the function name
static bool extract_func_name(const char *decl, char *name_out, int *name_pos) {
    const char *paren = strchr(decl, '(');
    if (!paren) return false;

    // Walk backwards from '(' to find the function name
    const char *end = paren - 1;
    while (end > decl && isspace((unsigned char)*end)) end--;
    if (end < decl || !is_ident_char(*end)) return false;

    const char *start = end;
    while (start > decl && is_ident_char(*(start - 1))) start--;

    int len = (int)(end - start + 1);
    if (len <= 0 || len > 255) return false;
    memcpy(name_out, start, len);
    name_out[len] = '\0';
    *name_pos = (int)(start - decl);
    return true;
}

// Parse a single parameter: "int x" or "const char *name" or "..."
static bool parse_one_param(const char *param_str, char *out, int idx) {
    char buf[512];
    strncpy(buf, param_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);

    if (strlen(buf) == 0) return false;
    if (strcmp(buf, "void") == 0) return false; // void param = no params
    if (strcmp(buf, "...") == 0) return false;   // handled separately

    // Find the parameter name: last identifier token
    // The type is everything before it
    int len = (int)strlen(buf);
    int name_end = len - 1;
    while (name_end >= 0 && isspace((unsigned char)buf[name_end])) name_end--;
    if (name_end < 0) return false;

    // Check for pointer at end of type (no separate name)
    // e.g., "char *" — type only, no name
    bool has_name = false;
    int name_start = name_end;
    if (is_ident_char(buf[name_end])) {
        while (name_start > 0 && is_ident_char(buf[name_start - 1])) name_start--;
        // Check if there's type info before the name
        int before = name_start - 1;
        while (before >= 0 && isspace((unsigned char)buf[before])) before--;
        if (before >= 0) has_name = true;
    }

    char pname[128];
    char ptype_str[512];

    if (has_name) {
        int nlen = name_end - name_start + 1;
        memcpy(pname, buf + name_start, nlen);
        pname[nlen] = '\0';
        memcpy(ptype_str, buf, name_start);
        ptype_str[name_start] = '\0';
    } else {
        // No name, generate synthetic
        snprintf(pname, sizeof(pname), "p%d", idx);
        strncpy(ptype_str, buf, sizeof(ptype_str) - 1);
        ptype_str[sizeof(ptype_str) - 1] = '\0';
    }
    trim(ptype_str);

    const char *mix_type = c_type_to_mix(ptype_str);
    if (!mix_type) return false; // unknown type

    // Rename reserved words by appending underscore
    if (is_reserved_word(pname)) {
        int pnlen = (int)strlen(pname);
        if (pnlen < 126) {
            pname[pnlen] = '_';
            pname[pnlen + 1] = '\0';
        }
    }

    snprintf(out, 256, "%s: %s", pname, mix_type);
    return true;
}

// ---- Parse simple typedefs into alias table ----
// Handles: typedef <known_type> <alias>;
// E.g.: typedef uint32_t Uint32;  →  Uint32 resolves to uint32

static void parse_typedefs(const char *text) {
    const char *p = text;
    while (*p) {
        const char *ts = strstr(p, "typedef ");
        if (!ts) break;
        p = ts + 8; // skip "typedef "

        // Skip struct/union/enum typedefs (handled separately)
        while (*p && isspace((unsigned char)*p)) p++;
        if (starts_with(p, "struct ") || starts_with(p, "union ") ||
            starts_with(p, "enum ")) {
            p++;
            continue;
        }

        // Collect everything until ';'
        char line[512];
        int li = 0;
        const char *lp = ts + 8;
        while (*lp && *lp != ';' && *lp != '{' && li < 510) line[li++] = *lp++;
        line[li] = '\0';
        if (*lp != ';') { p = lp; continue; }
        p = lp + 1;
        trim(line);

        // Must be exactly two tokens: "target_type alias_name"
        // Skip function pointers (contain parens)
        if (strchr(line, '(') || strchr(line, ')')) continue;
        // Skip pointer typedefs
        if (strchr(line, '*')) continue;

        // Find last identifier as alias name
        int len = (int)strlen(line);
        int ne = len - 1;
        while (ne >= 0 && isspace((unsigned char)line[ne])) ne--;
        if (ne < 0 || !is_ident_char(line[ne])) continue;
        int ns = ne;
        while (ns > 0 && is_ident_char(line[ns - 1])) ns--;

        char alias[128], target[128];
        int alen = ne - ns + 1;
        if (alen > 127) continue;
        memcpy(alias, line + ns, alen);
        alias[alen] = '\0';

        int tlen = ns;
        while (tlen > 0 && isspace((unsigned char)line[tlen - 1])) tlen--;
        if (tlen <= 0 || tlen > 127) continue;
        memcpy(target, line, tlen);
        target[tlen] = '\0';
        trim(target);

        // Only store if target is a known C type that c_type_to_mix can resolve
        const char *resolved = c_type_to_mix(target);
        if (!resolved || strcmp(resolved, "*byte") == 0) continue;

        // Don't store if alias is same as target
        if (strcmp(alias, target) == 0) continue;

        if (typedef_count < MAX_TYPEDEFS) {
            strncpy(typedefs[typedef_count].alias, alias, 127);
            strncpy(typedefs[typedef_count].target, target, 127);
            typedef_count++;
        }
    }
}

// ---- Parse struct/typedef definitions into shapes ----

static int parse_structs(const char *text) {
    int added = 0;
    const char *p = text;

    while (*p) {
        // Look for "typedef struct" pattern
        const char *ts = strstr(p, "typedef struct");
        if (!ts) break;
        p = ts + 14; // skip "typedef struct"

        // Skip whitespace
        while (*p && isspace((unsigned char)*p)) p++;

        // Optional struct tag name (skip it)
        if (is_ident_char(*p)) {
            while (*p && is_ident_char(*p)) p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }

        // Must have opening brace
        if (*p != '{') continue;
        p++; // skip {

        // Collect everything until matching }
        int depth = 1;
        const char *body_start = p;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            if (depth > 0) p++;
        }
        if (depth != 0) break;
        const char *body_end = p;
        p++; // skip }

        // Skip whitespace to find the typedef name
        while (*p && isspace((unsigned char)*p)) p++;
        if (!is_ident_char(*p)) continue;

        char typedef_name[128];
        int ni = 0;
        while (*p && is_ident_char(*p) && ni < 127) typedef_name[ni++] = *p++;
        typedef_name[ni] = '\0';

        // Skip if already recorded or if it starts with __
        if (typedef_name[0] == '_' && typedef_name[1] == '_') continue;
        if (shape_exists(typedef_name)) continue;

        // Parse fields from the body
        char fields[2048] = "";
        int flen = 0;
        const char *fp = body_start;
        int field_count = 0;
        char parsed_field_names[64][128];
        memset(parsed_field_names, 0, sizeof(parsed_field_names));

        while (fp < body_end) {
            // Skip whitespace
            while (fp < body_end && isspace((unsigned char)*fp)) fp++;
            if (fp >= body_end) break;

            // Skip nested structs/unions
            if (*fp == '{') {
                int d = 1;
                fp++;
                while (fp < body_end && d > 0) {
                    if (*fp == '{') d++;
                    else if (*fp == '}') d--;
                    fp++;
                }
                continue;
            }

            // Collect one field declaration until ';'
            char fdecl[512];
            int fi = 0;
            while (fp < body_end && *fp != ';' && fi < 510) {
                if (*fp == '{') break; // nested struct
                fdecl[fi++] = *fp++;
            }
            fdecl[fi] = '\0';
            if (fp < body_end && *fp == ';') fp++;
            trim(fdecl);
            if (strlen(fdecl) < 2) continue;

            // Strip qualifiers from the field declaration
            strip_attributes(fdecl);
            trim(fdecl);

            // Extract field name (last ident) and type (everything before)
            int dlen = (int)strlen(fdecl);
            int ne = dlen - 1;
            while (ne >= 0 && isspace((unsigned char)fdecl[ne])) ne--;
            if (ne < 0 || !is_ident_char(fdecl[ne])) continue;

            int ns = ne;
            while (ns > 0 && is_ident_char(fdecl[ns - 1])) ns--;

            // Check there's type info before the name
            int before = ns - 1;
            while (before >= 0 && isspace((unsigned char)fdecl[before])) before--;
            if (before < 0) continue;

            char fname[128];
            int fnlen = ne - ns + 1;
            memcpy(fname, fdecl + ns, fnlen);
            fname[fnlen] = '\0';

            char ftype_str[512];
            memcpy(ftype_str, fdecl, ns);
            ftype_str[ns] = '\0';
            trim(ftype_str);

            const char *mix_ft = c_type_to_mix(ftype_str);
            if (!mix_ft || strlen(mix_ft) == 0) continue;

            // Rename reserved words by appending underscore
            if (is_reserved_word(fname)) {
                int fl = (int)strlen(fname);
                if (fl < 126) { fname[fl] = '_'; fname[fl + 1] = '\0'; }
            }

            // Add to fields string
            if (field_count > 0) {
                flen += snprintf(fields + flen, sizeof(fields) - flen, "\n");
            }
            flen += snprintf(fields + flen, sizeof(fields) - flen,
                             "    %s: %s", fname, mix_ft);
            if (field_count < 64) {
                strncpy(parsed_field_names[field_count], fname, 127);
            }
            field_count++;
        }

        if (field_count > 0 && shape_count < MAX_SHAPES) {
            strncpy(shapes[shape_count].name, typedef_name, 127);
            strncpy(shapes[shape_count].fields, fields, sizeof(shapes[shape_count].fields) - 1);
            shapes[shape_count].field_name_count = field_count < 64 ? field_count : 64;
            memcpy(shapes[shape_count].field_names, parsed_field_names,
                   sizeof(parsed_field_names));
            shapes[shape_count].is_union = false;
            shape_count++;
            added++;
        }
    }

    return added;
}

// ---- Parse typedef union definitions ----
// Same approach as parse_structs but for union types.
static int parse_unions(const char *text) {
    int added = 0;
    const char *p = text;

    while (*p) {
        const char *ts = strstr(p, "typedef union");
        if (!ts) break;
        p = ts + 13; // skip "typedef union"

        while (*p && isspace((unsigned char)*p)) p++;

        // Optional union tag name (skip it)
        if (is_ident_char(*p)) {
            while (*p && is_ident_char(*p)) p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }

        if (*p != '{') continue;
        p++;

        // Collect body until matching }
        int depth = 1;
        const char *body_start = p;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            if (depth > 0) p++;
        }
        if (depth != 0) break;
        const char *body_end = p;
        p++;

        // Get typedef name
        while (*p && isspace((unsigned char)*p)) p++;
        if (!is_ident_char(*p)) continue;

        char typedef_name[128];
        int ni = 0;
        while (*p && is_ident_char(*p) && ni < 127) typedef_name[ni++] = *p++;
        typedef_name[ni] = '\0';

        if (typedef_name[0] == '_' && typedef_name[1] == '_') continue;
        if (shape_exists(typedef_name)) continue;

        // Parse fields from the body (same as structs)
        char fields[2048] = "";
        int flen = 0;
        const char *fp = body_start;
        int field_count = 0;
        char parsed_field_names[64][128];
        memset(parsed_field_names, 0, sizeof(parsed_field_names));

        while (fp < body_end) {
            while (fp < body_end && isspace((unsigned char)*fp)) fp++;
            if (fp >= body_end) break;

            // Skip nested structs/unions
            if (*fp == '{') {
                int d = 1;
                fp++;
                while (fp < body_end && d > 0) {
                    if (*fp == '{') d++;
                    else if (*fp == '}') d--;
                    fp++;
                }
                continue;
            }

            // Collect one field declaration until ';'
            char fdecl[512];
            int fi = 0;
            while (fp < body_end && *fp != ';' && fi < 510) {
                if (*fp == '{') break;
                fdecl[fi++] = *fp++;
            }
            fdecl[fi] = '\0';
            if (fp < body_end && *fp == ';') fp++;
            trim(fdecl);
            if (strlen(fdecl) < 2) continue;

            strip_attributes(fdecl);
            trim(fdecl);

            // Extract field name (last ident) and type (everything before)
            int dlen = (int)strlen(fdecl);
            int ne = dlen - 1;
            while (ne >= 0 && isspace((unsigned char)fdecl[ne])) ne--;
            if (ne < 0 || !is_ident_char(fdecl[ne])) continue;

            int ns = ne;
            while (ns > 0 && is_ident_char(fdecl[ns - 1])) ns--;

            // Check for array brackets before field name
            bool is_array = false;
            int bracket_end = ns - 1;
            while (bracket_end >= 0 && isspace((unsigned char)fdecl[bracket_end])) bracket_end--;
            if (bracket_end >= 0 && fdecl[bracket_end] == ']') is_array = true;

            char fname[128];
            int fnlen = ne - ns + 1;
            if (fnlen > 127) fnlen = 127;
            memcpy(fname, fdecl + ns, fnlen);
            fname[fnlen] = '\0';

            // Get C type
            char ctype[256];
            int ctlen = ns;
            while (ctlen > 0 && isspace((unsigned char)fdecl[ctlen - 1])) ctlen--;
            if (ctlen <= 0) continue;
            memcpy(ctype, fdecl, ctlen);
            ctype[ctlen] = '\0';
            trim(ctype);

            // Rename reserved words
            if (is_reserved_word(fname)) {
                int len = (int)strlen(fname);
                if (len < 126) { fname[len] = '_'; fname[len + 1] = '\0'; }
            }

            // Map to MIX type
            const char *mix_type;
            if (is_array) {
                mix_type = "*byte";
            } else {
                mix_type = c_type_to_mix(ctype);
                if (!mix_type) mix_type = "*byte";
            }

            int n = snprintf(fields + flen, sizeof(fields) - flen,
                            "    %s: %s\n", fname, mix_type);
            flen += n;

            if (field_count < 64) {
                strncpy(parsed_field_names[field_count], fname, 127);
                field_count++;
            }
        }

        if (flen > 0 && shape_count < MAX_SHAPES) {
            strncpy(shapes[shape_count].name, typedef_name, 127);
            strncpy(shapes[shape_count].fields, fields, sizeof(shapes[shape_count].fields) - 1);
            shapes[shape_count].field_name_count = field_count;
            for (int k = 0; k < field_count && k < 64; k++)
                strncpy(shapes[shape_count].field_names[k], parsed_field_names[k], 127);
            shapes[shape_count].is_union = true;
            shape_count++;
            added++;
        }
    }

    return added;
}

// ---- Parse enum definitions into constants ----

static int parse_enums(const char *text) {
    int added = 0;
    const char *p = text;

    while (*p) {
        // Look for "enum" followed by optional name and '{'
        // Match: "typedef enum {", "typedef enum Name {", "enum Name {"
        const char *e = strstr(p, "enum");
        if (!e) break;
        p = e + 4;

        // Skip whitespace
        while (*p && isspace((unsigned char)*p)) p++;

        // Optional tag name (skip it)
        if (is_ident_char(*p)) {
            while (*p && is_ident_char(*p)) p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }

        // Must have opening brace
        if (*p != '{') continue;
        p++; // skip {

        // Parse enumerators until }
        long long next_value = 0;
        while (*p && *p != '}') {
            // Skip whitespace and commas
            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
            if (*p == '}' || !*p) break;

            // Skip preprocessor lines
            if (*p == '#') {
                while (*p && *p != '\n') p++;
                continue;
            }

            // Read enumerator name
            if (!is_ident_char(*p)) { p++; continue; }
            char ename[256];
            int ei = 0;
            while (*p && is_ident_char(*p) && ei < 255) ename[ei++] = *p++;
            ename[ei] = '\0';

            // Skip internal names
            if (ename[0] == '_' && ename[1] == '_') {
                while (*p && *p != ',' && *p != '}') p++;
                continue;
            }

            // Skip whitespace
            while (*p && isspace((unsigned char)*p)) p++;

            long long val = next_value;
            bool hex = false;
            if (*p == '=') {
                p++; // skip =
                while (*p && isspace((unsigned char)*p)) p++;
                // Parse the value — could be hex or decimal
                hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
                char *endp;
                val = strtoll(p, &endp, 0);
                if (endp == p) {
                    // Skip unparseable value
                    while (*p && *p != ',' && *p != '}') p++;
                    continue;
                }
                p = endp;
                // Skip trailing type suffixes
                while (*p == 'U' || *p == 'L' || *p == 'u' || *p == 'l') p++;
            }

            // Skip MIX reserved words
            if (!is_reserved_word(ename)) {
                // Check for duplicate
                bool dup = false;
                for (int i = 0; i < const_count; i++) {
                    if (strcmp(consts[i].name, ename) == 0) { dup = true; break; }
                }
                if (!dup && const_count < MAX_CONSTS) {
                    strncpy(consts[const_count].name, ename, 255);
                    consts[const_count].int_value = val;
                    consts[const_count].is_float = false;
                    consts[const_count].is_hex = hex;
                    const_count++;
                    added++;
                }
            }

            next_value = val + 1;

            // Skip to next comma or end
            while (*p && *p != ',' && *p != '}') p++;
        }

        if (*p == '}') {
            p++;
            // Capture typedef name: } TypeName ;
            const char *save_p = p;
            while (*p && isspace((unsigned char)*p)) p++;
            if (is_ident_char(*p)) {
                char tname[256];
                int ti = 0;
                while (*p && is_ident_char(*p) && ti < (int)sizeof(tname) - 1)
                    tname[ti++] = *p++;
                tname[ti] = '\0';
                if (ti > 0 && enum_name_count < MAX_ENUM_NAMES) {
                    strncpy(enum_names[enum_name_count], tname, 255);
                    enum_name_count++;
                }
            } else {
                p = save_p;
            }
        }
    }

    return added;
}

static int parse_prototypes(const char *text) {
    int added = 0;

    // Process by collecting semicolon-terminated statements
    const char *p = text;
    char stmt[4096];

    while (*p) {
        // Skip whitespace and newlines
        while (*p && (isspace((unsigned char)*p))) p++;
        if (!*p) break;

        // Skip braced blocks entirely (struct/union/function bodies)
        if (*p == '{') {
            int depth = 0;
            while (*p) {
                if (*p == '{') depth++;
                else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                p++;
            }
            continue;
        }

        // Collect until ';' or '{' or end
        int si = 0;
        while (*p && si < (int)sizeof(stmt) - 1) {
            if (*p == '{') break;   // stop before brace, outer loop will skip it
            if (*p == ';') { p++; break; }
            if (*p == '\n' || *p == '\r') { stmt[si++] = ' '; p++; continue; }
            stmt[si++] = *p++;
        }
        stmt[si] = '\0';

        trim(stmt);
        if (strlen(stmt) < 3) continue;

        // Strip __attribute__
        strip_attributes(stmt);
        trim(stmt);

        // Skip things that aren't function prototypes
        if (starts_with(stmt, "typedef ")) continue;
        if (starts_with(stmt, "static ")) continue;
        if (starts_with(stmt, "inline ")) continue;
        if (starts_with(stmt, "static inline ")) continue;
        if (starts_with(stmt, "__inline")) continue;
        if (starts_with(stmt, "#")) continue;
        if (strstr(stmt, "struct {") || strstr(stmt, "union {") || strstr(stmt, "enum {")) continue;

        // Must contain '(' to be a function prototype
        if (!strchr(stmt, '(')) continue;

        // Strip leading 'extern'
        char *s = stmt;
        if (starts_with(s, "extern ")) s += 7;
        trim(s);

        // Extract function name
        char fname[256];
        int name_pos;
        if (!extract_func_name(s, fname, &name_pos)) continue;

        // Skip reserved/internal names
        if (fname[0] == '_' && fname[1] == '_') continue;
        // Skip C compiler builtins/keywords that look like function prototypes
        if (starts_with(fname, "_Static_assert") ||
            starts_with(fname, "_Alignas") ||
            starts_with(fname, "_Alignof") ||
            starts_with(fname, "_Atomic") ||
            starts_with(fname, "_Generic") ||
            starts_with(fname, "_Noreturn") ||
            starts_with(fname, "_Thread_local") ||
            starts_with(fname, "_Bool") ||
            starts_with(fname, "_Complex") ||
            starts_with(fname, "_Imaginary")) continue;

        // Skip if already recorded
        if (func_exists(fname)) continue;

        // Extract return type (everything before the function name)
        char ret_str[512];
        memcpy(ret_str, s, name_pos);
        ret_str[name_pos] = '\0';
        trim(ret_str);

        const char *ret_mix = c_type_to_mix(ret_str);
        if (!ret_mix) continue; // unknown return type, skip

        // Extract parameter list
        const char *paren_open = strchr(s, '(');
        const char *paren_close = strrchr(s, ')');
        if (!paren_open || !paren_close || paren_close <= paren_open) continue;

        char params_raw[2048];
        int plen = (int)(paren_close - paren_open - 1);
        if (plen < 0 || plen >= (int)sizeof(params_raw)) continue;
        memcpy(params_raw, paren_open + 1, plen);
        params_raw[plen] = '\0';
        trim(params_raw);

        // Check for variadic
        bool is_variadic = (strstr(params_raw, "...") != NULL);

        // Parse individual parameters
        char params_mix[2048] = "";
        int param_idx = 0;
        bool skip_func = false;

        if (strlen(params_raw) > 0 && strcmp(params_raw, "void") != 0) {
            // Split on commas (respecting nested parens for function pointers)
            char *param_strs[64];
            int param_count = 0;
            char params_copy[2048];
            strncpy(params_copy, params_raw, sizeof(params_copy) - 1);
            params_copy[sizeof(params_copy) - 1] = '\0';

            char *tok_start = params_copy;
            int depth = 0;
            for (char *c = params_copy; ; c++) {
                if (*c == '(') depth++;
                else if (*c == ')') depth--;
                else if ((*c == ',' || *c == '\0') && depth == 0) {
                    bool is_end = (*c == '\0');
                    *c = '\0';
                    if (param_count < 64) {
                        param_strs[param_count++] = tok_start;
                    }
                    if (is_end) break;
                    tok_start = c + 1;
                }
            }

            for (int i = 0; i < param_count; i++) {
                char trimmed[512];
                strncpy(trimmed, param_strs[i], sizeof(trimmed) - 1);
                trimmed[sizeof(trimmed) - 1] = '\0';
                trim(trimmed);

                if (strcmp(trimmed, "...") == 0) continue;
                if (strcmp(trimmed, "void") == 0) continue;

                char one_param[256];
                if (!parse_one_param(trimmed, one_param, param_idx)) {
                    skip_func = true;
                    break;
                }
                if (param_idx > 0) {
                    strncat(params_mix, ", ", sizeof(params_mix) - strlen(params_mix) - 1);
                }
                strncat(params_mix, one_param, sizeof(params_mix) - strlen(params_mix) - 1);
                param_idx++;
            }
        }

        if (skip_func) continue;
        if (func_count >= MAX_FUNCS) break;

        strncpy(funcs[func_count].name, fname, 255);
        funcs[func_count].c_name[0] = '\0';
        strncpy(funcs[func_count].ret_mix, ret_mix, 63);
        strncpy(funcs[func_count].params_mix, params_mix, sizeof(funcs[func_count].params_mix) - 1);
        funcs[func_count].is_variadic = is_variadic;
        func_count++;
        added++;
    }

    return added;
}

// ---- Function pointer typedef/global parsing (for GLAD-style headers) ----

// Stores parsed function pointer typedefs: e.g. PFNGLCLEARPROC -> {ret: "", params: "mask: uint32"}
#define MAX_FP_TYPEDEFS 4096
static struct {
    char typedef_name[128];    // e.g. "PFNGLCLEARPROC"
    char ret_mix[64];          // MIX return type
    char params_mix[2048];     // MIX parameter list
} fp_typedefs[MAX_FP_TYPEDEFS];
static int fp_typedef_count = 0;

// Parse function pointer typedefs from preprocessed output:
// typedef void (* PFNGLCLEARPROC)(GLbitfield mask);
// typedef void (APIENTRYP PFNGLCLEARPROC)(GLbitfield mask);
// After preprocessing, APIENTRYP expands to *, so pattern is:
// typedef <ret> (* <NAME>)(<params>);
static void parse_func_ptr_typedefs(const char *text) {
    const char *p = text;
    while (*p) {
        // Find "typedef "
        const char *tdef = strstr(p, "typedef ");
        if (!tdef) break;
        p = tdef + 8; // skip "typedef "

        // Skip whitespace
        while (*p && isspace((unsigned char)*p)) p++;

        // Collect return type up to '('
        char ret_str[256];
        int ri = 0;
        while (*p && *p != '(' && ri < (int)sizeof(ret_str) - 1) {
            ret_str[ri++] = *p++;
        }
        ret_str[ri] = '\0';
        if (*p != '(') continue;
        p++; // skip '('

        // Inside parens: expect "* NAME" or just "* NAME" (APIENTRYP expanded to *)
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '*') p++;
        else { continue; } // not a function pointer typedef
        while (*p && isspace((unsigned char)*p)) p++;

        // Collect typedef name (e.g. PFNGLCLEARPROC)
        char tname[128];
        int ti = 0;
        while (*p && is_ident_char(*p) && ti < (int)sizeof(tname) - 1) {
            tname[ti++] = *p++;
        }
        tname[ti] = '\0';
        if (ti == 0) continue;

        // Must be followed by )( for parameter list
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != ')') continue;
        p++;
        if (*p != '(') continue;
        p++;

        // Collect parameter list until matching ')'
        char params_raw[2048];
        int pi2 = 0;
        int depth = 1;
        while (*p && depth > 0 && pi2 < (int)sizeof(params_raw) - 1) {
            if (*p == '(') depth++;
            else if (*p == ')') { depth--; if (depth == 0) break; }
            params_raw[pi2++] = *p++;
        }
        params_raw[pi2] = '\0';
        if (*p == ')') p++;

        // Only care about PFN* typedefs (GLAD pattern)
        if (!starts_with(tname, "PFN")) continue;

        // Map return type
        trim(ret_str);
        const char *ret_mix = c_type_to_mix(ret_str);
        if (!ret_mix) continue;

        // Parse parameters
        trim(params_raw);
        char params_mix[2048] = "";
        int param_idx = 0;
        bool skip_func = false;

        if (strlen(params_raw) > 0 && strcmp(params_raw, "void") != 0) {
            // Split on commas
            char params_copy[2048];
            strncpy(params_copy, params_raw, sizeof(params_copy) - 1);
            params_copy[sizeof(params_copy) - 1] = '\0';

            char *param_strs[64];
            int param_count = 0;
            char *ps = params_copy;
            int pdepth = 0;
            char *start = ps;
            while (*ps) {
                if (*ps == '(' || *ps == '<') pdepth++;
                else if (*ps == ')' || *ps == '>') pdepth--;
                else if (*ps == ',' && pdepth == 0) {
                    *ps = '\0';
                    if (param_count < 64) param_strs[param_count++] = start;
                    start = ps + 1;
                }
                ps++;
            }
            if (param_count < 64) param_strs[param_count++] = start;

            for (int i = 0; i < param_count && !skip_func; i++) {
                char param[512];
                strncpy(param, param_strs[i], sizeof(param) - 1);
                param[sizeof(param) - 1] = '\0';
                trim(param);
                strip_attributes(param);
                trim(param);

                if (strcmp(param, "...") == 0) continue;
                if (strcmp(param, "void") == 0) continue;

                // Extract param name (last identifier)
                char pname[128] = "";
                char ptype[512];
                int len = (int)strlen(param);
                int end = len - 1;
                while (end >= 0 && isspace((unsigned char)param[end])) end--;
                int name_end = end;
                while (end >= 0 && is_ident_char(param[end])) end--;
                int name_start = end + 1;
                if (name_start <= name_end) {
                    int nlen = name_end - name_start + 1;
                    memcpy(pname, param + name_start, nlen);
                    pname[nlen] = '\0';
                    memcpy(ptype, param, name_start);
                    ptype[name_start] = '\0';
                } else {
                    strncpy(ptype, param, sizeof(ptype) - 1);
                    snprintf(pname, sizeof(pname), "p%d", i);
                }
                trim(ptype);

                const char *pmix = c_type_to_mix(ptype);
                if (!pmix) { skip_func = true; continue; }

                // Make name safe
                if (is_reserved_word(pname)) {
                    char safe[140];
                    snprintf(safe, sizeof(safe), "%s_", pname);
                    strncpy(pname, safe, sizeof(pname) - 1);
                }

                if (param_idx > 0) strcat(params_mix, ", ");
                char entry[256];
                snprintf(entry, sizeof(entry), "%s: %s", pname, pmix);
                strcat(params_mix, entry);
                param_idx++;
            }
        }

        if (skip_func) continue;
        if (fp_typedef_count >= MAX_FP_TYPEDEFS) continue;

        strncpy(fp_typedefs[fp_typedef_count].typedef_name, tname,
                sizeof(fp_typedefs[0].typedef_name) - 1);
        strncpy(fp_typedefs[fp_typedef_count].ret_mix, ret_mix,
                sizeof(fp_typedefs[0].ret_mix) - 1);
        strncpy(fp_typedefs[fp_typedef_count].params_mix, params_mix,
                sizeof(fp_typedefs[0].params_mix) - 1);
        fp_typedef_count++;
    }
}

// Parse function pointer global declarations and add to funcs[]:
// GLAPI PFNGLCLEARPROC glad_glClear;
// After preprocessing: extern PFNGLCLEARPROC glad_glClear;
// Transform name: glad_glClear -> gl_Clear (strip "glad_gl", add "gl_")
static void parse_func_ptr_globals(const char *text) {
    const char *p = text;
    while (*p) {
        // Look for lines containing "glad_gl"
        const char *line_start = p;
        // Find end of line
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);

        char line[2048];
        int llen = (int)(eol - line_start);
        if (llen >= (int)sizeof(line)) llen = (int)sizeof(line) - 1;
        memcpy(line, line_start, llen);
        line[llen] = '\0';
        p = (*eol) ? eol + 1 : eol;

        // Look for pattern: [extern] PFNXXX glad_glYYY ;
        char *glad_pos = strstr(line, "glad_gl");
        if (!glad_pos) continue;

        // Extract the variable name
        char varname[256];
        int vi = 0;
        char *vp = glad_pos;
        while (*vp && is_ident_char(*vp) && vi < (int)sizeof(varname) - 1) {
            varname[vi++] = *vp++;
        }
        varname[vi] = '\0';

        // Find the typedef name (word before glad_gl)
        char *before = glad_pos - 1;
        while (before > line && isspace((unsigned char)*before)) before--;
        char tname[128];
        char *te = before;
        while (te >= line && is_ident_char(*te)) te--;
        te++;
        int tlen = (int)(before - te + 1);
        if (tlen > 0 && tlen < (int)sizeof(tname)) {
            memcpy(tname, te, tlen);
            tname[tlen] = '\0';
        } else {
            continue;
        }

        // Look up typedef
        int fp_idx = -1;
        for (int i = 0; i < fp_typedef_count; i++) {
            if (strcmp(fp_typedefs[i].typedef_name, tname) == 0) {
                fp_idx = i;
                break;
            }
        }
        if (fp_idx < 0) continue;

        // Transform name: glad_glClear -> gl_Clear
        // Strip "glad_gl" prefix, add "gl_"
        char mix_name[256];
        const char *after_prefix = varname + 7; // skip "glad_gl"
        snprintf(mix_name, sizeof(mix_name), "gl_%s", after_prefix);

        // Skip if already recorded
        if (func_exists(mix_name)) continue;
        if (func_count >= MAX_FUNCS) continue;

        strncpy(funcs[func_count].name, mix_name, sizeof(funcs[0].name) - 1);
        strncpy(funcs[func_count].c_name, varname, sizeof(funcs[0].c_name) - 1);
        strncpy(funcs[func_count].ret_mix, fp_typedefs[fp_idx].ret_mix,
                sizeof(funcs[0].ret_mix) - 1);
        strncpy(funcs[func_count].params_mix, fp_typedefs[fp_idx].params_mix,
                sizeof(funcs[0].params_mix) - 1);
        funcs[func_count].is_variadic = false;
        func_count++;
    }
}

// Names provided by the C standard headers we already #include in the
// generated C output (string.h, stdio.h, stdlib.h, math.h, ctype.h). Skip
// them so cbind doesn't redeclare them with conflicting prototypes when a
// user header transitively pulls in the system headers.
static bool is_libc_name(const char *name) {
    static const char *libc_names[] = {
        // <string.h>
        "memcpy","memmove","memset","memchr","memcmp",
        "strlen","strcpy","strncpy","strcat","strncat","strcmp","strncmp",
        "strchr","strrchr","strstr","strdup","strndup","strerror","strtok","strtok_r",
        "strspn","strcspn","strpbrk","strcoll","strxfrm","strsignal","strnlen",
        "bcopy","bzero","bcmp","ffs","ffsl","ffsll","fls","flsl","flsll",
        "index","rindex","memccpy","stpcpy","stpncpy","memmem",
        "memset_pattern4","memset_pattern8","memset_pattern16",
        "strcasestr","strchrnul","strnstr","strlcat","strlcpy","strmode","strsep",
        "swab","timingsafe_bcmp","strcasecmp","strncasecmp",
        // <stdio.h>
        "printf","fprintf","sprintf","snprintf","vprintf","vfprintf","vsprintf","vsnprintf",
        "scanf","fscanf","sscanf","vscanf","vfscanf","vsscanf",
        "fopen","freopen","fclose","fread","fwrite","fseek","ftell","fseeko","ftello",
        "fflush","fputs","fgets","fgetc","fputc","getc","putc","ungetc",
        "getchar","putchar","puts","gets","rewind","feof","ferror","clearerr",
        "fileno","setbuf","setvbuf","perror","remove","rename","tmpfile","tmpnam",
        "fdopen","popen","pclose","getline","getdelim",
        "ctermid","flockfile","ftrylockfile","funlockfile","getc_unlocked",
        "putc_unlocked","getchar_unlocked","putchar_unlocked",
        "fgetpos","fsetpos","renameat",
        // <stdlib.h>
        "malloc","calloc","realloc","free","aligned_alloc","posix_memalign",
        "atoi","atol","atoll","atof","strtol","strtoll","strtoul","strtoull",
        "strtod","strtof","strtold",
        "qsort","bsearch","abs","labs","llabs","div","ldiv","lldiv",
        "rand","srand","rand_r","random","srandom",
        "exit","_Exit","abort","atexit","at_quick_exit","quick_exit",
        "system","getenv","setenv","unsetenv","putenv","clearenv",
        "mblen","mbtowc","wctomb","mbstowcs","wcstombs",
        // <math.h>
        "sqrt","sqrtf","sqrtl","cbrt","cbrtf",
        "sin","sinf","cos","cosf","tan","tanf",
        "asin","asinf","acos","acosf","atan","atanf","atan2","atan2f",
        "sinh","coshf","tanh","asinh","acosh","atanh",
        "exp","expf","exp2","expm1","log","logf","log1p","log2","log10",
        "pow","powf","ceil","ceilf","floor","floorf","round","roundf","trunc","truncf",
        "fmod","fmodf","fabs","fabsf","hypot","hypotf",
        "isnan","isinf","isfinite","signbit","copysign","copysignf",
        "nextafter","nextafterf","fma","fmax","fmin",
        "lround","lroundf","llround","llroundf","scalbln","scalbn",
        "frexp","ldexp","modf","nan","nanf",
        // <ctype.h>
        "isalpha","isdigit","isalnum","isspace","isupper","islower","isxdigit",
        "iscntrl","isprint","ispunct","isgraph","isblank","isascii",
        "tolower","toupper","toascii",
        // <time.h>
        "time","clock","difftime","mktime","localtime","gmtime","asctime","ctime",
        "strftime","strptime","gettimeofday","clock_gettime","nanosleep",
        // <unistd.h> (sometimes pulled via SDL)
        "read","write","close","lseek","unlink","access","getpid","getppid",
        "fork","execve","sleep","usleep","alarm","pipe","dup","dup2",
        // varargs builtins
        "__builtin_va_start","__builtin_va_end","__builtin_va_copy","__builtin_va_arg",
        "__builtin_expect","__builtin_unreachable","__builtin_offsetof",
        NULL
    };
    for (int i = 0; libc_names[i]; i++)
        if (strcmp(name, libc_names[i]) == 0) return true;
    // Skip any name that starts with __ (compiler builtins / reserved).
    if (name[0] == '_' && name[1] == '_') return true;
    // Heuristic suffixes for POSIX/BSD/Apple libc variants. Keeps the
    // explicit list short; almost no user-defined function ends with these.
    int len = (int)strlen(name);
    if (len > 2 && name[len-2] == '_') {
        char s = name[len-1];
        // _r (re-entrant), _s (secure C11), _l (locale-aware)
        if (s == 'r' || s == 's' || s == 'l') {
            // Only skip if the name starts with a known libc prefix to avoid
            // catching legitimate user functions like `transform_l`.
            const char *libc_prefixes[] = {
                "str","mem","print","scan","read","write","ftell","fseek",
                "fgets","fputs","localtime","gmtime","ctime","asctime",
                "strerror","strsignal","strtok","getpw","getgr","gethostby",
                "rand","drand","erand","jrand","lrand","mrand","nrand",
                "qsort","bsearch","strftime","wcs", NULL };
            for (int i = 0; libc_prefixes[i]; i++) {
                int plen = (int)strlen(libc_prefixes[i]);
                if (plen <= len - 2 && memcmp(name, libc_prefixes[i], plen) == 0)
                    return true;
            }
        }
    }
    // _np (non-portable, Apple/BSD extension): always skip.
    if (len > 3 && name[len-3] == '_' && name[len-2] == 'n' && name[len-1] == 'p')
        return true;
    // _chk (FORTIFY_SOURCE wrappers): always skip.
    if (len > 4 && memcmp(name + len - 4, "_chk", 4) == 0) return true;
    return false;
}

// Same idea for typedefs and structs. The system headers already define
// these — emitting our own MIX shape would create a `typedef struct ... {}`
// in the C output that conflicts with the libc one.
static bool is_libc_typedef(const char *name) {
    static const char *libc_typedefs[] = {
        "FILE","fpos_t","DIR",
        "size_t","ssize_t","ptrdiff_t","off_t","off64_t","time_t","clock_t",
        "wchar_t","wint_t","wctype_t","mbstate_t","sig_atomic_t","jmp_buf",
        "div_t","ldiv_t","lldiv_t","va_list","__builtin_va_list",
        "intmax_t","uintmax_t","intptr_t","uintptr_t",
        "pid_t","uid_t","gid_t","mode_t","dev_t","ino_t","nlink_t","blksize_t","blkcnt_t",
        "useconds_t","suseconds_t","fsblkcnt_t","fsfilcnt_t","id_t","key_t",
        "tm","timespec","timeval","timezone","stat","stat64","statvfs","statfs","tms",
        "lconv","fenv_t","fexcept_t","femode_t","sigset_t","sigaction","siginfo_t",
        "sched_param","pthread_t","pthread_attr_t","pthread_mutex_t",
        "pthread_mutexattr_t","pthread_cond_t","pthread_condattr_t",
        "pthread_rwlock_t","pthread_key_t","pthread_once_t",
        // libc internal/Apple variants
        "__sFILE","__sbuf","__sFILEX",
        NULL
    };
    for (int i = 0; libc_typedefs[i]; i++)
        if (strcmp(name, libc_typedefs[i]) == 0) return true;
    if (name[0] == '_' && name[1] == '_') return true;
    return false;
}

// ---- Output writer ----

static void write_mix_output(FILE *out, const char *lib_name, const char *source_path) {
    fprintf(out, "// Auto-generated MIX bindings for %s\n", lib_name);
    fprintf(out, "// Source: %s\n", source_path);
    fprintf(out, "// Generated by: mix --bind\n\n");

    // Emit shape declarations for parsed structs (skip libc-provided ones).
    int emitted_shapes = 0;
    for (int i = 0; i < shape_count; i++) {
        if (is_libc_typedef(shapes[i].name)) continue;
        if (emitted_shapes++ == 0) fprintf(out, "// ---- Shapes ----\n\n");
        const char *kw = shapes[i].is_union ? "union" : "shape";
        fprintf(out, "%s %s\n%s\n\n", kw, shapes[i].name, shapes[i].fields);
    }

    int emitted_funcs = 0;
    for (int i = 0; i < func_count; i++) {
        if (is_libc_name(funcs[i].name)) continue;
        if (funcs[i].c_name[0] && is_libc_name(funcs[i].c_name)) continue;
        if (emitted_funcs++ == 0) fprintf(out, "extern \"%s\"\n", lib_name);
        if (funcs[i].is_variadic) fprintf(out, "    // variadic\n");
        fprintf(out, "    %s", funcs[i].name);
        if (funcs[i].c_name[0]) fprintf(out, " \"%s\"", funcs[i].c_name);
        fprintf(out, "(%s)", funcs[i].params_mix);
        if (strlen(funcs[i].ret_mix) > 0) fprintf(out, " -> %s", funcs[i].ret_mix);
        fprintf(out, " ~\n");
    }
    if (emitted_funcs > 0) fprintf(out, "\n");

    for (int i = 0; i < const_count; i++) {
        if (is_libc_name(consts[i].name)) continue;
        if (consts[i].is_shape_lit) {
            fprintf(out, "@const %s = %s(%s)\n",
                    consts[i].name, consts[i].shape_name, consts[i].shape_values);
        } else if (consts[i].is_float) {
            fprintf(out, "@const %s = %g\n", consts[i].name, consts[i].float_value);
        } else if (consts[i].is_hex) {
            fprintf(out, "@const %s = 0x%llx\n", consts[i].name, consts[i].int_value);
        } else {
            fprintf(out, "@const %s = %lld\n", consts[i].name, consts[i].int_value);
        }
    }
}

// ---- Path helpers ----

static bool path_is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *basename_no_ext(const char *path) {
    static char buf[256];
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    // If it's a directory, use the directory name
    if (strlen(buf) == 0) {
        // trailing slash case, try again
        if (slash && slash > path) {
            const char *prev = slash - 1;
            while (prev > path && *prev != '/') prev--;
            if (*prev == '/') prev++;
            int len = (int)(slash - prev);
            if (len > 0 && len < 255) {
                memcpy(buf, prev, len);
                buf[len] = '\0';
            }
        }
    }
    return buf;
}

// ---- Public entry point ----

int cbind_generate(const char *header_path, const char *out_path,
                   const char *lib_name, bool verbose) {
    // Reset globals
    func_count = 0;
    const_count = 0;
    shape_count = 0;
    typedef_count = 0;
    typedef_count = 0;
    fp_typedef_count = 0;
    enum_name_count = 0;

    // Derive lib name if not provided
    char derived_lib[256] = "";
    if (!lib_name || strlen(lib_name) == 0) {
        strncpy(derived_lib, basename_no_ext(header_path), sizeof(derived_lib) - 1);
        lib_name = derived_lib;
    }

    if (path_is_directory(header_path)) {
        // Count total .h files first for progress
        DIR *dir = opendir(header_path);
        if (!dir) {
            fprintf(stderr, "mix: cannot open directory '%s'\n", header_path);
            return 1;
        }
        int total_files = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            int nlen = (int)strlen(name);
            if (nlen < 3 || strcmp(name + nlen - 2, ".h") != 0) continue;
            total_files++;
        }
        closedir(dir);

        if (total_files == 0) {
            fprintf(stderr, "mix: no .h files found in '%s'\n", header_path);
            return 1;
        }

        // Process all .h files in the directory
        dir = opendir(header_path);
        if (!dir) {
            fprintf(stderr, "mix: cannot open directory '%s'\n", header_path);
            return 1;
        }
        int file_count = 0;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            int nlen = (int)strlen(name);
            if (nlen < 3 || strcmp(name + nlen - 2, ".h") != 0) continue;

            file_count++;
            fprintf(stderr, "mix: [%d/%d] %s\n", file_count, total_files, name);

            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", header_path, name);

            char *text = preprocess_header(full_path, verbose);
            if (text) {
                parse_typedefs(text);
                parse_structs(text);
                parse_unions(text);
                parse_enums(text);
                int nf = parse_prototypes(text);
                free(text);
                int nc = extract_defines(full_path, verbose);
                if (verbose) fprintf(stderr, "  -> %d functions, %d constants\n", nf, nc);
            } else {
                extract_defines(full_path, verbose);
            }
        }
        closedir(dir);
    } else {
        // Single file
        char *text = preprocess_header(header_path, verbose);
        if (!text) {
            fprintf(stderr, "mix: failed to preprocess '%s'\n", header_path);
            return 1;
        }
        parse_typedefs(text);
        parse_enums(text);
        parse_structs(text);
        parse_unions(text);
        parse_func_ptr_typedefs(text);
        parse_func_ptr_globals(text);
        parse_prototypes(text);
        free(text);
        extract_defines(header_path, verbose);
    }

    // Write output
    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "mix: cannot create '%s'\n", out_path);
        return 1;
    }

    write_mix_output(out, lib_name, header_path);
    fclose(out);

    fprintf(stderr, "mix: wrote %d shapes, %d functions, %d constants to %s\n",
            shape_count, func_count, const_count, out_path);
    return 0;
}

// Generate MIX binding source as an in-memory string.
// source_dir: directory of the .mix file (NULL to skip), forwarded to
// resolve_header_path.
// Returns a malloc'd string (caller must free), or NULL on failure.
char *cbind_generate_string(const char *header_path, const char *lib_name,
                            bool verbose, const char *source_dir) {
    // Reset globals
    func_count = 0;
    const_count = 0;
    shape_count = 0;
    typedef_count = 0;

    // Try to resolve header path via source dir, CPPFLAGS -I directories
    char *resolved = resolve_header_path(header_path, source_dir);
    if (resolved) {
        header_path = resolved;
    }

    // Derive lib name if not provided
    char derived_lib[256] = "";
    if (!lib_name || strlen(lib_name) == 0) {
        strncpy(derived_lib, basename_no_ext(header_path), sizeof(derived_lib) - 1);
        lib_name = derived_lib;
    }

    if (path_is_directory(header_path)) {
        DIR *dir = opendir(header_path);
        if (!dir) {
            fprintf(stderr, "mix: cannot open directory '%s'\n", header_path);
            free(resolved);
            return NULL;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            int nlen = (int)strlen(name);
            if (nlen < 3 || strcmp(name + nlen - 2, ".h") != 0) continue;

            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", header_path, name);

            char *text = preprocess_header(full_path, verbose);
            if (text) {
                parse_structs(text);
                parse_enums(text);
                parse_prototypes(text);
                free(text);
                extract_defines(full_path, verbose);
            } else {
                extract_defines(full_path, verbose);
            }
        }
        closedir(dir);
    } else {
        char *text = preprocess_header(header_path, verbose);
        if (!text) {
            fprintf(stderr, "mix: failed to preprocess '%s'\n", header_path);
            free(resolved);
            return NULL;
        }
        parse_typedefs(text);
        parse_enums(text);
        parse_structs(text);
        parse_unions(text);
        parse_func_ptr_typedefs(text);
        parse_func_ptr_globals(text);
        parse_prototypes(text);
        free(text);
        extract_defines(header_path, verbose);
    }

    // Write to in-memory buffer via open_memstream
    char *buf = NULL;
    size_t buf_size = 0;
    FILE *mem = open_memstream(&buf, &buf_size);
    if (!mem) {
        fprintf(stderr, "mix: open_memstream failed\n");
        free(resolved);
        return NULL;
    }

    write_mix_output(mem, lib_name, header_path);
    fclose(mem);

    if (verbose) {
        fprintf(stderr, "mix: generated %d shapes, %d functions, %d constants from '%s'\n",
                shape_count, func_count, const_count, header_path);
    }

    free(resolved);
    return buf;
}
