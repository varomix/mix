#ifndef LSP_WORKSPACE_H
#define LSP_WORKSPACE_H

#include "../arena.h"
#include "lsp_symbols.h"
#include <sys/stat.h>
#include <time.h>

// One file in the workspace cache. Holds its own arena because the AST
// references arena memory for identifier strings. The SymbolIndex is built
// from the AST and survives until invalidate or destroy.
typedef struct WorkspaceFile {
    char *path;             // absolute filesystem path
    char *uri;              // file:// URI
    time_t mtime;
    Arena arena;
    SymbolIndex symbols;
    bool valid;
    struct WorkspaceFile *next;
} WorkspaceFile;

typedef struct {
    WorkspaceFile *head;    // singly-linked list (workspaces are small)
    int file_count;
} WorkspaceCache;

void workspace_init(WorkspaceCache *ws);
void workspace_destroy(WorkspaceCache *ws);

// Recursively scan `root` for *.mix files and analyze each. Skips common
// build/vendor/git directories. Cap protects against runaway scans.
void workspace_scan(WorkspaceCache *ws, const char *root, int file_cap);

// Find or refresh a single file (re-analyzes if mtime changed).
WorkspaceFile *workspace_get(WorkspaceCache *ws, const char *path);

// Mark file invalid so the next workspace_get re-analyzes. No-op if absent.
void workspace_invalidate(WorkspaceCache *ws, const char *path);

#endif // LSP_WORKSPACE_H
