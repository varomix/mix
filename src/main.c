#include "mix.h"
#include "arena.h"
#include "errors.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "sema.h"
#include "qbe_emit.h"
#include "c_emit.h"
#include "cbind.h"
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
    MODE_BUILD,
    MODE_RUN,
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
    fprintf(stderr, "  build [file.mix]    Compile to binary (default)\n");
    fprintf(stderr, "  run [file.mix]      Compile and execute\n");
    fprintf(stderr, "  run -f <file.mix>   Compile and run specific file\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>           Output file (default: derived from input)\n");
    fprintf(stderr, "  --emit-ir           Output QBE IR only\n");
    fprintf(stderr, "  --emit-tokens       Print token stream\n");
    fprintf(stderr, "  --emit-ast          Print AST\n");
    fprintf(stderr, "  --debug             Enable debug mode (DWARF info + @debug)\n");
    fprintf(stderr, "  --bind <path>       Generate .mix bindings from C header(s)\n");
    fprintf(stderr, "  --backend <name>    Backend: qbe (default), c\n");
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

// Resolve module path: "math" -> "./math.mix", "engine.physics" -> "./engine/physics.mix"
// For "std.*" modules, search relative to the compiler binary first.
static char *resolve_module_path(Arena *arena, const char *base_dir,
                                  const char *module_path, const char *exe_dir) {
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
static const char *compiling_modules[MAX_COMPILING_MODULES];
static int compiling_module_count = 0;

static bool is_module_compiling(const char *path) {
    for (int i = 0; i < compiling_module_count; i++) {
        if (strcmp(compiling_modules[i], path) == 0) return true;
    }
    return false;
}

// Compile a single module, return its output file path (or NULL on failure).
// Recursively compiles any modules this module imports via 'use'.
// For QBE backend: returns .s file path
// For C backend: returns .c file path
static char *compile_module(const char *source_path, Arena *arena, Sema *sema,
                            const char *base_dir, bool verbose,
                            bool debug, bool use_c_backend,
                            const char *exe_dir,
                            char **module_asm_files, int *module_count,
                            int max_modules) {
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
        if (decl->kind == NODE_USE_DECL) {
            char *sub_path = resolve_module_path(arena, base_dir,
                                                  decl->use_decl.module_path, exe_dir);
            if (verbose) fprintf(stderr, "mix: compiling sub-module '%s' -> %s\n",
                                 decl->use_decl.module_path, sub_path);

            char *sub_asm = compile_module(sub_path, arena, sema, base_dir,
                                           verbose, debug, use_c_backend,
                                           exe_dir, module_asm_files,
                                           module_count, max_modules);
            if (!sub_asm) {
                fprintf(stderr, "mix: failed to compile module '%s'\n",
                        decl->use_decl.module_path);
                free(source); free(lexer.tokens); compiling_module_count--;
                return NULL;
            }
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
    const char *backend = "qbe";  /* "qbe" or "c" */
    RunMode mode = MODE_BUILD;
    bool output_set = false;

    #define MAX_LINK_FLAGS 64
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
        }
        // Otherwise it's a filename — legacy mode, arg_start stays 1
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
    } else if (strncmp(argv[i], "-l", 2) == 0 || strncmp(argv[i], "-L", 2) == 0 ||
                   strncmp(argv[i], "-framework", 10) == 0) {
            if (link_flag_count >= MAX_LINK_FLAGS) {
                fprintf(stderr, "mix: too many linker flags (max %d)\n", MAX_LINK_FLAGS);
                return 1;
            }
            link_flags[link_flag_count++] = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "mix: unknown option '%s'\n", argv[i]);
            usage();
        } else {
            input_file = argv[i];
        }
    }

    // Binding mode: generate .mix from C headers
    if (bind_path) {
        if (!output_file) output_file = "a.out";
        return cbind_generate(bind_path, output_file, bind_lib, verbose);
    }

    // Check MIX_BACKEND env var
    {
        const char *env_backend = getenv("MIX_BACKEND");
        if (env_backend && env_backend[0]) backend = env_backend;
    }
    bool use_c_backend = (strcmp(backend, "c") == 0);

    // Auto-discover if no input file specified
    if (!input_file) {
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

    // Lex
    Lexer lexer = lexer_create(source, input_file, &arena);
    lexer_tokenize(&lexer);

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
    Parser parser = parser_create(lexer.tokens, lexer.token_count, &arena, input_file);
    AstNode *program = parser_parse(&parser);

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
        if (decl->kind == NODE_USE_DECL) {
            char *mod_path = resolve_module_path(&arena, base_dir,
                                                  decl->use_decl.module_path, exe_dir);
            if (verbose) fprintf(stderr, "mix: compiling module '%s' -> %s\n",
                                 decl->use_decl.module_path, mod_path);

            char *asm_file = compile_module(mod_path, &arena, &sema, base_dir,
                                            verbose, debug_mode, use_c_backend,
                                            exe_dir, module_asm_files,
                                            &module_count, MAX_MODULES);
            if (!asm_file) {
                fprintf(stderr, "mix: failed to compile module '%s'\n", decl->use_decl.module_path);
                return 1;
            }
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
    sema_analyze(&sema, program);

    if (mix_error_count() > 0) {
        fprintf(stderr, "mix: %d error(s) during analysis\n", mix_error_count());
        arena_destroy(&arena); free(source); free(lexer.tokens);
        return 1;
    }

    // Emit code for main file
    char gen_path[256];  // generated .ssa or .c
    char asm_path[256];  // .s from QBE (only for qbe backend)

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
        c_emit_program(&c_emitter, program);
        fclose(c_out);
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
        qbe_emit_program(&emitter, program);
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
    if (!use_c_backend) {
        snprintf(asm_path, sizeof(asm_path), "/tmp/mix_%d.s", getpid());
        const char *qbe_argv[] = {"qbe", "-o", asm_path, gen_path, NULL};
        if (verbose) { fprintf(stderr, "mix: "); print_argv(qbe_argv); }
        char *qbe_stderr = NULL;
        int ret = run_process(qbe_argv, verbose ? NULL : &qbe_stderr);
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

    // Find runtime
    char runtime_beside_exe[1100] = "";
    if (exe_dir[0]) {
        snprintf(runtime_beside_exe, sizeof(runtime_beside_exe),
                 "%s/../lib/runtime.c", exe_dir);
    }
    const char *runtime_paths[] = {
        "lib/runtime.c",
        "../lib/runtime.c",
        runtime_beside_exe[0] ? runtime_beside_exe : NULL,
        NULL
    };
    const char *runtime_path = NULL;
    for (int i = 0; runtime_paths[i]; i++) {
        FILE *test = fopen(runtime_paths[i], "r");
        if (test) { fclose(test); runtime_path = runtime_paths[i]; break; }
    }

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
    if (runtime_path)
        link_argv[ai++] = runtime_path;
    link_argv[ai++] = "-o";
    link_argv[ai++] = output_file;
    link_argv[ai++] = "-lm";
    for (int i = 0; i < link_flag_count && ai < MAX_LINK_ARGV - 2; i++)
        link_argv[ai++] = link_flags[i];

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
    int ret = run_process(link_argv, verbose ? NULL : &link_stderr);
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
