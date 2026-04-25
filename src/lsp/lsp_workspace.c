#include "lsp_workspace.h"
#include "lsp_document.h"
#include "../lexer.h"
#include "../parser.h"
#include "../sema.h"
#include "../errors.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define WS_ARENA_DEFAULT (256 * 1024)

void workspace_init(WorkspaceCache *ws) {
    ws->head = NULL;
    ws->file_count = 0;
}

static void workspace_file_free(WorkspaceFile *wf) {
    free(wf->path);
    free(wf->uri);
    symbol_index_destroy(&wf->symbols);
    arena_destroy(&wf->arena);
    free(wf);
}

void workspace_destroy(WorkspaceCache *ws) {
    WorkspaceFile *wf = ws->head;
    while (wf) {
        WorkspaceFile *next = wf->next;
        workspace_file_free(wf);
        wf = next;
    }
    ws->head = NULL;
    ws->file_count = 0;
}

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

// Discard sema diagnostics — workspace cache doesn't surface them.
static void discard_diag(DiagSeverity sev, SrcLoc loc, const char *msg, void *ud) {
    (void)sev; (void)loc; (void)msg; (void)ud;
}

// Re-run lex/parse/sema and rebuild the symbol index for a workspace file.
// Returns false on read error (file unchanged otherwise).
//
// We swallow diagnostics: workspace cache analysis is silent — only the
// open document's diagnostics surface to the editor.
static bool analyze_file(WorkspaceFile *wf) {
    char *source = read_whole_file(wf->path);
    if (!source) return false;

    arena_destroy(&wf->arena);
    wf->arena = arena_create(WS_ARENA_DEFAULT);
    symbol_index_clear(&wf->symbols);

    DiagnosticCallback prev_cb = NULL;
    void *prev_ud = NULL;
    errors_get_callback(&prev_cb, &prev_ud);
    errors_set_callback(discard_diag, NULL);
    errors_reset();

    Lexer lexer = lexer_create(source, wf->path, &wf->arena);
    lexer_tokenize(&lexer);
    Parser parser = parser_create(lexer.tokens, lexer.token_count, &wf->arena, wf->path);
    AstNode *program = parser_parse(&parser);

    if (program) {
        Sema sema = {0};
        sema.arena = &wf->arena;
        sema.symtab = symtab_create(&wf->arena);
        sema_analyze(&sema, program);
        symbol_index_build(&wf->symbols, program);
    }

    errors_reset();
    errors_set_callback(prev_cb, prev_ud);
    free(source);
    free(lexer.tokens);
    wf->valid = true;
    return true;
}

static WorkspaceFile *workspace_find(WorkspaceCache *ws, const char *path) {
    for (WorkspaceFile *wf = ws->head; wf; wf = wf->next) {
        if (strcmp(wf->path, path) == 0) return wf;
    }
    return NULL;
}

static WorkspaceFile *workspace_add(WorkspaceCache *ws, const char *path) {
    WorkspaceFile *wf = calloc(1, sizeof(WorkspaceFile));
    wf->path = strdup(path);
    wf->uri = lsp_path_to_uri(path);
    wf->arena = arena_create(WS_ARENA_DEFAULT);
    symbol_index_init(&wf->symbols);
    wf->valid = false;
    wf->next = ws->head;
    ws->head = wf;
    ws->file_count++;
    return wf;
}

WorkspaceFile *workspace_get(WorkspaceCache *ws, const char *path) {
    WorkspaceFile *wf = workspace_find(ws, path);
    struct stat st;
    if (stat(path, &st) != 0) return wf;  // gone — return stale (or NULL)

    if (!wf) {
        wf = workspace_add(ws, path);
        wf->mtime = st.st_mtime;
        analyze_file(wf);
    } else if (!wf->valid || st.st_mtime != wf->mtime) {
        wf->mtime = st.st_mtime;
        analyze_file(wf);
    }
    return wf;
}

void workspace_invalidate(WorkspaceCache *ws, const char *path) {
    WorkspaceFile *wf = workspace_find(ws, path);
    if (wf) wf->valid = false;
}

// Skip these subdirectories during scan — they never contain user .mix files
// we care about, and walking them on every initialize is slow.
static bool skip_dir(const char *name) {
    if (name[0] == '.') return true;            // .git, .vscode, hidden
    if (strcmp(name, "build") == 0) return true;
    if (strcmp(name, "node_modules") == 0) return true;
    if (strcmp(name, "out") == 0) return true;
    if (strcmp(name, "target") == 0) return true;
    return false;
}

static void scan_recursive(WorkspaceCache *ws, const char *dir, int file_cap) {
    if (ws->file_count >= file_cap) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (skip_dir(ent->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_recursive(ws, path, file_cap);
            if (ws->file_count >= file_cap) break;
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len < 5 || strcmp(ent->d_name + len - 4, ".mix") != 0) continue;
            workspace_get(ws, path);  // adds + analyzes
        }
    }
    closedir(d);
}

void workspace_scan(WorkspaceCache *ws, const char *root, int file_cap) {
    if (!root) return;
    scan_recursive(ws, root, file_cap);
}
