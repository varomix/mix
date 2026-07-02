#include "mix.h"
#include "arena.h"
#include "errors.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "sema.h"
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

typedef enum {
    MODE_NONE,
    MODE_BUILD,
    MODE_RUN,
    MODE_FMT,
} RunMode;

static void cli_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "mix: error: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void cli_help(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "mix: help: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void compile_phase_failed(const char *phase) {
    fprintf(stderr, "mix: %d error(s) during %s\n", mix_error_count(), phase);
    cli_help("fix the first error above, then run the same command again");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        cli_error("cannot open '%s': %s", path, strerror(errno));
        cli_help("check that the path exists and that you have permission to read it");
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); return NULL; }
    long size = st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) {
        fprintf(stderr, "mix: out of memory\n");
        fprintf(stderr, "mix: help: free memory and try again, or reduce input size\n");
        exit(1);
    }
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

#ifdef __APPLE__
static char *detect_macos_sdk_libdir(void) {
    static char cached_libdir[1200];
    static bool cached = false;
    if (cached) return cached_libdir[0] ? cached_libdir : NULL;

    cached = true;
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

    strcpy(cached_libdir, libdir);
    return cached_libdir;
}
#endif

// Detect the WASI-capable clang for --target wasm32.
// Checks $WASI_CLANG env var first, then brew-installed Emscripten LLVM.
// Returns NULL if no suitable clang is found.
static const char *detect_wasi_clang(void) {
    const char *env = getenv("WASI_CLANG");
    if (env) return env;
    struct stat st;
#if defined(__APPLE__)
    const char *candidates[] = {
        "/opt/homebrew/Cellar/emscripten/6.0.1/libexec/llvm/bin/clang",
        "/opt/homebrew/bin/wasm32-wasi-clang",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (stat(candidates[i], &st) == 0 && S_ISREG(st.st_mode)) return candidates[i];
    }
#endif
    (void)st;
    return NULL;
}

// Detect the Emscripten compiler (emcc) for --target wasm-browser.
// Checks $EMCC env var first, then common install locations + PATH.
// Returns a pointer to a static buffer, or NULL.
static const char *detect_emcc(void) {
    const char *env = getenv("EMCC");
    if (env) return env;
    static const char *candidates[] = {
        "/opt/homebrew/bin/emcc",
        "/usr/local/bin/emcc",
        "emcc",     // rely on PATH lookup at exec time
        NULL
    };
    static char found[1024];
    static bool cached = false;
    if (cached) return found[0] ? found : NULL;
    cached = true;
    struct stat st;
    for (int i = 0; candidates[i]; i++) {
        if (candidates[i][0] == '/') {
            if (stat(candidates[i], &st) == 0 && S_ISREG(st.st_mode)) {
                strcpy(found, candidates[i]);
                return found;
            }
        } else {
            // Check PATH for bare name
            const char *path = getenv("PATH");
            if (!path) continue;
            char *path_copy = strdup(path);
            if (!path_copy) continue;
            char *dir = strtok(path_copy, ":");
            while (dir) {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", dir, candidates[i]);
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
                    strcpy(found, full);
                    free(path_copy);
                    return found;
                }
                dir = strtok(NULL, ":");
            }
            free(path_copy);
        }
    }
    return NULL;
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
            cli_error("could not capture tool output: %s", strerror(errno));
            cli_help("free file descriptors or run with -v to avoid stderr capture");
            return -1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        cli_error("could not start child process: %s", strerror(errno));
        cli_help("check process limits and available system resources, then try again");
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
        cli_error("could not run '%s': %s", argv[0], strerror(errno));
        cli_help("install the tool or make sure it is on PATH");
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
        cli_error("could not wait for child process: %s", strerror(errno));
        cli_help("try again; if this repeats, run with -v and report the command");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// Like run_process but writes data to child's stdin.
// stdin_data may be NULL (behaves identically to run_process).
static int run_process_stdin(const char *const argv[], const char *stdin_data, size_t stdin_len, char **captured_stderr) {
    int stdin_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (stdin_data) {
        if (pipe(stdin_pipe) < 0) {
            cli_error("could not create compiler input pipe: %s", strerror(errno));
            cli_help("free file descriptors or disk resources, then try again");
            return -1;
        }
    }
    if (captured_stderr) {
        *captured_stderr = NULL;
        if (pipe(stderr_pipe) < 0) {
            cli_error("could not capture tool output: %s", strerror(errno));
            cli_help("free file descriptors or run with -v to avoid stderr capture");
            if (stdin_data) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
            return -1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        cli_error("could not start child process: %s", strerror(errno));
        cli_help("check process limits and available system resources, then try again");
        if (stdin_data) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        if (captured_stderr) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
        return -1;
    }
    if (pid == 0) {
        if (stdin_data) {
            close(stdin_pipe[1]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
        }
        if (captured_stderr) {
            close(stderr_pipe[0]);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        }
        execvp(argv[0], (char *const *)argv);
        cli_error("could not run '%s': %s", argv[0], strerror(errno));
        cli_help("install the tool or make sure it is on PATH");
        _exit(127);
    }

    // Parent
    if (stdin_data) {
        close(stdin_pipe[0]);
        size_t written = 0;
        while (written < stdin_len) {
            ssize_t n = write(stdin_pipe[1], stdin_data + written, stdin_len - written);
            if (n < 0) break;
            written += n;
        }
        close(stdin_pipe[1]);
    }

    if (captured_stderr) {
        close(stderr_pipe[1]);
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        if (!buf) { close(stderr_pipe[0]); *captured_stderr = NULL; }
        else {
            ssize_t n;
            while ((n = read(stderr_pipe[0], buf + len, cap - len - 1)) > 0) {
                len += n;
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *newbuf = realloc(buf, cap);
                    if (!newbuf) break;
                    buf = newbuf;
                }
            }
            buf[len] = '\0';
            close(stderr_pipe[0]);
            *captured_stderr = buf;
        }
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        cli_error("could not wait for child process: %s", strerror(errno));
        cli_help("try again; if this repeats, run with -v and report the command");
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
    fprintf(stderr, "  --emit-ir           Output backend IR only (.ll for llvm, .c for c)\n");
    fprintf(stderr, "  --emit-tokens       Print token stream\n");
    fprintf(stderr, "  --emit-ast          Print AST\n");
    fprintf(stderr, "  --debug             Enable debug mode (DWARF info + @debug)\n");
    fprintf(stderr, "  -O<level>           Set optimization level (e.g., -O2, -Os)\n");
    fprintf(stderr, "  --bind <path>       Generate .mix bindings from C header(s)\n");
    fprintf(stderr, "  --backend <name>    Backend: llvm (default), c (fallback)\n");
    fprintf(stderr, "  --target <arch>     Target architecture: native (default), wasm32, wasm-browser\n");
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
        cli_error("cannot open current directory: %s", strerror(errno));
        cli_help("run mix from a readable project directory or pass a specific .mix file");
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
        cli_error("no .mix file with main() found in current directory");
        cli_help("pass the file explicitly, for example `mix build path/to/app.mix`, or add `main()` to one .mix file here");
        return NULL;
    }
    if (match_count > 1) {
        cli_error("multiple .mix files with main() found");
        cli_help("choose one explicitly, for example `mix build app.mix`");
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
        cli_error("fmt cannot open '%s': %s", dir, strerror(errno));
        cli_help("check that the path exists and is readable");
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
        cli_error("fmt failed to format '%s'", path);
        cli_help("fix the syntax error reported above, then run fmt again");
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
                cli_error("fmt cannot create '%s': %s", tmp, strerror(errno));
                cli_help("check write permissions and free space in the target directory");
                rc = 1;
            } else {
                fputs(out, f);
                fclose(f);
                if (rename(tmp, path) != 0) {
                    cli_error("fmt could not replace '%s': %s", path, strerror(errno));
                    cli_help("check write permissions and whether another process is using the file");
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
            cli_error("fmt failed to format stdin");
            cli_help("fix the syntax error reported above, then run fmt again");
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
            cli_error("fmt cannot stat '%s': %s", paths[i], strerror(errno));
            cli_help("check that the path exists and is accessible");
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

    // If the resolved path does not exist, try <base_dir>/lib/<module_path>/<module_path>.mix
    // then <exe_dir>/../lib/<module_path>/<module_path>.mix
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return arena_strdup(arena, path); }

    char lib_path[512];
    snprintf(lib_path, sizeof(lib_path), "%s/lib/%s/%s.mix", base_dir, module_path, module_path);
    f = fopen(lib_path, "r");
    if (f) { fclose(f); return arena_strdup(arena, lib_path); }

    if (exe_dir && exe_dir[0]) {
        snprintf(lib_path, sizeof(lib_path), "%s/../lib/%s/%s.mix", exe_dir, module_path, module_path);
        f = fopen(lib_path, "r");
        if (f) { fclose(f); return arena_strdup(arena, lib_path); }
    }

    // Return the original path anyway — let the caller report the error
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
// For C backend:    returns .c file path
// For LLVM backend: returns .o file path (text IR + clang -c)
static char *compile_module(const char *source_path, Arena *arena, Sema *sema,
                            const char *base_dir, bool verbose,
                            bool debug, bool use_c_backend, bool use_llvm_backend,
                            const char *exe_dir,
                            char **module_asm_files, int *module_count,
                            int max_modules,
                            char **link_flags, int *link_flag_count,
                            bool is_wasm_browser) {
    // Circular import check
    if (is_module_compiling(source_path)) {
        cli_error("circular import detected: '%s'", source_path);
        cli_help("remove one `use` from the cycle or move shared declarations into a third module");
        return NULL;
    }
    if (compiling_module_count >= MAX_COMPILING_MODULES) {
        cli_error("too many nested imports (max %d)", MAX_COMPILING_MODULES);
        cli_help("simplify the import chain or raise MAX_COMPILING_MODULES in the compiler");
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
        free(source); 
compiling_module_count--;
        return NULL;
    }

    // Parse
    Parser parser = parser_create(lexer.tokens, lexer.token_count, arena, source_path);
    AstNode *program = parser_parse(&parser);
    if (mix_error_count() > 0) {
        free(source); 
compiling_module_count--;
        return NULL;
    }

    // Recursively compile any modules this module imports
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_USE_C_DECL) {
            // use c "header.h" in a module — generate bindings and analyze
            char *bind_src = cbind_generate_string(decl->use_c_decl.header_path,
                                                     decl->use_c_decl.lib_name,
                                                     verbose, NULL);
            if (!bind_src) {
                cli_error("failed to generate bindings for '%s'",
                          decl->use_c_decl.header_path);
                cli_help("check that the header path is correct; use -v to see the C binding command");
                free(source); 
compiling_module_count--;
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
            // bl.tokens is arena-allocated, no free needed

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

            // Propagate `frameworks "Cocoa,IOKit"` to the root linker line.
            const char *fw = decl->use_c_decl.frameworks;
            if (fw && link_flags && link_flag_count) {
                char fw_copy[256];
                strncpy(fw_copy, fw, sizeof(fw_copy) - 1);
                fw_copy[sizeof(fw_copy) - 1] = '\0';
                char *save;
                char *tok = strtok_r(fw_copy, ",", &save);
                while (tok) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    char *end = tok + strlen(tok) - 1;
                    while (end > tok && (*end == ' ' || *end == '\t')) end--;
                    *(end + 1) = '\0';
                    if (*tok && *link_flag_count + 2 < MAX_LINK_FLAGS) {
                        link_flags[(*link_flag_count)++] = "-framework";
                        link_flags[(*link_flag_count)++] = arena_strdup(arena, tok);
                    }
                    tok = strtok_r(NULL, ",", &save);
                }
            }
        } else if (decl->kind == NODE_EXTERN_BLOCK) {
            const char *lib = decl->extern_block.lib_name;
            if (lib && strcmp(lib, "C") != 0 && link_flags && link_flag_count
                && *link_flag_count < MAX_LINK_FLAGS) {
                char dash_l[256];
                snprintf(dash_l, sizeof(dash_l), "-l%s", lib);
                bool already = false;
                for (int li = 0; li < *link_flag_count; li++) {
                    if (strcmp(link_flags[li], dash_l) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
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
                                           link_flags, link_flag_count,
                                           is_wasm_browser);
            if (!sub_asm) {
                cli_error("failed to compile module '%s'",
                          decl->use_decl.module_path);
                cli_help("fix the module error above or check that the `use` path resolves to the intended .mix file");
                free(source); 
compiling_module_count--;
                return NULL;
            }
            // Apply selective imports if specified
            filter_imported_symbols(&sema->symtab, snap,
                                    decl->use_decl.imports,
                                    decl->use_decl.import_count);

            // Restore error source to this module after sub-module compilation
            errors_set_source(source, source_path);

            if (*module_count >= max_modules) {
                cli_error("too many modules (max %d)", max_modules);
                cli_help("reduce the import graph or raise the module limit in the compiler");
                free(source); 
compiling_module_count--;
                return NULL;
            }
            module_asm_files[(*module_count)++] = sub_asm;
        }
    }

    // Analyze (uses shared sema so pub symbols are visible to main)
    sema_analyze(sema, program);
    if (mix_error_count() > 0) {
        free(source); 
compiling_module_count--;
        return NULL;
    }

    int mod_id = *module_count;  // unique ID for temp file naming

    if (use_c_backend) {
        // C backend: emit .c file directly
        char c_path[256];
        snprintf(c_path, sizeof(c_path), "/tmp/mod_%d_%d.c", getpid(), mod_id);
        FILE *c_out = fopen(c_path, "w");
        if (!c_out) {
            cli_error("cannot create temporary C output '%s': %s", c_path, strerror(errno));
            cli_help("check that /tmp is writable and has free space");
            free(source); 
compiling_module_count--;
            return NULL;
        }
        CEmitter c_emitter = c_emitter_create(c_out, arena, &sema->symtab);
        c_emit_program(&c_emitter, program);
        fclose(c_out);
        free(source);
        
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
            cli_error("cannot create temporary LLVM output '%s': %s", ll_path, strerror(errno));
            cli_help("check that /tmp is writable and has free space");
            free(source); 
compiling_module_count--;
            return NULL;
        }
        LirModule *lmod = lower_program(program, arena, &sema->symtab);
        if (!lmod || mix_error_count() > 0) {
            fclose(ll_out);
            free(source); 
compiling_module_count--;
            return NULL;
        }
        LlvmEmitter le = llvm_emitter_create(ll_out);
        if (debug) llvm_emitter_enable_debug(&le, source_path);
        llvm_emit_module(&le, lmod);
        fclose(ll_out);

        const char *clang_argv[12];
        int cai = 0;
        if (is_wasm_browser) {
            const char *emcc = detect_emcc();
            clang_argv[cai++] = emcc ? emcc : "emcc";
            clang_argv[cai++] = "-c";
            clang_argv[cai++] = "-Wno-override-module";
        } else {
            clang_argv[cai++] = "clang";
            clang_argv[cai++] = "-c";
            clang_argv[cai++] = "-Wno-override-module";
        }
        if (debug) clang_argv[cai++] = "-g";
        clang_argv[cai++] = "-o";
        clang_argv[cai++] = obj_path;
        clang_argv[cai++] = ll_path;
        clang_argv[cai++] = NULL;
        if (verbose) { fprintf(stderr, "mix: "); print_argv(clang_argv); }
        char *clang_stderr = NULL;
        int ret = run_process(clang_argv, verbose ? NULL : &clang_stderr);
        if (ret != 0) {
            if (verbose) {
                const char *tool = is_wasm_browser ? "emcc" : "clang";
                cli_error("%s -c failed for module '%s'", tool, source_path);
                cli_help("read the tool output above, then fix the generated IR path with -v if needed");
            } else {
                cli_error("internal compiler error while compiling '%s'", source_path);
                cli_help("run the same command with -v to show the backend tool output");
            }
            free(clang_stderr);
            free(source); 
compiling_module_count--;
            return NULL;
        }
        free(clang_stderr);

        if (!verbose) remove(ll_path);
        free(source);
        
        compiling_module_count--;
        return arena_strdup(arena, obj_path);
    } else {
        // No remaining backend (QBE retired in Phase 9). Sema/lower
        // route through the C and LLVM branches above.
        cli_error("internal compiler error: unknown backend");
        cli_help("choose --backend llvm or --backend c");
        free(source); 
compiling_module_count--;
        return NULL;
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
    const char *opt_level = NULL;
    const char *target_arch = "native";
    const char *backend = "llvm";  /* "llvm" (default) or "c" */
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
        } else if (strncmp(argv[i], "-O", 2) == 0 && argv[i][2] != '\0') {
            opt_level = argv[i] + 2;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("mix %s (%s)\n", MIX_VERSION, MIX_VERSION_DATE);
            return 0;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_path = argv[++i];
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target_arch = argv[++i];
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
                cli_error("too many linker flags (max %d)", MAX_LINK_FLAGS);
                cli_help("remove duplicate -l/-L flags or raise MAX_LINK_FLAGS in the compiler");
                return 1;
            }
            link_flags[link_flag_count++] = argv[i];
        } else if (strcmp(argv[i], "-framework") == 0 && i + 1 < argc) {
            if (link_flag_count + 1 >= MAX_LINK_FLAGS) {
                cli_error("too many linker flags (max %d)", MAX_LINK_FLAGS);
                cli_help("remove duplicate framework flags or raise MAX_LINK_FLAGS in the compiler");
                return 1;
            }
            link_flags[link_flag_count++] = argv[i];
            link_flags[link_flag_count++] = argv[++i];
        } else if (argv[i][0] == '-') {
            cli_error("unknown option '%s'", argv[i]);
            cli_help("run `mix` with no arguments to see the supported commands and options");
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
    if (!use_c_backend && !use_llvm_backend) {
        cli_error("unknown --backend '%s'", backend);
        cli_help("choose `--backend llvm` or `--backend c`");
        return 1;
    }

    // Validate target arch
    bool is_wasm_target = (strcmp(target_arch, "wasm32") == 0);
    bool is_wasm_browser = (strcmp(target_arch, "wasm-browser") == 0);
    if (is_wasm_target) {
        if (!use_llvm_backend) {
            cli_error("--target wasm32 requires the LLVM backend");
            cli_help("remove `--backend c` or pass `--backend llvm`");
            return 1;
        }
        if (!detect_wasi_clang()) {
            cli_error("--target wasm32 requires a WASI-capable clang");
            cli_help("install wasi-libc + wasi-runtimes, or set WASI_CLANG=/path/to/clang");
            return 1;
        }
    }
    if (is_wasm_browser) {
        if (!use_llvm_backend) {
            cli_error("--target wasm-browser requires the LLVM backend");
            cli_help("remove `--backend c` or pass `--backend llvm`");
            return 1;
        }
        if (!detect_emcc()) {
            cli_error("--target wasm-browser requires emcc (Emscripten)");
            cli_help("install Emscripten or set EMCC=/path/to/emcc");
            return 1;
        }
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
        if (is_wasm_target) {
            strncat(derived_name, ".wasm", sizeof(derived_name) - strlen(derived_name) - 1);
        } else if (is_wasm_browser) {
            strncat(derived_name, ".html", sizeof(derived_name) - strlen(derived_name) - 1);
        }
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
        compile_phase_failed("lexing");
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
        arena_destroy(&arena); free(source); 
        return 0;
    }

    if (mix_error_count() > 0) {
        compile_phase_failed("parsing");
        arena_destroy(&arena); free(source); 
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

    // Vendor lib directories (from lib/vendor/*/lib/)
    #define MAX_VENDOR_LDIRS 16
    const char *vendor_ldirs[MAX_VENDOR_LDIRS];
    int vendor_ldir_count = 0;

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

    // Let cbind.c resolve vendor paths relative to the project root
    cbind_set_project_root(exe_dir);

    // Collect vendor -I and -L directories (from lib/vendor/*/include/ and lib/vendor/*/lib/)
    {
        // Scan CWD-relative lib/vendor/
        struct {
            const char *base;
            char idirs[MAX_VENDOR_IDIRS][512];
            int idir_count;
            char ldirs[MAX_VENDOR_LDIRS][512];
            int ldir_count;
        } v_scan = {0};

        DIR *vdir = opendir("lib/vendor");
        if (vdir) {
            struct dirent *ventry;
            while ((ventry = readdir(vdir)) != NULL) {
                if (ventry->d_name[0] == '.') continue;
                char inc_dir[512];
                snprintf(inc_dir, sizeof(inc_dir), "lib/vendor/%s/include", ventry->d_name);
                struct stat vst;
                if (stat(inc_dir, &vst) == 0 && S_ISDIR(vst.st_mode)) {
                    bool already = false;
                    for (int vi = 0; vi < v_scan.idir_count; vi++) {
                        if (strcmp(v_scan.idirs[vi], inc_dir) == 0) { already = true; break; }
                    }
                    if (!already && v_scan.idir_count < MAX_VENDOR_IDIRS) {
                        snprintf(v_scan.idirs[v_scan.idir_count++], 512, "%s", inc_dir);
                    }
                }
                char lib_dir[512];
                snprintf(lib_dir, sizeof(lib_dir), "lib/vendor/%s/lib", ventry->d_name);
                if (stat(lib_dir, &vst) == 0 && S_ISDIR(vst.st_mode)) {
                    bool already = false;
                    for (int li = 0; li < v_scan.ldir_count; li++) {
                        if (strcmp(v_scan.ldirs[li], lib_dir) == 0) { already = true; break; }
                    }
                    if (!already && v_scan.ldir_count < MAX_VENDOR_LDIRS) {
                        snprintf(v_scan.ldirs[v_scan.ldir_count++], 512, "%s", lib_dir);
                    }
                }
            }
            closedir(vdir);
        }

        // Also scan relative to project root (exe_dir/../lib/vendor/) so it
        // works when the compiler is run from a subdirectory.
        if (exe_dir[0]) {
            char exe_vendor[1024];
            snprintf(exe_vendor, sizeof(exe_vendor), "%s/../lib/vendor", exe_dir);
            DIR *edir = opendir(exe_vendor);
            if (edir) {
                struct dirent *ventry;
                while ((ventry = readdir(edir)) != NULL) {
                    if (ventry->d_name[0] == '.') continue;
                    char inc_dir[512];
                    snprintf(inc_dir, sizeof(inc_dir), "%s/%s/include", exe_vendor, ventry->d_name);
                    struct stat vst;
                    if (stat(inc_dir, &vst) == 0 && S_ISDIR(vst.st_mode)) {
                        bool already = false;
                        for (int vi = 0; vi < v_scan.idir_count; vi++) {
                            if (strcmp(v_scan.idirs[vi], inc_dir) == 0) { already = true; break; }
                        }
                        if (!already && v_scan.idir_count < MAX_VENDOR_IDIRS) {
                            snprintf(v_scan.idirs[v_scan.idir_count++], 512, "%s", inc_dir);
                        }
                    }
                    char lib_dir[512];
                    snprintf(lib_dir, sizeof(lib_dir), "%s/%s/lib", exe_vendor, ventry->d_name);
                    if (stat(lib_dir, &vst) == 0 && S_ISDIR(vst.st_mode)) {
                        bool already = false;
                        for (int li = 0; li < v_scan.ldir_count; li++) {
                            if (strcmp(v_scan.ldirs[li], lib_dir) == 0) { already = true; break; }
                        }
                        if (!already && v_scan.ldir_count < MAX_VENDOR_LDIRS) {
                            snprintf(v_scan.ldirs[v_scan.ldir_count++], 512, "%s", lib_dir);
                        }
                    }
                }
                closedir(edir);
            }
        }

        // Copy collected paths into the arena
        for (int vi = 0; vi < v_scan.idir_count; vi++) {
            vendor_idirs[vendor_idir_count++] = arena_strdup(&arena, v_scan.idirs[vi]);
        }
        for (int li = 0; li < v_scan.ldir_count; li++) {
            vendor_ldirs[vendor_ldir_count++] = arena_strdup(&arena, v_scan.ldirs[li]);
        }
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

            char *bind_src = cbind_generate_string(header, lib, verbose, base_dir);
            if (!bind_src) {
                cli_error("failed to generate bindings for '%s'", header);
                cli_help("check that the header path is correct; use -v to see the C binding command");
                return 1;
            }

            // Feed through lex -> parse -> sema
            errors_set_source(bind_src, header);
            Lexer bind_lex = lexer_create(bind_src, header, &arena);
            lexer_tokenize(&bind_lex);
            if (mix_error_count() > 0) {
                free(bind_src);
                cli_error("generated bindings for '%s' contain errors", header);
                cli_help("run with -v to inspect the generated binding step, then fix or narrow the C header");
                return 1;
            }
            Parser bind_parser = parser_create(bind_lex.tokens, bind_lex.token_count, &arena, header);
            AstNode *bind_prog = parser_parse(&bind_parser);
            if (mix_error_count() > 0) {
                free(bind_src);
                cli_error("generated bindings for '%s' contain parse errors", header);
                cli_help("run with -v to inspect the generated binding step, then fix or narrow the C header");
                return 1;
            }
            sema_analyze(&sema, bind_prog);
            if (mix_error_count() > 0) {
                free(bind_src);
                cli_error("generated bindings for '%s' contain type errors", header);
                cli_help("run with -v to inspect the generated binding step, then fix or narrow the C header");
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

            // Collect `frameworks "Cocoa,IOKit"` and emit -framework flags
            const char *fw = decl->use_c_decl.frameworks;
            if (fw && link_flag_count < MAX_LINK_FLAGS) {
                char fw_copy[256];
                strncpy(fw_copy, fw, sizeof(fw_copy) - 1);
                fw_copy[sizeof(fw_copy) - 1] = '\0';
                char *save;
                char *tok = strtok_r(fw_copy, ",", &save);
                while (tok) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    char *end = tok + strlen(tok) - 1;
                    while (end > tok && (*end == ' ' || *end == '\t')) end--;
                    *(end + 1) = '\0';
                    if (*tok && link_flag_count + 2 < MAX_LINK_FLAGS) {
                        link_flags[link_flag_count++] = "-framework";
                        link_flags[link_flag_count++] = arena_strdup(&arena, tok);
                    }
                    tok = strtok_r(NULL, ",", &save);
                }
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

                    const char *vendor_bases[4];
                    int vendor_base_count = 0;
                    vendor_bases[vendor_base_count++] = "lib/vendor";
                    if (exe_dir[0]) {
                        char exe_vendor[1024];
                        snprintf(exe_vendor, sizeof(exe_vendor), "%s/../lib/vendor", exe_dir);
                        vendor_bases[vendor_base_count++] = arena_strdup(&arena, exe_vendor);
                    }
                    for (int vb = 0; vb < vendor_base_count && !found_src; vb++) {
                        const char *vbase = vendor_bases[vb];
                        DIR *vdir = opendir(vbase);
                        if (!vdir) continue;
                        struct dirent *ventry;
                        while ((ventry = readdir(vdir)) != NULL) {
                            if (ventry->d_name[0] == '.') continue;
                            char try_paths[5][1024];
                            int try_count = 0;
                            // Try full path under vendor subdirs
                            snprintf(try_paths[try_count++], 1024,
                                     "%s/%s/src/%s", vbase, ventry->d_name, src_path);
                            snprintf(try_paths[try_count++], 1024,
                                     "%s/%s/%s", vbase, ventry->d_name, src_path);
                            // If first component matches vendor name, try rest only
                            if (src_rest && src_slash > src_path) {
                                int prefix_len = (int)(src_slash - src_path);
                                if ((int)strlen(ventry->d_name) == prefix_len &&
                                    strncmp(ventry->d_name, src_path, prefix_len) == 0) {
                                    snprintf(try_paths[try_count++], 1024,
                                             "%s/%s/src/%s", vbase, ventry->d_name, src_rest);
                                    snprintf(try_paths[try_count++], 1024,
                                             "%s/%s/%s", vbase, ventry->d_name, src_rest);
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
                        cli_error("source file '%s' not found", src_path);
                        cli_help("check the `source` path in the `use c` declaration or place the file under lib/vendor/<name>/src");
                        return 1;
                    }
                }
            }

            // (vendor dirs collected before the loop)
        } else if (decl->kind == NODE_EXTERN_BLOCK) {
            const char *lib = decl->extern_block.lib_name;
            if (lib && strcmp(lib, "C") != 0 && link_flag_count < MAX_LINK_FLAGS) {
                char *lflag = arena_alloc(&arena, strlen(lib) + 3);
                sprintf(lflag, "-l%s", lib);
                bool already = false;
                for (int li = 0; li < link_flag_count; li++) {
                    if (strcmp(link_flags[li], lflag) == 0) { already = true; break; }
                }
                if (!already) link_flags[link_flag_count++] = lflag;
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
                                            link_flags, &link_flag_count,
                                            is_wasm_browser);
            if (!asm_file) {
                cli_error("failed to compile module '%s'", decl->use_decl.module_path);
                cli_help("fix the module error above or check that the `use` path resolves to the intended .mix file");
                return 1;
            }
            // Apply selective imports if specified
            filter_imported_symbols(&sema.symtab, snap,
                                    decl->use_decl.imports,
                                    decl->use_decl.import_count);

            // Restore error source to main file after module compilation
            errors_set_source(source, input_file);
            if (module_count >= MAX_MODULES) {
                cli_error("too many modules (max %d)", MAX_MODULES);
                cli_help("reduce the import graph or raise MAX_MODULES in the compiler");
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
        compile_phase_failed("analysis");
        arena_destroy(&arena); free(source); 
        return 1;
    }

    // Emit code for main file
    char gen_path[256] = "";  // .c file (C backend) or empty (LLVM backend)
    char *lir_buf = NULL;     // LLVM IR buffer (LLVM backend, piped to clang)
    size_t lir_size = 0;

    if (use_c_backend) {
        // C backend: emit .c file
        if (emit_ir_only) {
            snprintf(gen_path, sizeof(gen_path), "%s", output_file);
        } else {
            snprintf(gen_path, sizeof(gen_path), "/tmp/mix_%d.c", getpid());
        }
        FILE *c_out = fopen(gen_path, "w");
        if (!c_out) {
            cli_error("cannot create '%s': %s", gen_path, strerror(errno));
            cli_help("choose a writable output path with -o or fix permissions for the target directory");
            return 1;
        }
        CEmitter c_emitter = c_emitter_create(c_out, &arena, &sema.symtab);
        TIMER_START(emit);
        c_emit_program(&c_emitter, program);
        TIMER_END(emit);
        fclose(c_out);
    } else if (use_llvm_backend) {
        // LLVM backend: AST → lower → LIR → llvm_emit to memory buffer
        if (emit_ir_only) {
            snprintf(gen_path, sizeof(gen_path), "%s", output_file);
            FILE *ll_out = fopen(gen_path, "w");
            if (!ll_out) {
                cli_error("cannot create '%s': %s", gen_path, strerror(errno));
                cli_help("choose a writable output path with -o or fix permissions for the target directory");
                return 1;
            }
            TIMER_START(emit);
            LirModule *lmod = lower_program(program, &arena, &sema.symtab);
            LlvmEmitter le = llvm_emitter_create(ll_out);
            if (is_wasm_target) le.wasm_main = true;
            if (debug_mode) llvm_emitter_enable_debug(&le, input_file);
            if (lmod) llvm_emit_module(&le, lmod);
            TIMER_END(emit);
            fclose(ll_out);
        } else {
            TIMER_START(emit);
            FILE *ll_out = open_memstream(&lir_buf, &lir_size);
            if (!ll_out) {
                cli_error("could not create the in-memory LLVM output buffer: %s", strerror(errno));
                cli_help("free some memory and try again; run with -v if this keeps happening");
                return 1;
            }
            LirModule *lmod = lower_program(program, &arena, &sema.symtab);
            LlvmEmitter le = llvm_emitter_create(ll_out);
            if (is_wasm_target) le.wasm_main = true;
            if (debug_mode) llvm_emitter_enable_debug(&le, input_file);
            if (lmod) llvm_emit_module(&le, lmod);
            TIMER_END(emit);
            fclose(ll_out);
        }
    }

    if (mix_error_count() > 0) {
        compile_phase_failed("code generation");
        arena_destroy(&arena); free(source); 
        return 1;
    }

    if (emit_ir_only) {
        if (verbose) fprintf(stderr, "mix: wrote %s\n", gen_path);
        arena_destroy(&arena); free(source); 
        return 0;
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
    {
        // Cache the result so subsequent invocations skip the stat storm
        static const char *cached_runtime = NULL;
        static bool cached = false;
        if (cached) {
            runtime_path = cached_runtime;
        } else {
            if (!debug_mode) {
                for (int i = 0; runtime_o_paths[i]; i++) {
                    struct stat ost, src_st;
                    if (stat(runtime_o_paths[i], &ost) != 0) continue;
                    char rt_c[1200];
                    snprintf(rt_c, sizeof(rt_c), "%s", runtime_o_paths[i]);
                    char *dot = strrchr(rt_c, '.');
                    if (dot) { strcpy(dot, ".c"); }
                    if (stat(rt_c, &src_st) == 0 && src_st.st_mtime > ost.st_mtime) continue;
                    const char *src_alt[] = {"lib/runtime.c", "../lib/runtime.c", NULL};
                    bool stale = false;
                    for (int j = 0; src_alt[j]; j++) {
                        if (stat(src_alt[j], &src_st) == 0 && src_st.st_mtime > ost.st_mtime) {
                            stale = true; break;
                        }
                    }
                    if (stale) continue;
                    runtime_path = runtime_o_paths[i];
                    break;
                }
            }
            if (!runtime_path) {
                for (int i = 0; runtime_paths[i]; i++) {
                    FILE *test = fopen(runtime_paths[i], "r");
                    if (test) { fclose(test); runtime_path = runtime_paths[i]; break; }
                }
            }
            cached = true;
            cached_runtime = runtime_path;
        }
    }

    // Link — build argv array to avoid shell injection
    // Max entries: clang/cc + flags(5) + main_file + modules(64) + runtime + -o + output + -lm
    //              + link_flags(64) + LDFLAGS tokens(64) + NULL
    #define MAX_LINK_ARGV 256
    const char *link_argv[MAX_LINK_ARGV];
    int ai = 0;
    bool cross_target = is_wasm_target || is_wasm_browser;

    // For wasm-browser, write .ll to a temp file (emcc doesn't support stdin pipe)
    char emsc_ll_path[256] = "";
    if (is_wasm_browser && lir_buf) {
        snprintf(emsc_ll_path, sizeof(emsc_ll_path), "/tmp/mix_emsc_%d.ll", getpid());
        FILE *f = fopen(emsc_ll_path, "w");
        if (f) {
            fwrite(lir_buf, 1, lir_size, f);
            fclose(f);
        }
    }

    if (is_wasm_browser) {
        const char *emcc = detect_emcc();
        link_argv[ai++] = emcc ? emcc : "emcc";
        link_argv[ai++] = emsc_ll_path;
        link_argv[ai++] = "-Wno-override-module";
        link_argv[ai++] = "-sNO_EXIT_RUNTIME=1";
        char shell_path[512] = "";
        // Check for shell.html next to the source file first
        const char *last_slash = input_file ? strrchr(input_file, '/') : NULL;
        if (last_slash) {
            int dir_len = (int)(last_slash - input_file);
            snprintf(shell_path, sizeof(shell_path), "%.*s/shell.html", dir_len, input_file);
            struct stat sh_st;
            if (stat(shell_path, &sh_st) == 0) {
                link_argv[ai++] = "--shell-file";
                link_argv[ai++] = arena_strdup(&arena, shell_path);
                goto shell_done;
            }
        }
        if (exe_dir[0]) {
            snprintf(shell_path, sizeof(shell_path), "%s/../lib/shell.html", exe_dir);
            struct stat shell_st;
            if (stat(shell_path, &shell_st) == 0) {
                link_argv[ai++] = "--shell-file";
                link_argv[ai++] = arena_strdup(&arena, shell_path);
            }
        }
        shell_done: ;
    } else if (is_wasm_target) {
        const char *wasi_clang = detect_wasi_clang();
        link_argv[ai++] = wasi_clang ? wasi_clang : "clang";
        link_argv[ai++] = "--target=wasm32-wasip1";
        link_argv[ai++] = "--sysroot=/opt/homebrew/share/wasi-sysroot";
        link_argv[ai++] = "-resource-dir=/opt/homebrew/share/wasi-runtimes";
        link_argv[ai++] = "-x";
        link_argv[ai++] = "ir";
        link_argv[ai++] = "-";
        link_argv[ai++] = "-x";
        link_argv[ai++] = "none";
        link_argv[ai++] = "-Wno-override-module";
    } else if (use_llvm_backend) {
        // Combined compile+link: pipe .ll to clang's stdin, then link with runtime + modules
        link_argv[ai++] = "clang";
        link_argv[ai++] = "-x";
        link_argv[ai++] = "ir";
        link_argv[ai++] = "-";
        link_argv[ai++] = "-x";
        link_argv[ai++] = "none";
        link_argv[ai++] = "-Wno-override-module";
    } else {
        link_argv[ai++] = "cc";
        link_argv[ai++] = gen_path;
    }
    if (debug_mode) link_argv[ai++] = "-g";
    if (opt_level) {
        char *opt_buf = arena_alloc(&arena, 4);
        if (opt_buf) { sprintf(opt_buf, "-O%s", opt_level); link_argv[ai++] = opt_buf; }
    }
    for (int i = 0; i < module_count && ai < MAX_LINK_ARGV - 8; i++)
        link_argv[ai++] = module_asm_files[i];
    // Add source files (e.g. glad.c)
    for (int i = 0; i < source_file_count && ai < MAX_LINK_ARGV - 8; i++)
        link_argv[ai++] = source_files[i];
    if (runtime_path && !cross_target)
        link_argv[ai++] = runtime_path;
    if (is_wasm_browser) {
        link_argv[ai++] = "build/runtime-emsc.o";
        // Also resolve via exe_dir so it works from subdirectories
        if (exe_dir[0]) {
            char emsc_o_path[512];
            snprintf(emsc_o_path, sizeof(emsc_o_path), "%s/../build/runtime-emsc.o", exe_dir);
            struct stat emsc_o_st;
            if (stat(emsc_o_path, &emsc_o_st) == 0) {
                link_argv[ai - 1] = arena_strdup(&arena, emsc_o_path);
            }
        }
    } else if (is_wasm_target) {
        link_argv[ai++] = "build/runtime-wasi.o";
    }
    link_argv[ai++] = "-o";
    link_argv[ai++] = output_file;
    if (!cross_target)
        link_argv[ai++] = "-lm";
    // Vendor library search paths first (project vendored libs take priority
    // over system/brew paths like /opt/homebrew/lib).
    if (!cross_target) {
        for (int i = 0; i < vendor_ldir_count && ai < MAX_LINK_ARGV - 2; i++) {
            char *lflag = arena_alloc(&arena, strlen(vendor_ldirs[i]) + 3);
            sprintf(lflag, "-L%s", vendor_ldirs[i]);
            link_argv[ai++] = lflag;
        }
    }
    // Force per-directory search so a vendored .a in a vendor -L dir beats a
    // .dylib for the same lib found in a later system -L dir (macOS linker
    // defaults to trying -search_dylibs_first first, which would pick up a
    // brew .dylib before our vendored .a).
    if (!cross_target)
        link_argv[ai++] = "-Wl,-search_paths_first";
#ifdef __APPLE__
    if (!cross_target) {
        char *sdk_libdir = detect_macos_sdk_libdir();
        if (sdk_libdir && ai < MAX_LINK_ARGV - 2) {
            char *lflag = arena_alloc(&arena, strlen(sdk_libdir) + 3);
            sprintf(lflag, "-L%s", sdk_libdir);
            link_argv[ai++] = lflag;
        }
    }
#endif
    if (!cross_target) {
        for (int i = 0; i < link_flag_count && ai < MAX_LINK_ARGV - 2; i++)
            link_argv[ai++] = link_flags[i];
    }
    // Add vendor -I directories
    if (!cross_target) {
        for (int i = 0; i < vendor_idir_count && ai < MAX_LINK_ARGV - 2; i++) {
            char *iflag = arena_alloc(&arena, strlen(vendor_idirs[i]) + 3);
            sprintf(iflag, "-I%s", vendor_idirs[i]);
            link_argv[ai++] = iflag;
        }
    }

    // CPPFLAGS -I flags: pass to linker so source files can find includes
    const char *env_cppflags = NULL;
    if (!cross_target) env_cppflags = getenv("CPPFLAGS");
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
    const char *env_ldflags = cross_target ? NULL : getenv("LDFLAGS");
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
    int ret;
    if (is_wasm_browser) {
        // emcc reads from a file, not stdin
        ret = run_process(link_argv, verbose ? NULL : &link_stderr);
    } else if (use_llvm_backend && lir_buf) {
        ret = run_process_stdin(link_argv, lir_buf, lir_size, verbose ? NULL : &link_stderr);
    } else {
        ret = run_process(link_argv, verbose ? NULL : &link_stderr);
    }
    TIMER_END(cc);
    free(ldflags_buf);
    if (ret != 0) {
        if (verbose) {
            cli_error("linker failed (exit %d)", ret);
            cli_help("read the tool output above, then adjust missing symbols, sources, or linker flags");
        } else {
            // Try to translate common linker errors
            if (link_stderr && (strstr(link_stderr, "undefined reference") ||
                                strstr(link_stderr, "undefined symbol") ||
                                strstr(link_stderr, "Undefined symbols"))) {
                cli_error("linking failed: undefined symbol");
                cli_help("make sure the function is defined, add the needed C source file, or pass the required -l library");
            } else if (link_stderr && (strstr(link_stderr, "library not found") ||
                                       strstr(link_stderr, "cannot find -l"))) {
                cli_error("linking failed: library not found");
                cli_help("check the -l name and add a matching -L search path if the library is not in a system directory");
            } else {
                cli_error("linking failed");
                cli_help("run the same command with -v to show the linker output");
            }
        }
        free(link_stderr);
        return 1;
    }
    free(link_stderr);

    // Cleanup
    if (!verbose) {
        if (use_c_backend && gen_path[0]) remove(gen_path);
        for (int i = 0; i < module_count; i++)
            remove(module_asm_files[i]);
        if (emsc_ll_path[0]) remove(emsc_ll_path);
    }

    if (verbose) fprintf(stderr, "mix: wrote %s\n", output_file);

    TIMER_START(cleanup);
    free(lir_buf);
    arena_destroy(&arena);
    free(source);
    
    TIMER_END(cleanup);

    // In run mode, execute the compiled binary
    if (mode == MODE_RUN) {
        TIMER_START(run);
        if (is_wasm_target) {
            const char *run_argv[] = {"wasmtime", output_file, NULL};
            int rc = run_process(run_argv, NULL);
            TIMER_END(run);
            return rc;
        }
        if (is_wasm_browser) {
            // Extract base name for the URL
            const char *url_base = strrchr(output_file, '/');
            url_base = url_base ? url_base + 1 : output_file;
            // Determine the server directory
            char serve_dir[512] = ".";
            const char *sep = strrchr(output_file, '/');
            if (sep) {
                size_t dlen = sep - output_file;
                memcpy(serve_dir, output_file, dlen);
                serve_dir[dlen] = '\0';
            }
            // Try to find a free port starting at 8080
            int port = 8080;
            for (int p = 8080; p < 8090; p++) {
                port = p;
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) break;
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(p);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    close(sock);
                    break; // port is free
                }
                close(sock);
            }
            // Fork HTTP server
            pid_t server_pid = fork();
            if (server_pid == 0) {
                char port_str[16];
                snprintf(port_str, sizeof(port_str), "%d", port);
                const char *sv_argv[] = {"python3", "-m", "http.server", port_str, NULL};
                if (serve_dir[0]) chdir(serve_dir);
                execvp("python3", (char **)sv_argv);
                _exit(1);
            }
            // Open browser
            char url[512];
            snprintf(url, sizeof(url), "http://localhost:%d/%s", port, url_base);
            pid_t browser_pid = fork();
            if (browser_pid == 0) {
                const char *open_argv[] = {"open", url, NULL};
                execvp("open", (char **)open_argv);
                _exit(1);
            }
            waitpid(browser_pid, NULL, 0);
            fprintf(stderr, "mix: serving at %s (press Enter to stop)\n", url);
            getchar();
            kill(server_pid, SIGTERM);
            waitpid(server_pid, NULL, 0);
            TIMER_END(run);
            return 0;
        }
        char run_path[512];
        if (output_file[0] == '/' || (output_file[0] == '.' && output_file[1] == '/')) {
            snprintf(run_path, sizeof(run_path), "%s", output_file);
        } else {
            snprintf(run_path, sizeof(run_path), "./%s", output_file);
        }
        const char *run_argv[] = {run_path, NULL};
        int rc = run_process(run_argv, NULL);
        TIMER_END(run);
        return rc;
    }

    return 0;
}
