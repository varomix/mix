#include "mix.h"
#include "arena.h"
#include "errors.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "sema.h"
#include "qbe_emit.h"
#include "c_emit.h"
#include "llvm_emit.h"
#include "lower.h"
#include "lir.h"
#include "cbind.h"
#include "fmt.h"
#include <sys/time.h>

// --- Phase timing (only printed when --timings is on) ---
static bool g_timings = false;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

#define TIMER_START(label) double _t_##label = now_ms()
#define TIMER_END(label) do { \
    if (g_timings) fprintf(stderr, "  %-12s %7.2f ms\n", #label, now_ms() - _t_##label); \
} while (0)
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

typedef enum {
    MODE_NONE,
    MODE_BUILD,
    MODE_RUN,
    MODE_FMT,
} RunMode;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "mix: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) { fprintf(stderr, "mix: out of memory\n"); exit(1); }
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

#ifdef __APPLE__
static char *detect_macos_sdk_libdir(Arena *arena) {
    FILE *fp = popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (!fp) return NULL;

    char sdk_path[1024];
    if (!fgets(sdk_path, sizeof(sdk_path), fp)) {
        pclose(fp);
        return NULL;
    }
    pclose(fp);

    size_t len = strlen(sdk_path);
    while (len > 0 && (sdk_path[len - 1] == '\n' || sdk_path[len - 1] == '\r')) {
        sdk_path[--len] = '\0';
    }
    if (len == 0) return NULL;

    char libdir[1200];
    snprintf(libdir, sizeof(libdir), "%s/usr/lib", sdk_path);
    struct stat st;
    if (stat(libdir, &st) != 0) return NULL;

    return arena_strdup(arena, libdir);
}
#endif

// Run a subprocess without shell interpretation (prevents command injection).
// argv must be NULL-terminated. Returns the process exit code, or -1 on failure.
// If captured_stderr is non-NULL, child stderr is captured into a malloc'd buffer
// (caller must free) instead of going to the terminal.
static int run_process(const char *const argv[], char **captured_stderr) {
    int pipe_fds[2] = {-1, -1};
    if (captured_stderr) {
        *captured_stderr = NULL;
        if (pipe(pipe_fds) < 0) {
            fprintf(stderr, "mix: pipe failed: %s\n", strerror(errno));
            return -1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "mix: fork failed: %s\n", strerror(errno));
        if (captured_stderr) { close(pipe_fds[0]); close(pipe_fds[1]); }
        return -1;
    }
    if (pid == 0) {
        if (captured_stderr) {
            close(pipe_fds[0]);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[1]);
        }
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "mix: exec '%s' failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    // Parent
    if (captured_stderr) {
        close(pipe_fds[1]);
        // Read child stderr into buffer
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        if (!buf) { close(pipe_fds[0]); *captured_stderr = NULL; }
        else {
        ssize_t n;
        while ((n = read(pipe_fds[0], buf + len, cap - len - 1)) > 0) {
            len += n;
            if (len + 1 >= cap) {
                cap *= 2;
                char *newbuf = realloc(buf, cap);
                if (!newbuf) break;
                buf = newbuf;
            }
        }
        buf[len] = '\0';
        close(pipe_fds[0]);
        *captured_stderr = buf;
        }
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "mix: waitpid failed: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// Print an argv array as a command string (for verbose logging).
static void print_argv(const char *const argv[]) {
    for (int i = 0; argv[i]; i++) {
        if (i > 0) fprintf(stderr, " ");
        fprintf(stderr, "%s", argv[i]);
    }
    fprintf(stderr, "\n");
}

static void usage(void) {
    fprintf(stderr, "mix %s (%s)\n\n", MIX_VERSION, MIX_VERSION_DATE);
    fprintf(stderr, "Usage: mix [command] [options] [file.mix]\n\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  build [file.mix]    Compile to binary\n");
    fprintf(stderr, "  run [file.mix]      Compile and execute\n");
    fprintf(stderr, "  run -f <file.mix>   Compile and run specific file\n");
    fprintf(stderr, "  fmt [path...]       Format source files or directories.\n");
    fprintf(stderr, "                      Reads stdin if no path given.\n");
    fprintf(stderr, "                      Flags: -w (write in place),\n");
    fprintf(stderr, "                             --check (exit 1 if reformat needed),\n");
    fprintf(stderr, "                             --diff (print unified diff)\n\n");
    fprintf(stderr, "  Running 'mix' with no command or file shows this help.\n");
    fprintf(stderr, "  'build' or 'run' without a file auto-discovers main().\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>           Output file (default: derived from input)\n");
    fprintf(stderr, "  --emit-ir           Output backend IR only (.ssa for qbe, .c for c, .ll for llvm)\n");
    fprintf(stderr, "  --emit-tokens       Print token stream\n");
    fprintf(stderr, "  --emit-ast          Print AST\n");
    fprintf(stderr, "  --debug             Enable debug mode (DWARF info + @debug)\n");
    fprintf(stderr, "  --bind <path>       Generate .mix bindings from C header(s)\n");
    fprintf(stderr, "  --backend <name>    Backend: llvm (default), qbe (legacy/parity oracle), c (fallback)\n");
    fprintf(stderr, "  --lib <name>        Library name for --bind (e.g., SDL3)\n");
    fprintf(stderr, "  --version           Show version\n");
    fprintf(stderr, "  -v                  Verbose\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  mix game.mix              Compile game.mix -> game\n");
    fprintf(stderr, "  mix build game.mix        Compile game.mix -> game\n");
    fprintf(stderr, "  mix run game.mix          Compile and run game.mix\n");
    fprintf(stderr, "  mix build                 Auto-find main() file, compile\n");
    fprintf(stderr, "  mix run                   Auto-find main() file, compile & run\n");
    fprintf(stderr, "  mix game.mix -o out       Compile game.mix -> out\n");
    exit(1);
}

// Scan CWD for *.mix files containing a main() function definition.
// Returns the filename if exactly 1 found, or NULL (with error message) otherwise.
static const char *auto_discover_main(void) {
    DIR *dir = opendir(".");
    if (!dir) {
        fprintf(stderr, "mix: cannot open current directory\n");
        return NULL;
    }

    static char found_file[512];
    int match_count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        int len = (int)strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".mix") != 0) continue;

        // Open and check for "main(" at start of a line
        FILE *f = fopen(name, "r");
        if (!f) continue;
        char line[1024];
        bool has_main = false;
        while (fgets(line, sizeof(line), f)) {
            // Skip leading whitespace
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "main(", 5) == 0) {
                has_main = true;
                break;
            }
        }
        fclose(f);

        if (has_main) {
            match_count++;
            if (match_count == 1) {
                strncpy(found_file, name, sizeof(found_file) - 1);
                found_file[sizeof(found_file) - 1] = '\0';
            }
        }
    }
    closedir(dir);

    if (match_count == 0) {
        fprintf(stderr, "mix: no .mix file with main() found in current directory\n");
        return NULL;
    }
    if (match_count > 1) {
        fprintf(stderr, "mix: multiple .mix files with main() found, specify one\n");
        return NULL;
    }
    return found_file;
}

// Derive output name from input filename: "examples/game.mix" -> "game"
static void derive_output_name(const char *input_file, char *out, size_t out_size) {
    // Get basename
    const char *slash = strrchr(input_file, '/');
    const char *base = slash ? slash + 1 : input_file;

    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';

    // Strip .mix extension
    char *dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".mix") == 0) {
        *dot = '\0';
    }
}

// --- mix fmt helpers ---

// Format `src` to a freshly-allocated buffer. Returns NULL on failure.
// Caller frees.
static char *fmt_to_buf(const char *src, const char *path) {
    char *buf = NULL;
    size_t buf_size = 0;
    FILE *mem = open_memstream(&buf, &buf_size);
    if (!mem) return NULL;
    int rc = mix_format(src, path, mem);
    fclose(mem);
    if (rc != 0) { free(buf); return NULL; }
    return buf;
}

// Tiny line-based unified diff suitable for human review. Not a full
// hunk-coalescing implementation — emits one hunk per file.
static void fmt_print_diff(const char *path, const char *a, const char *b) {
    printf("--- %s\n", path);
    printf("+++ %s (formatted)\n", path);
    const char *ap = a;
    const char *bp = b;
    while (*ap || *bp) {
        const char *anl = strchr(ap, '\n');
        const char *bnl = strchr(bp, '\n');
        size_t alen = anl ? (size_t)(anl - ap) : strlen(ap);
        size_t blen = bnl ? (size_t)(bnl - bp) : strlen(bp);
        if (alen == blen && memcmp(ap, bp, alen) == 0) {
            printf(" %.*s\n", (int)alen, ap);
        } else {
            if (*ap) printf("-%.*s\n", (int)alen, ap);
            if (*bp) printf("+%.*s\n", (int)blen, bp);
        }
        if (anl) ap = anl + 1; else ap += alen;
        if (bnl) bp = bnl + 1; else bp += blen;
    }
}

// Skip directories that obviously hold generated/vendor content during a
// recursive scan. Mirrors the workspace-cache rule in lsp_workspace.c.
static bool fmt_skip_dir(const char *name) {
    if (name[0] == '.') return true;
    if (strcmp(name, "build") == 0) return true;
    if (strcmp(name, "node_modules") == 0) return true;
    if (strcmp(name, "out") == 0) return true;
    if (strcmp(name, "target") == 0) return true;
    return false;
}

static int fmt_one_file(const char *path, bool write_in_place,
                        bool check, bool diff);

static int fmt_walk_dir(const char *dir, bool write_in_place,
                        bool check, bool diff) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "mix fmt: cannot open '%s': %s\n", dir, strerror(errno));
        return 1;
    }
    int worst = 0;
    struct dirent *ent;
    char path[1024];
    // Strip trailing slash so we don't produce "dir//file"
    int dir_len = (int)strlen(dir);
    while (dir_len > 1 && dir[dir_len - 1] == '/') dir_len--;
    while ((ent = readdir(d)) != NULL) {
        if (fmt_skip_dir(ent->d_name)) continue;
        snprintf(path, sizeof(path), "%.*s/%s", dir_len, dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int rc = fmt_walk_dir(path, write_in_place, check, diff);
            if (rc > worst) worst = rc;
        } else if (S_ISREG(st.st_mode)) {
            size_t n = strlen(ent->d_name);
            if (n < 5 || strcmp(ent->d_name + n - 4, ".mix") != 0) continue;
            int rc = fmt_one_file(path, write_in_place, check, diff);
            if (rc > worst) worst = rc;
        }
    }
    closedir(d);
    return worst;
}

static int fmt_one_file(const char *path, bool write_in_place,
                        bool check, bool diff) {
    char *src = read_file(path);
    if (!src) return 1;
    char *out = fmt_to_buf(src, path);
    if (!out) {
        fprintf(stderr, "mix fmt: failed to format '%s'\n", path);
        free(src);
        return 1;
    }

    int rc = 0;
    bool changed = (strcmp(src, out) != 0);

    if (check) {
        if (changed) { printf("%s\n", path); rc = 1; }
    } else if (diff) {
        if (changed) fmt_print_diff(path, src, out);
    } else if (write_in_place) {
        if (changed) {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s.fmt.tmp", path);
            FILE *f = fopen(tmp, "w");
            if (!f) {
                fprintf(stderr, "mix fmt: cannot create '%s': %s\n", tmp, strerror(errno));
                rc = 1;
            } else {
                fputs(out, f);
                fclose(f);
                if (rename(tmp, path) != 0) {
                    fprintf(stderr, "mix fmt: rename failed: %s\n", strerror(errno));
                    unlink(tmp);
                    rc = 1;
                }
            }
        }
    } else {
        fputs(out, stdout);
    }

    free(src);
    free(out);
    return rc;
}

// Top-level fmt command: dispatches over the collected paths. With no
// paths, reads stdin and writes to stdout. Returns the worst exit code.
static int fmt_dispatch(const char **paths, int n_paths,
                        bool write_in_place, bool check, bool diff) {
    if (n_paths == 0) {
        // Stdin mode — only --check makes sense besides default.
        size_t cap = 8192, len = 0;
        char *src = malloc(cap);
        int c;
        while ((c = getchar()) != EOF) {
            if (len + 1 >= cap) { cap *= 2; src = realloc(src, cap); }
            src[len++] = (char)c;
        }
        src[len] = '\0';
        char *out = fmt_to_buf(src, NULL);
        int rc = 0;
        if (!out) {
            fprintf(stderr, "mix fmt: failed to format stdin\n");
            rc = 1;
        } else if (check) {
            rc = (strcmp(src, out) == 0) ? 0 : 1;
        } else if (diff) {
            if (strcmp(src, out) != 0) fmt_print_diff("<stdin>", src, out);
        } else {
            fputs(out, stdout);
        }
        free(src); free(out);
        return rc;
    }

    int worst = 0;
    for (int i = 0; i < n_paths; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) {
            fprintf(stderr, "mix fmt: cannot stat '%s': %s\n", paths[i], strerror(errno));
            if (1 > worst) worst = 1;
            continue;
        }
        int rc;
        if (S_ISDIR(st.st_mode)) {
            rc = fmt_walk_dir(paths[i], write_in_place, check, diff);
        } else {
            rc = fmt_one_file(paths[i], write_in_place, check, diff);
        }
        if (rc > worst) worst = rc;
    }
    return worst;
}

// Resolve module path: "math" -> "./math.mix", "engine.physics" -> "./engine/physics.mix"
// For "std.*" modules, search relative to the compiler binary first.
// Path-style modules (containing '/' from `use ../../mixel`) are joined to
// base_dir verbatim — no dot-to-slash conversion.
static char *resolve_module_path(Arena *arena, const char *base_dir,
                                  const char *module_path, const char *exe_dir) {
    // Path-style: `use ../../mixel` parsed module_path = "../../mixel".
    // Treat as a literal relative path under base_dir.
    if (strchr(module_path, '/')) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.mix", base_dir, module_path);
        return arena_strdup(arena, path);
    }
    // Check for stdlib import: starts with "std."
    if (strncmp(module_path, "std.", 4) == 0) {
        const char *rest = module_path + 4; // skip "std."
        char std_path[512];

        // Build relative path from rest (convert dots to slashes)
        char rel[256];
        int ri = 0;
        for (const char *p = rest; *p && ri < (int)sizeof(rel) - 1; p++) {
            rel[ri++] = (*p == '.') ? '/' : *p;
        }
        rel[ri] = '\0';

        if (exe_dir && exe_dir[0]) {
            // Try: <exe_dir>/../lib/mix/std/<rest>.mix (installed layout)
            snprintf(std_path, sizeof(std_path), "%s/../lib/mix/std/%s.mix", exe_dir, rel);
            FILE *f = fopen(std_path, "r");
            if (f) { fclose(f); return arena_strdup(arena, std_path); }

            // Try: <exe_dir>/../lib/std/<rest>.mix (dev layout: build/ -> lib/)
            snprintf(std_path, sizeof(std_path), "%s/../lib/std/%s.mix", exe_dir, rel);
            f = fopen(std_path, "r");
            if (f) { fclose(f); return arena_strdup(arena, std_path); }
        }

        // Fallthrough: try as a regular relative path
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/", base_dir);
    int off = (int)strlen(path);

    for (const char *p = module_path; *p; p++) {
        if (off >= (int)sizeof(path) - 5) break;
        if (*p == '.') {
            path[off++] = '/';
        } else {
            path[off++] = *p;
        }
    }
    path[off] = '\0';
    strncat(path, ".mix", sizeof(path) - strlen(path) - 1);
    return arena_strdup(arena, path);
}

// Circular import detection — stack of modules currently being compiled.
// Entries are pushed on entry to compile_module and popped on exit,
// so only modules on the current call-chain are tracked.
#define MAX_COMPILING_MODULES 128
#define MAX_LINK_FLAGS 64
static const char *compiling_modules[MAX_COMPILING_MODULES];
static int compiling_module_count = 0;

static bool is_module_compiling(const char *path) {
    for (int i = 0; i < compiling_module_count; i++) {
        if (strcmp(compiling_modules[i], path) == 0) return true;
    }
    return false;
}

// Walk the symbol list from head down to (but not including) snapshot, and
// remove any symbol whose name is NOT in `imports`. Used to apply selective
// imports after a sub-module has been analyzed (which inserts all pub symbols
// into the shared symtab).
//
// For shape methods (mangled `Shape_method`), the *shape* is what gets named
// in the imports list; methods/variants are kept iff their owning shape name
// matches an import.
static void filter_imported_symbols(SymTab *symtab, Symbol *snapshot,
                                    char **imports, int import_count) {
    if (import_count == 0) return;

    Symbol **link = &symtab->current->symbols;
    while (*link && *link != snapshot) {
        Symbol *sym = *link;
        bool keep = false;
        for (int i = 0; i < import_count; i++) {
            const char *want = imports[i];
            size_t wlen = strlen(want);
            // Exact match: regular symbol (function, constant, shape, etc.)
            if (strcmp(sym->name, want) == 0) { keep = true; break; }
            // Method/variant prefix match: `Shape_method` belongs to `Shape`
            if (strncmp(sym->name, want, wlen) == 0 && sym->name[wlen] == '_') {
                keep = true; break;
            }
        }
        if (keep) {
            link = &sym->next;
        } else {
            *link = sym->next;  // splice out
        }
    }
}

// Compile a single module, return its output file path (or NULL on failure).
// Recursively compiles any modules this module imports via 'use'.
// For QBE backend:  returns .s file path
// For C backend:    returns .c file path
// For LLVM backend: returns .o file path (Phase 1: text IR + clang -c)
static char *compile_module(const char *source_path, Arena *arena, Sema *sema,
                            const char *base_dir, bool verbose,
                            bool debug, bool use_c_backend, bool use_llvm_backend,
                            const char *exe_dir,
                            char **module_asm_files, int *module_count,
                            int max_modules,
                            char **link_flags, int *link_flag_count) {
    // Circular import check
    if (is_module_compiling(source_path)) {
        fprintf(stderr, "mix: circular import detected: '%s'\n", source_path);
        return NULL;
    }
    if (compiling_module_count >= MAX_COMPILING_MODULES) {
        fprintf(stderr, "mix: too many nested imports (max %d)\n", MAX_COMPILING_MODULES);
        return NULL;
    }
    compiling_modules[compiling_module_count++] = source_path;

    char *source = read_file(source_path);
    if (!source) { compiling_module_count--; return NULL; }

    // Set error source to this module's source for correct source-line display
    errors_set_source(source, source_path);

    // Lex
    Lexer lexer = lexer_create(source, source_path, arena);
    lexer_tokenize(&lexer);
    if (mix_error_count() > 0) {
        free(source); free(lexer.tokens); compiling_module_count--;
        return NULL;
    }

    // Parse
    Parser parser = parser_create(lexer.tokens, lexer.token_count, arena, source_path);
    AstNode *program = parser_parse(&parser);
    if (mix_error_count() > 0) {
        free(source); free(lexer.tokens); compiling_module_count--;
        return NULL;
    }

    // Recursively compile any modules this module imports
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_USE_C_DECL) {
            // use c "header.h" in a module — generate bindings and analyze
            char *bind_src = cbind_generate_string(decl->use_c_decl.header_path,
                                                    decl->use_c_decl.lib_name, verbose);
            if (!bind_src) {
                fprintf(stderr, "mix: failed to generate bindings for '%s'\n",
                        decl->use_c_decl.header_path);
                free(source); free(lexer.tokens); compiling_module_count--;
                return NULL;
            }
            errors_set_source(bind_src, decl->use_c_decl.header_path);
            Lexer bl = lexer_create(bind_src, decl->use_c_decl.header_path, arena);
            lexer_tokenize(&bl);
            if (mix_error_count() == 0) {
                Parser bp = parser_create(bl.tokens, bl.token_count, arena, decl->use_c_decl.header_path);
                AstNode *bprog = parser_parse(&bp);
                if (mix_error_count() == 0) sema_analyze(sema, bprog);
            }
            errors_set_source(source, source_path);
            free(bind_src);
            free(bl.tokens);

            // Propagate `link "lib"` to the root linker line so an importing
            // file doesn't have to re-declare it. Dedupe — multiple modules
            // pulling in the same lib should still produce a single -l flag.
            const char *lib = decl->use_c_decl.lib_name;
            if (lib && link_flags && link_flag_count) {
                char dash_l[256];
                snprintf(dash_l, sizeof(dash_l), "-l%s", lib);
                bool already = false;
                for (int li = 0; li < *link_flag_count; li++) {
                    if (strcmp(link_flags[li], dash_l) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already && *link_flag_count < MAX_LINK_FLAGS) {
                    link_flags[(*link_flag_count)++] = arena_strdup(arena, dash_l);
                }
            }
        } else if (decl->kind == NODE_USE_DECL) {
            char *sub_path = resolve_module_path(arena, base_dir,
                                                  decl->use_decl.module_path, exe_dir);
            if (verbose) fprintf(stderr, "mix: compiling sub-module '%s' -> %s\n",
                                 decl->use_decl.module_path, sub_path);

            // Snapshot symtab head so we can filter selective imports after compile
            Symbol *snap = sema->symtab.current->symbols;

            char *sub_asm = compile_module(sub_path, arena, sema, base_dir,
                                           verbose, debug, use_c_backend, use_llvm_backend,
                                           exe_dir, module_asm_files,
                                           module_count, max_modules,
                                           link_flags, link_flag_count);
            if (!sub_asm) {
                fprintf(stderr, "mix: failed to compile module '%s'\n",
                        decl->use_decl.module_path);
                free(source); free(lexer.tokens); compiling_module_count--;
                return NULL;
            }
            // Apply selective imports if specified
            filter_imported_symbols(&sema->symtab, snap,
                                    decl->use_decl.imports,
                                    decl->use_decl.import_count);

            // Restore error source to this module after sub-module compilation
            errors_set_source(source, source_path);

            if (*module_count >= max_modules) {
                fprintf(stderr, "mix: too many modules (max %d)\n", max_modules);
                free(source); free(lexer.tokens); compiling_module_count--;
                return NULL;
            }
            module_asm_files[(*module_count)++] = sub_asm;
        }
    }

    // Analyze (uses shared sema so pub symbols are visible to main)
    sema_analyze(sema, program);
    if (mix_error_count() > 0) {
        free(source); free(lexer.tokens); compiling_module_count--;
        return NULL;
    }

    int mod_id = *module_count;  // unique ID for temp file naming

    if (use_c_backend) {
        // C backend: emit .c file directly
        char c_path[256];
        snprintf(c_path, sizeof(c_path), "/tmp/mod_%d_%d.c", getpid(), mod_id);
        FILE *c_out = fopen(c_path, "w");
        if (!c_out) {
            free(source); free(lexer.tokens); compiling_module_count--;
            return NULL;
        }
        CEmitter c_emitter = c_emitter_create(c_out, arena, &sema->symtab);
        c_emit_program(&c_emitter, program);
        fclose(c_out);
        free(source);
        free(lexer.tokens);
        compiling_module_count--;
        return arena_strdup(arena, c_path);
    } else if (use_llvm_backend) {
        // LLVM backend (Phase 2): AST → lower → LIR → llvm_emit → .ll,
        // then clang -c → .o. Modules are not yet exercised at this phase
        // (hello.mix has no imports), but this branch keeps the surface
        // uniform so Phase 3 expansion does not re-touch compile_module.
        char ll_path[256], obj_path[256];
        snprintf(ll_path,  sizeof(ll_path),  "/tmp/mod_%d_%d.ll", getpid(), mod_id);
        snprintf(obj_path, sizeof(obj_path), "/tmp/mod_%d_%d.o",  getpid(), mod_id);

        FILE *ll_out = fopen(ll_path, "w");
        if (!ll_out) {
            free(source); free(lexer.tokens); compiling_module_count--;
            return NULL;
        }
        LirModule *lmod = lower_program(program, arena, &sema->symtab);
        if (!lmod || mix_error_count() > 0) {
            fclose(ll_out);
            free(source); free(lexer.tokens); compiling_module_count--;
            return NULL;
        }
        LlvmEmitter le = llvm_emitter_create(ll_out);
        llvm_emit_module(&le, lmod);
        fclose(ll_out);

        const char *clang_argv[] = {"clang", "-c", "-o", obj_path, ll_path, NULL};
        if (verbose) { fprintf(stderr, "mix: "); print_argv(clang_argv); }
        char *clang_stderr = NULL;
        int ret = run_process(clang_argv, verbose ? NULL : &clang_stderr);
        if (ret != 0) {
            if (verbose) {
                fprintf(stderr, "mix: clang -c failed for module '%s'\n", source_path);
            } else {
                fprintf(stderr, "mix: internal compiler error while compiling '%s'. Run with -v for details.\n", source_path);
            }
            free(clang_stderr);
            free(source); free(lexer.tokens); compiling_module_count--;
            return NULL;
        }
        free(clang_stderr);

        if (!verbose) remove(ll_path);
        free(source);
        free(lexer.tokens);
        compiling_module_count--;
        return arena_strdup(arena, obj_path);
    } else {
        // QBE backend: emit .ssa, compile to .s
        char ssa_path[256], asm_path[256];
        snprintf(ssa_path, sizeof(ssa_path), "/tmp/mod_%d_%d.ssa", getpid(), mod_id);
        snprintf(asm_path, sizeof(asm_path), "/tmp/mod_%d_%d.s", getpid(), mod_id);

        FILE *ssa_out = fopen(ssa_path, "w");
        if (!ssa_out) {
            free(source); free(lexer.tokens); compiling_module_count--;
            return NULL;
        }

        QbeEmitter emitter = qbe_emitter_create(ssa_out, arena, &sema->symtab, debug);
        qbe_emit_program(&emitter, program);
        fclose(ssa_out);

        // QBE compile
        const char *qbe_argv[] = {"qbe", "-o", asm_path, ssa_path, NULL};
        if (verbose) { fprintf(stderr, "mix: "); print_argv(qbe_argv); }

        char *qbe_stderr = NULL;
        int ret = run_process(qbe_argv, verbose ? NULL : &qbe_stderr);
        if (ret != 0) {
            if (verbose) {
                fprintf(stderr, "mix: qbe failed for module '%s'\n", source_path);
            } else {
                fprintf(stderr, "mix: internal compiler error while compiling '%s'. Run with -v for details.\n", source_path);
            }
            free(qbe_stderr);
            free(source); free(lexer.tokens); compiling_module_count--;
            return NULL;
        }
        free(qbe_stderr);

        if (!verbose) remove(ssa_path);
        free(source);
        free(lexer.tokens);
        compiling_module_count--;
        return arena_strdup(arena, asm_path);
    }
}

int main(int argc, char **argv) {
    errors_init();

    const char *input_file = NULL;
    const char *output_file = NULL;
    const char *bind_path = NULL;
    const char *bind_lib = NULL;
    bool emit_ir_only = false;
    bool emit_tokens = false;
    bool emit_ast = false;
    bool verbose = false;
    bool debug_mode = false;
    const char *backend = "llvm";  /* "llvm" (default), "qbe", or "c" */
    RunMode mode = MODE_NONE;
    bool output_set = false;
    bool fmt_write_in_place = false;
    bool fmt_check = false;
    bool fmt_diff = false;
    #define MAX_FMT_PATHS 64
    const char *fmt_paths[MAX_FMT_PATHS];
    int fmt_path_count = 0;

    char *link_flags[MAX_LINK_FLAGS];
    int link_flag_count = 0;

    // Parse subcommand (argv[1])
    int arg_start = 1;
    if (argc > 1 && argv[1][0] != '-') {
        if (strcmp(argv[1], "build") == 0) {
            mode = MODE_BUILD;
            arg_start = 2;
        } else if (strcmp(argv[1], "run") == 0) {
            mode = MODE_RUN;
            arg_start = 2;
        } else if (strcmp(argv[1], "fmt") == 0) {
            mode = MODE_FMT;
            arg_start = 2;
        } else {
            // Otherwise it's a filename — legacy mode, arg_start stays 1
            mode = MODE_BUILD;
        }
    }

    for (int i = arg_start; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
            output_set = true;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "--emit-ir") == 0) {
            emit_ir_only = true;
        } else if (strcmp(argv[i], "--emit-tokens") == 0) {
            emit_tokens = true;
        } else if (strcmp(argv[i], "--emit-ast") == 0) {
            emit_ast = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        } else if (strcmp(argv[i], "--release") == 0) {
            debug_mode = false;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("mix %s (%s)\n", MIX_VERSION, MIX_VERSION_DATE);
            return 0;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_path = argv[++i];
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            bind_lib = argv[++i];
        } else if (strcmp(argv[i], "--timings") == 0) {
            g_timings = true;
        } else if (strcmp(argv[i], "-w") == 0) {
            fmt_write_in_place = true;
        } else if (mode == MODE_FMT && strcmp(argv[i], "--check") == 0) {
            fmt_check = true;
        } else if (mode == MODE_FMT && strcmp(argv[i], "--diff") == 0) {
            fmt_diff = true;
    } else if (strncmp(argv[i], "-l", 2) == 0 || strncmp(argv[i], "-L", 2) == 0) {
            if (link_flag_count >= MAX_LINK_FLAGS) {
                fprintf(stderr, "mix: too many linker flags (max %d)\n", MAX_LINK_FLAGS);
                return 1;
            }
            link_flags[link_flag_count++] = argv[i];
        } else if (strcmp(argv[i], "-framework") == 0 && i + 1 < argc) {
            if (link_flag_count + 1 >= MAX_LINK_FLAGS) {
                fprintf(stderr, "mix: too many linker flags (max %d)\n", MAX_LINK_FLAGS);
                return 1;
            }
            link_flags[link_flag_count++] = argv[i];
            link_flags[link_flag_count++] = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "mix: unknown option '%s'\n", argv[i]);
            usage();
        } else if (mode == MODE_FMT) {
            // fmt accepts multiple positional args (files or directories).
            if (fmt_path_count < MAX_FMT_PATHS)
                fmt_paths[fmt_path_count++] = argv[i];
            input_file = argv[i];   // first one also fills input_file
        } else {
            input_file = argv[i];
        }
    }

    // Binding mode: generate .mix from C headers
    if (bind_path) {
        if (!output_file) output_file = "a.out";
        return cbind_generate(bind_path, output_file, bind_lib, verbose);
    }

    if (mode == MODE_FMT) {
        return fmt_dispatch(fmt_paths, fmt_path_count,
                            fmt_write_in_place, fmt_check, fmt_diff);
    }

    // Check MIX_BACKEND env var
    {
        const char *env_backend = getenv("MIX_BACKEND");
        if (env_backend && env_backend[0]) backend = env_backend;
    }
    bool use_c_backend = (strcmp(backend, "c") == 0);
    bool use_llvm_backend = (strcmp(backend, "llvm") == 0);
    if (!use_c_backend && !use_llvm_backend && strcmp(backend, "qbe") != 0) {
        fprintf(stderr, "mix: unknown --backend '%s' (expected: qbe, c, llvm)\n", backend);
        return 1;
    }

    // Auto-discover if no input file specified
    if (!input_file) {
        if (mode == MODE_NONE) {
            usage();
        }
        // Check for build.mix in CWD when in build mode
        if (mode == MODE_BUILD) {
            FILE *bf = fopen("build.mix", "r");
            if (bf) {
                fclose(bf);
                // Set MIX_COMPILER env var to our own path
                char exe_path[1024];
                #ifdef __APPLE__
                uint32_t exe_size2 = sizeof(exe_path);
                if (_NSGetExecutablePath(exe_path, &exe_size2) == 0) {
                    char *resolved = realpath(exe_path, NULL);
                    if (resolved) {
                        setenv("MIX_COMPILER", resolved, 1);
                        free(resolved);
                    }
                }
                #elif defined(__linux__)
                ssize_t len2 = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
                if (len2 > 0) {
                    exe_path[len2] = '\0';
                    setenv("MIX_COMPILER", exe_path, 1);
                }
                #endif

                // Compile build.mix to temp binary and run it
                input_file = "build.mix";
                char tmp_out[256];
                snprintf(tmp_out, sizeof(tmp_out), "/tmp/mix_build_%d", getpid());
                output_file = tmp_out;
                output_set = true;
                mode = MODE_RUN;
                // Fall through to normal compilation pipeline
                goto skip_auto_discover;
            }
        }
        const char *discovered = auto_discover_main();
        if (!discovered) {
            return 1;
        }
        input_file = discovered;
    }
    skip_auto_discover:
    ;  // empty statement after label (C11 compat)

    // Derive default output name from input filename if -o not specified
    static char derived_name[256];
    if (!output_set) {
        derive_output_name(input_file, derived_name, sizeof(derived_name));
        output_file = derived_name;
    }

    // Determine base directory for module resolution
    char base_dir_buf[512];
    {
        // Find last / in input_file
        const char *last_slash = strrchr(input_file, '/');
        if (last_slash) {
            int len = (int)(last_slash - input_file);
            memcpy(base_dir_buf, input_file, len);
            base_dir_buf[len] = '\0';
        } else {
            strcpy(base_dir_buf, ".");
        }
    }
    const char *base_dir = base_dir_buf;

    // Read source
    char *source = read_file(input_file);
    if (!source) return 1;
    errors_set_source(source, input_file);

    Arena arena = arena_create(ARENA_DEFAULT_CAP);

    if (g_timings) fprintf(stderr, "mix: --- compile timings ---\n");

    // Lex
    TIMER_START(lex);
    Lexer lexer = lexer_create(source, input_file, &arena);
    lexer_tokenize(&lexer);
    TIMER_END(lex);

    if (emit_tokens) {
        lexer_print_tokens(&lexer);
        arena_destroy(&arena);
        free(source);
        return 0;
    }

    if (mix_error_count() > 0) {
        fprintf(stderr, "mix: %d error(s) during lexing\n", mix_error_count());
        arena_destroy(&arena); free(source);
        return 1;
    }

    // Parse
    TIMER_START(parse);
    Parser parser = parser_create(lexer.tokens, lexer.token_count, &arena, input_file);
    AstNode *program = parser_parse(&parser);
    TIMER_END(parse);

    if (emit_ast) {
        ast_print(program, 0);
        arena_destroy(&arena); free(source); free(lexer.tokens);
        return 0;
    }

    if (mix_error_count() > 0) {
        fprintf(stderr, "mix: %d error(s) during parsing\n", mix_error_count());
        arena_destroy(&arena); free(source); free(lexer.tokens);
        return 1;
    }

    // Compile imported modules first
    #define MAX_MODULES 64
    char *module_asm_files[MAX_MODULES];
    int module_count = 0;

    // Source files to compile and link (from use c ... source "file.c")
    #define MAX_SOURCE_FILES 16
    const char *source_files[MAX_SOURCE_FILES];
    int source_file_count = 0;

    // Vendor include directories (from lib/vendor/*/include/)
    #define MAX_VENDOR_IDIRS 16
    const char *vendor_idirs[MAX_VENDOR_IDIRS];
    int vendor_idir_count = 0;

    Sema sema = sema_create(&arena);
    sema.debug_mode = debug_mode;

    // Get directory of the mix executable (needed for stdlib path and runtime search)
    char exe_dir[1024] = "";
    {
        char exe_path[1024];
        #ifdef __APPLE__
        uint32_t exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) == 0) {
            char *resolved = realpath(exe_path, NULL);
            if (resolved) {
                char *d = dirname(resolved);
                strncpy(exe_dir, d, sizeof(exe_dir) - 1);
                free(resolved);
            }
        }
        #elif defined(__linux__)
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            char *d = dirname(exe_path);
            strncpy(exe_dir, d, sizeof(exe_dir) - 1);
        }
        #endif
    }

    // Process use declarations — compile each module
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_USE_C_DECL) {
            // use c "header.h" link "lib"
            const char *header = decl->use_c_decl.header_path;
            const char *lib = decl->use_c_decl.lib_name;
            if (verbose) fprintf(stderr, "mix: use c \"%s\"%s%s%s\n",
                                 header, lib ? " link \"" : "", lib ? lib : "", lib ? "\"" : "");

            char *bind_src = cbind_generate_string(header, lib, verbose);
            if (!bind_src) {
                fprintf(stderr, "mix: failed to generate bindings for '%s'\n", header);
                return 1;
            }

            // Feed through lex -> parse -> sema
            errors_set_source(bind_src, header);
            Lexer bind_lex = lexer_create(bind_src, header, &arena);
            lexer_tokenize(&bind_lex);
            if (mix_error_count() > 0) {
                free(bind_src); free(bind_lex.tokens);
                fprintf(stderr, "mix: errors in generated bindings for '%s'\n", header);
                return 1;
            }
            Parser bind_parser = parser_create(bind_lex.tokens, bind_lex.token_count, &arena, header);
            AstNode *bind_prog = parser_parse(&bind_parser);
            if (mix_error_count() > 0) {
                free(bind_src); free(bind_lex.tokens);
                fprintf(stderr, "mix: errors in generated bindings for '%s'\n", header);
                return 1;
            }
            sema_analyze(&sema, bind_prog);
            if (mix_error_count() > 0) {
                free(bind_src); free(bind_lex.tokens);
                fprintf(stderr, "mix: errors in generated bindings for '%s'\n", header);
                return 1;
            }

            // Merge generated declarations into main program AST so emitter sees them
            if (bind_prog && bind_prog->program.decl_count > 0) {
                int old_count = program->program.decl_count;
                int new_count = old_count + bind_prog->program.decl_count;
                AstNode **merged = arena_alloc(&arena, sizeof(AstNode*) * new_count);
                memcpy(merged, program->program.decls, sizeof(AstNode*) * old_count);
                memcpy(merged + old_count, bind_prog->program.decls,
                       sizeof(AstNode*) * bind_prog->program.decl_count);
                program->program.decls = merged;
                program->program.decl_count = new_count;
            }

            // Restore error source
            errors_set_source(source, input_file);
            free(bind_src);
            free(bind_lex.tokens);

            // Collect -l flag for linker (dedupe so the same lib pulled in
            // from multiple `use c` declarations only appears once).
            if (lib && link_flag_count < MAX_LINK_FLAGS) {
                char *lflag = arena_alloc(&arena, strlen(lib) + 3);
                sprintf(lflag, "-l%s", lib);
                bool already = false;
                for (int li = 0; li < link_flag_count; li++) {
                    if (strcmp(link_flags[li], lflag) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) link_flags[link_flag_count++] = lflag;
            }

            // Collect source file for compilation (e.g. source "glad/glad.c")
            const char *src_path = decl->use_c_decl.source_path;
            if (src_path && source_file_count < MAX_SOURCE_FILES) {
                struct stat src_st;
                if (stat(src_path, &src_st) == 0) {
                    source_files[source_file_count++] = arena_strdup(&arena, src_path);
                } else {
                    bool found_src = false;
                    // Extract first component and rest: "glad/glad.c" -> "glad" + "glad.c"
                    const char *src_slash = strchr(src_path, '/');
                    const char *src_rest = src_slash ? src_slash + 1 : NULL;

                    DIR *vdir = opendir("lib/vendor");
                    if (vdir) {
                        struct dirent *ventry;
                        while ((ventry = readdir(vdir)) != NULL) {
                            if (ventry->d_name[0] == '.') continue;
                            char try_paths[5][1024];
                            int try_count = 0;
                            // Try full path under vendor subdirs
                            snprintf(try_paths[try_count++], 1024,
                                     "lib/vendor/%s/src/%s", ventry->d_name, src_path);
                            snprintf(try_paths[try_count++], 1024,
                                     "lib/vendor/%s/%s", ventry->d_name, src_path);
                            // If first component matches vendor name, try rest only
                            if (src_rest && src_slash > src_path) {
                                int prefix_len = (int)(src_slash - src_path);
                                if ((int)strlen(ventry->d_name) == prefix_len &&
                                    strncmp(ventry->d_name, src_path, prefix_len) == 0) {
                                    snprintf(try_paths[try_count++], 1024,
                                             "lib/vendor/%s/src/%s", ventry->d_name, src_rest);
                                    snprintf(try_paths[try_count++], 1024,
                                             "lib/vendor/%s/%s", ventry->d_name, src_rest);
                                }
                            }
                            for (int tp = 0; tp < try_count; tp++) {
                                if (stat(try_paths[tp], &src_st) == 0) {
                                    source_files[source_file_count++] = arena_strdup(&arena, try_paths[tp]);
                                    found_src = true;
                                    break;
                                }
                            }
                            if (found_src) break;
                        }
                        closedir(vdir);
                    }
                    if (!found_src) {
                        fprintf(stderr, "mix: source file '%s' not found\n", src_path);
                        return 1;
                    }
                }
            }

            // Collect vendor -I directories for the linker/compile step
            {
                DIR *vdir = opendir("lib/vendor");
                if (vdir) {
                    struct dirent *ventry;
                    while ((ventry = readdir(vdir)) != NULL) {
                        if (ventry->d_name[0] == '.') continue;
                        char inc_dir[512];
                        snprintf(inc_dir, sizeof(inc_dir), "lib/vendor/%s/include", ventry->d_name);
                        struct stat vst;
                        if (stat(inc_dir, &vst) == 0 && S_ISDIR(vst.st_mode)) {
                            // Check if already in list
                            bool already = false;
                            for (int vi = 0; vi < vendor_idir_count; vi++) {
                                if (strcmp(vendor_idirs[vi], inc_dir) == 0) { already = true; break; }
                            }
                            if (!already && vendor_idir_count < MAX_VENDOR_IDIRS) {
                                vendor_idirs[vendor_idir_count++] = arena_strdup(&arena, inc_dir);
                            }
                        }
                    }
                    closedir(vdir);
                }
            }
        } else if (decl->kind == NODE_USE_DECL) {
            char *mod_path = resolve_module_path(&arena, base_dir,
                                                  decl->use_decl.module_path, exe_dir);
            if (verbose) fprintf(stderr, "mix: compiling module '%s' -> %s\n",
                                 decl->use_decl.module_path, mod_path);

            // Snapshot symtab head so we can filter selective imports after compile
            Symbol *snap = sema.symtab.current->symbols;

            char *asm_file = compile_module(mod_path, &arena, &sema, base_dir,
                                            verbose, debug_mode, use_c_backend, use_llvm_backend,
                                            exe_dir, module_asm_files,
                                            &module_count, MAX_MODULES,
                                            link_flags, &link_flag_count);
            if (!asm_file) {
                fprintf(stderr, "mix: failed to compile module '%s'\n", decl->use_decl.module_path);
                return 1;
            }
            // Apply selective imports if specified
            filter_imported_symbols(&sema.symtab, snap,
                                    decl->use_decl.imports,
                                    decl->use_decl.import_count);

            // Restore error source to main file after module compilation
            errors_set_source(source, input_file);
            if (module_count >= MAX_MODULES) {
                fprintf(stderr, "mix: too many modules (max %d)\n", MAX_MODULES);
                return 1;
            }
            module_asm_files[module_count++] = asm_file;
        }
    }

    // Analyze main file (modules already registered their pub symbols)
    TIMER_START(sema);
    sema_analyze(&sema, program);
    TIMER_END(sema);

    if (mix_error_count() > 0) {
        fprintf(stderr, "mix: %d error(s) during analysis\n", mix_error_count());
        arena_destroy(&arena); free(source); free(lexer.tokens);
        return 1;
    }

    // Emit code for main file
    char gen_path[256];  // generated .ssa, .c, or .ll
    char asm_path[256];  // .s from QBE, or .o from clang -c (LLVM)

    if (use_c_backend) {
        // C backend: emit .c file
        if (emit_ir_only) {
            snprintf(gen_path, sizeof(gen_path), "%s", output_file);
        } else {
            snprintf(gen_path, sizeof(gen_path), "/tmp/mix_%d.c", getpid());
        }
        FILE *c_out = fopen(gen_path, "w");
        if (!c_out) {
            fprintf(stderr, "mix: cannot create '%s': %s\n", gen_path, strerror(errno));
            return 1;
        }
        CEmitter c_emitter = c_emitter_create(c_out, &arena, &sema.symtab);
        TIMER_START(emit);
        c_emit_program(&c_emitter, program);
        TIMER_END(emit);
        fclose(c_out);
    } else if (use_llvm_backend) {
        // LLVM backend (Phase 2): AST → lower → LIR → llvm_emit → .ll
        if (emit_ir_only) {
            snprintf(gen_path, sizeof(gen_path), "%s", output_file);
        } else {
            snprintf(gen_path, sizeof(gen_path), "/tmp/mix_%d.ll", getpid());
        }
        FILE *ll_out = fopen(gen_path, "w");
        if (!ll_out) {
            fprintf(stderr, "mix: cannot create '%s': %s\n", gen_path, strerror(errno));
            return 1;
        }
        TIMER_START(emit);
        LirModule *lmod = lower_program(program, &arena, &sema.symtab);
        LlvmEmitter le = llvm_emitter_create(ll_out);
        if (lmod) llvm_emit_module(&le, lmod);
        TIMER_END(emit);
        fclose(ll_out);
    } else {
        // QBE backend: emit .ssa file
        if (emit_ir_only) {
            snprintf(gen_path, sizeof(gen_path), "%s", output_file);
        } else {
            snprintf(gen_path, sizeof(gen_path), "/tmp/mix_%d.ssa", getpid());
        }
        FILE *ssa_out = fopen(gen_path, "w");
        if (!ssa_out) {
            fprintf(stderr, "mix: cannot create '%s': %s\n", gen_path, strerror(errno));
            return 1;
        }
        QbeEmitter emitter = qbe_emitter_create(ssa_out, &arena, &sema.symtab, debug_mode);
        TIMER_START(emit);
        qbe_emit_program(&emitter, program);
        TIMER_END(emit);
        fclose(ssa_out);
    }

    if (mix_error_count() > 0) {
        fprintf(stderr, "mix: %d error(s) during code generation\n", mix_error_count());
        arena_destroy(&arena); free(source); free(lexer.tokens);
        return 1;
    }

    if (emit_ir_only) {
        if (verbose) fprintf(stderr, "mix: wrote %s\n", gen_path);
        arena_destroy(&arena); free(source); free(lexer.tokens);
        return 0;
    }

    // QBE backend: compile .ssa -> .s
    if (!use_c_backend && !use_llvm_backend) {
        snprintf(asm_path, sizeof(asm_path), "/tmp/mix_%d.s", getpid());
        const char *qbe_argv[] = {"qbe", "-o", asm_path, gen_path, NULL};
        if (verbose) { fprintf(stderr, "mix: "); print_argv(qbe_argv); }
        TIMER_START(qbe);
        char *qbe_stderr = NULL;
        int ret = run_process(qbe_argv, verbose ? NULL : &qbe_stderr);
        TIMER_END(qbe);
        if (ret != 0) {
            if (verbose) {
                fprintf(stderr, "mix: qbe failed (exit %d)\n", ret);
            } else {
                fprintf(stderr, "mix: internal compiler error while compiling '%s'. Run with -v for details.\n", input_file);
            }
            free(qbe_stderr);
            return 1;
        }
        free(qbe_stderr);
    }

    // LLVM backend: compile .ll -> .o via clang.
    // The resulting .o slots into the same place QBE's .s slots into for
    // the final cc link step below.
    if (use_llvm_backend) {
        snprintf(asm_path, sizeof(asm_path), "/tmp/mix_%d.o", getpid());
        const char *clang_argv[] = {"clang", "-c", "-o", asm_path, gen_path, NULL};
        if (verbose) { fprintf(stderr, "mix: "); print_argv(clang_argv); }
        TIMER_START(qbe);
        char *clang_stderr = NULL;
        int ret = run_process(clang_argv, verbose ? NULL : &clang_stderr);
        TIMER_END(qbe);
        if (ret != 0) {
            if (verbose) {
                fprintf(stderr, "mix: clang -c failed (exit %d)\n", ret);
            } else {
                fprintf(stderr, "mix: internal compiler error while compiling '%s'. Run with -v for details.\n", input_file);
            }
            free(clang_stderr);
            return 1;
        }
        free(clang_stderr);
    }

    // Find runtime
    char runtime_beside_exe[1100] = "";
    if (exe_dir[0]) {
        snprintf(runtime_beside_exe, sizeof(runtime_beside_exe),
                 "%s/../lib/runtime.c", exe_dir);
    }
    // Prefer pre-compiled runtime.o (built once at `make` time). Falls back
    // to runtime.c when the .o is missing or stale, or when --debug is on
    // (debug builds want the runtime's source to live alongside user code
    // for stepping). The .o saves ~300ms per compile vs rebuilding the
    // runtime from scratch every time.
    char runtime_o_beside_exe[1100] = "";
    if (exe_dir[0]) {
        snprintf(runtime_o_beside_exe, sizeof(runtime_o_beside_exe),
                 "%s/../lib/runtime.o", exe_dir);
    }
    const char *runtime_o_paths[] = {
        "build/runtime.o",
        "../build/runtime.o",
        runtime_o_beside_exe[0] ? runtime_o_beside_exe : NULL,
        NULL
    };
    const char *runtime_paths[] = {
        "lib/runtime.c",
        "../lib/runtime.c",
        runtime_beside_exe[0] ? runtime_beside_exe : NULL,
        NULL
    };
    const char *runtime_path = NULL;
    bool runtime_is_object = false;
    if (!debug_mode) {
        for (int i = 0; runtime_o_paths[i]; i++) {
            struct stat ost, src_st;
            if (stat(runtime_o_paths[i], &ost) != 0) continue;
            // Make sure the .o is at least as new as runtime.c — otherwise
            // a fresh edit could be silently linked against stale code.
            // Only checked if a sibling .c file exists.
            char rt_c[1200];
            snprintf(rt_c, sizeof(rt_c), "%s", runtime_o_paths[i]);
            char *dot = strrchr(rt_c, '.');
            if (dot) { strcpy(dot, ".c"); }
            if (stat(rt_c, &src_st) == 0 && src_st.st_mtime > ost.st_mtime) continue;
            // Also try the lib/runtime.c sibling for the build/runtime.o
            // case where they're not in the same dir.
            const char *src_alt[] = {"lib/runtime.c", "../lib/runtime.c", NULL};
            bool stale = false;
            for (int j = 0; src_alt[j]; j++) {
                if (stat(src_alt[j], &src_st) == 0 && src_st.st_mtime > ost.st_mtime) {
                    stale = true; break;
                }
            }
            if (stale) continue;
            runtime_path = runtime_o_paths[i];
            runtime_is_object = true;
            break;
        }
    }
    if (!runtime_path) {
        for (int i = 0; runtime_paths[i]; i++) {
            FILE *test = fopen(runtime_paths[i], "r");
            if (test) { fclose(test); runtime_path = runtime_paths[i]; break; }
        }
    }
    (void)runtime_is_object;

    // Link — build argv array to avoid shell injection
    // Max entries: cc + flags(2) + main_file + modules(64) + runtime + -o + output + -lm
    //              + link_flags(64) + LDFLAGS tokens(64) + NULL
    #define MAX_LINK_ARGV 256
    const char *link_argv[MAX_LINK_ARGV];
    int ai = 0;

    link_argv[ai++] = "cc";
    if (debug_mode) link_argv[ai++] = "-g";
    if (!debug_mode) link_argv[ai++] = "-O2";
    link_argv[ai++] = use_c_backend ? gen_path : asm_path;
    for (int i = 0; i < module_count && ai < MAX_LINK_ARGV - 8; i++)
        link_argv[ai++] = module_asm_files[i];
    // Add source files (e.g. glad.c)
    for (int i = 0; i < source_file_count && ai < MAX_LINK_ARGV - 8; i++)
        link_argv[ai++] = source_files[i];
    if (runtime_path)
        link_argv[ai++] = runtime_path;
    link_argv[ai++] = "-o";
    link_argv[ai++] = output_file;
    link_argv[ai++] = "-lm";
#ifdef __APPLE__
    {
        char *sdk_libdir = detect_macos_sdk_libdir(&arena);
        if (sdk_libdir && ai < MAX_LINK_ARGV - 2) {
            char *lflag = arena_alloc(&arena, strlen(sdk_libdir) + 3);
            sprintf(lflag, "-L%s", sdk_libdir);
            link_argv[ai++] = lflag;
        }
    }
#endif
    for (int i = 0; i < link_flag_count && ai < MAX_LINK_ARGV - 2; i++)
        link_argv[ai++] = link_flags[i];
    // Add vendor -I directories
    for (int i = 0; i < vendor_idir_count && ai < MAX_LINK_ARGV - 2; i++) {
        char *iflag = arena_alloc(&arena, strlen(vendor_idirs[i]) + 3);
        sprintf(iflag, "-I%s", vendor_idirs[i]);
        link_argv[ai++] = iflag;
    }

    // CPPFLAGS -I flags: pass to linker so source files can find includes
    const char *env_cppflags = getenv("CPPFLAGS");
    if (env_cppflags && env_cppflags[0]) {
        char *cppbuf = strdup(env_cppflags);
        char *tok = strtok(cppbuf, " \t");
        while (tok && ai < MAX_LINK_ARGV - 1) {
            if (strncmp(tok, "-I", 2) == 0) {
                link_argv[ai++] = arena_strdup(&arena, tok);
            } else if (strcmp(tok, "-I") == 0) {
                tok = strtok(NULL, " \t");
                if (tok && ai < MAX_LINK_ARGV - 2) {
                    char *iflag = arena_alloc(&arena, strlen(tok) + 3);
                    sprintf(iflag, "-I%s", tok);
                    link_argv[ai++] = iflag;
                }
            }
            tok = strtok(NULL, " \t");
        }
        free(cppbuf);
    }

    // LDFLAGS: split by whitespace since execvp doesn't use a shell.
    // Tokenize a mutable copy so each flag becomes a separate argv entry.
    char *ldflags_buf = NULL;
    const char *env_ldflags = getenv("LDFLAGS");
    if (env_ldflags && env_ldflags[0]) {
        ldflags_buf = strdup(env_ldflags);
        char *tok = strtok(ldflags_buf, " \t");
        while (tok && ai < MAX_LINK_ARGV - 1) {
            link_argv[ai++] = tok;
            tok = strtok(NULL, " \t");
        }
    }

    link_argv[ai] = NULL;
    #undef MAX_LINK_ARGV

    if (verbose) { fprintf(stderr, "mix: "); print_argv(link_argv); }

    char *link_stderr = NULL;
    TIMER_START(cc);
    int ret = run_process(link_argv, verbose ? NULL : &link_stderr);
    TIMER_END(cc);
    free(ldflags_buf);
    if (ret != 0) {
        if (verbose) {
            fprintf(stderr, "mix: linker failed (exit %d)\n", ret);
        } else {
            // Try to translate common linker errors
            if (link_stderr && (strstr(link_stderr, "undefined reference") ||
                                strstr(link_stderr, "undefined symbol") ||
                                strstr(link_stderr, "Undefined symbols"))) {
                fprintf(stderr, "mix: linking failed: undefined symbol. Check that all functions and external libraries are available.\n");
            } else if (link_stderr && (strstr(link_stderr, "library not found") ||
                                       strstr(link_stderr, "cannot find -l"))) {
                fprintf(stderr, "mix: linking failed: library not found. Check your -l flags.\n");
            } else {
                fprintf(stderr, "mix: linking failed. Run with -v for details.\n");
            }
        }
        free(link_stderr);
        return 1;
    }
    free(link_stderr);

    // Cleanup
    if (!verbose) {
        remove(gen_path);
        if (!use_c_backend) remove(asm_path);
        for (int i = 0; i < module_count; i++)
            remove(module_asm_files[i]);
    }

    if (verbose) fprintf(stderr, "mix: wrote %s\n", output_file);

    arena_destroy(&arena);
    free(source);
    free(lexer.tokens);

    // In run mode, execute the compiled binary
    if (mode == MODE_RUN) {
        // Build the path — if output_file is a bare name, prefix with ./
        char run_path[512];
        if (output_file[0] == '/' || (output_file[0] == '.' && output_file[1] == '/')) {
            snprintf(run_path, sizeof(run_path), "%s", output_file);
        } else {
            snprintf(run_path, sizeof(run_path), "./%s", output_file);
        }
        const char *run_argv[] = {run_path, NULL};
        return run_process(run_argv, NULL);
    }

    return 0;
}
