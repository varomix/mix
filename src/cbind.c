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
    char ret_mix[64];
    char params_mix[2048];
    bool is_variadic;
} CFunc;

typedef struct {
    char name[256];
    long long int_value;
    double float_value;
    bool is_float;
} CConst;

#define MAX_FUNCS  4096
#define MAX_CONSTS 4096
#define MAX_BUF    (16 * 1024 * 1024) // 16MB preprocessor output limit

static CFunc funcs[MAX_FUNCS];
static int func_count = 0;
static CConst consts[MAX_CONSTS];
static int const_count = 0;

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

    char cmd[2048];
    if (include_dir[0])
        snprintf(cmd, sizeof(cmd), "cc -E -P -I\"%s\" -x c \"%s\" 2>/dev/null",
                 include_dir, path);
    else
        snprintf(cmd, sizeof(cmd), "cc -E -P -x c \"%s\" 2>/dev/null", path);
    return run_command(cmd, verbose);
}

// ---- Extract #define constants ----

static int extract_defines(const char *path, bool verbose) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cc -E -dM -x c \"%s\" 2>/dev/null", path);
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

        // Try integer parse
        char *endp;
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

    snprintf(out, 256, "%s: %s", pname, mix_type);
    return true;
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
        strncpy(funcs[func_count].ret_mix, ret_mix, 63);
        strncpy(funcs[func_count].params_mix, params_mix, sizeof(funcs[func_count].params_mix) - 1);
        funcs[func_count].is_variadic = is_variadic;
        func_count++;
        added++;
    }

    return added;
}

// ---- Output writer ----

static void write_mix_output(FILE *out, const char *lib_name, const char *source_path) {
    fprintf(out, "// Auto-generated MIX bindings for %s\n", lib_name);
    fprintf(out, "// Source: %s\n", source_path);
    fprintf(out, "// Generated by: mix --bind\n\n");

    if (func_count > 0) {
        fprintf(out, "extern \"%s\"\n", lib_name);
        for (int i = 0; i < func_count; i++) {
            if (funcs[i].is_variadic) {
                fprintf(out, "    // variadic\n");
            }
            fprintf(out, "    %s(%s)", funcs[i].name, funcs[i].params_mix);
            if (strlen(funcs[i].ret_mix) > 0) {
                fprintf(out, " -> %s", funcs[i].ret_mix);
            }
            fprintf(out, " ~\n");
        }
        fprintf(out, "\n");
    }

    if (const_count > 0) {
        for (int i = 0; i < const_count; i++) {
            if (consts[i].is_float) {
                fprintf(out, "@const %s = %g\n", consts[i].name, consts[i].float_value);
            } else {
                fprintf(out, "@const %s = %lld\n", consts[i].name, consts[i].int_value);
            }
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

    fprintf(stderr, "mix: wrote %d functions, %d constants to %s\n",
            func_count, const_count, out_path);
    return 0;
}
