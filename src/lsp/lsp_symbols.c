#include "lsp_symbols.h"
#include "../lexer.h"
#include "../parser.h"
#include <libgen.h>
#include <sys/stat.h>

#define SYM_BUCKETS 128

static unsigned int sym_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

void symbol_index_init(SymbolIndex *idx) {
    idx->bucket_count = SYM_BUCKETS;
    idx->buckets = calloc(SYM_BUCKETS, sizeof(SymbolEntry *));
    idx->all = NULL;
    idx->all_count = 0;
    idx->all_cap = 0;
}

void symbol_index_clear(SymbolIndex *idx) {
    for (int i = 0; i < idx->bucket_count; i++) {
        SymbolEntry *e = idx->buckets[i];
        while (e) {
            SymbolEntry *next = e->next;
            free(e->name);
            free(e->container);
            if (e->param_names) {
                for (int j = 0; j < e->param_name_count; j++) free(e->param_names[j]);
                free(e->param_names);
            }
            if (e->param_type_strs) {
                for (int j = 0; j < e->param_name_count; j++) free(e->param_type_strs[j]);
                free(e->param_type_strs);
            }
            free(e->return_type_str);
            free(e);
            e = next;
        }
        idx->buckets[i] = NULL;
    }
    idx->all_count = 0;
}

void symbol_index_destroy(SymbolIndex *idx) {
    symbol_index_clear(idx);
    free(idx->buckets);
    free(idx->all);
    idx->buckets = NULL;
    idx->all = NULL;
}

// Forward declarations
static void store_fn_params(SymbolIndex *idx, const char *fn_name,
                            Param *params, int param_count, AstNode *return_type);

static void sym_add(SymbolIndex *idx, const char *name, SrcLoc loc,
                    MixType *type, NodeKind kind, const char *container) {
    SymbolEntry *e = calloc(1, sizeof(SymbolEntry));
    e->name = strdup(name);
    e->def_loc = loc;
    e->type = type;
    e->decl_kind = kind;
    e->container = container ? strdup(container) : NULL;
    e->param_names = NULL;
    e->param_type_strs = NULL;
    e->param_name_count = 0;
    e->return_type_str = NULL;

    unsigned int h = sym_hash(name) % idx->bucket_count;
    e->next = idx->buckets[h];
    idx->buckets[h] = e;

    // Add to flat list
    if (idx->all_count >= idx->all_cap) {
        idx->all_cap = idx->all_cap ? idx->all_cap * 2 : 64;
        idx->all = realloc(idx->all, sizeof(SymbolEntry *) * idx->all_cap);
    }
    idx->all[idx->all_count++] = e;
}

SymbolEntry *symbol_index_lookup(SymbolIndex *idx, const char *name) {
    unsigned int h = sym_hash(name) % idx->bucket_count;
    for (SymbolEntry *e = idx->buckets[h]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

// Walk a block to index local variable declarations
static void index_block(SymbolIndex *idx, AstNode *block) {
    if (!block || block->kind != NODE_BLOCK) return;
    for (int i = 0; i < block->block.stmt_count; i++) {
        AstNode *stmt = block->block.stmts[i];
        if (stmt->kind == NODE_VAR_DECL) {
            sym_add(idx, stmt->var_decl.name, stmt->loc,
                    stmt->resolved_type, NODE_VAR_DECL, NULL);
        }
        // Recurse into nested blocks
        if (stmt->kind == NODE_IF_STMT) {
            index_block(idx, stmt->if_stmt.then_block);
            if (stmt->if_stmt.else_block && stmt->if_stmt.else_block->kind == NODE_BLOCK)
                index_block(idx, stmt->if_stmt.else_block);
        } else if (stmt->kind == NODE_WHILE_STMT) {
            index_block(idx, stmt->while_stmt.body);
        } else if (stmt->kind == NODE_FOR_STMT) {
            // Index the loop variable with the element type from the iterable
            if (stmt->for_stmt.var_name) {
                MixType *var_type = NULL;
                AstNode *iter = stmt->for_stmt.iterable;
                if (iter && iter->resolved_type) {
                    MixType *it = iter->resolved_type;
                    if (it->kind == TYPE_LIST && it->list.elem_type) {
                        var_type = it->list.elem_type;
                    } else if (it->kind == TYPE_MAP) {
                        var_type = it->map.val_type;
                    } else {
                        // Range iteration or string — variable is same type
                        var_type = it;
                    }
                }
                sym_add(idx, stmt->for_stmt.var_name, stmt->loc,
                        var_type, NODE_VAR_DECL, NULL);
                // Index the index variable if present (for i, x in list)
                if (stmt->for_stmt.index_name) {
                    sym_add(idx, stmt->for_stmt.index_name, stmt->loc,
                            NULL, NODE_VAR_DECL, NULL);
                }
            }
            index_block(idx, stmt->for_stmt.body);
        } else if (stmt->kind == NODE_UNSAFE_BLOCK) {
            index_block(idx, stmt->unsafe_block.body);
        } else if (stmt->kind == NODE_ZONE_STMT) {
            index_block(idx, stmt->zone_stmt.body);
        }
    }
}

void symbol_index_build(SymbolIndex *idx, AstNode *program) {
    if (!program || program->kind != NODE_PROGRAM) return;

    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        switch (decl->kind) {
            case NODE_FN_DECL:
                sym_add(idx, decl->fn_decl.name, decl->loc,
                        decl->resolved_type, NODE_FN_DECL, NULL);
                // Store param names and type strings for signature help
                store_fn_params(idx, decl->fn_decl.name,
                                decl->fn_decl.params, decl->fn_decl.param_count,
                                decl->fn_decl.return_type);
                // Index parameters
                for (int j = 0; j < decl->fn_decl.param_count; j++) {
                    Param *p = &decl->fn_decl.params[j];
                    MixType *pt = p->type ? p->type->resolved_type : NULL;
                    sym_add(idx, p->name, decl->loc, pt, NODE_VAR_DECL, NULL);
                }
                // Index local variables in body
                index_block(idx, decl->fn_decl.body);
                break;

            case NODE_SHAPE_DECL:
                sym_add(idx, decl->shape_decl.name, decl->loc,
                        NULL, NODE_SHAPE_DECL, NULL);
                // Index methods
                for (int j = 0; j < decl->shape_decl.method_count; j++) {
                    AstNode *m = decl->shape_decl.methods[j];
                    sym_add(idx, m->fn_decl.name, m->loc,
                            m->resolved_type, NODE_FN_DECL,
                            decl->shape_decl.name);
                    index_block(idx, m->fn_decl.body);
                }
                break;

            case NODE_CONST_DECL:
                sym_add(idx, decl->const_decl.name, decl->loc,
                        decl->resolved_type, NODE_CONST_DECL, NULL);
                break;

            case NODE_TYPE_ALIAS:
                sym_add(idx, decl->type_alias.name, decl->loc,
                        NULL, NODE_TYPE_ALIAS, NULL);
                break;

            case NODE_COND_DECL:
                if (decl->cond_decl.active) {
                    for (int j = 0; j < decl->cond_decl.decl_count; j++) {
                        AstNode *cd = decl->cond_decl.decls[j];
                        if (cd->kind == NODE_FN_DECL) {
                            sym_add(idx, cd->fn_decl.name, cd->loc,
                                    cd->resolved_type, NODE_FN_DECL, NULL);
                            index_block(idx, cd->fn_decl.body);
                        }
                    }
                }
                break;

            case NODE_EXTERN_BLOCK:
                // Index extern functions for completion + signature help
                for (int j = 0; j < decl->extern_block.decl_count; j++) {
                    AstNode *ef = decl->extern_block.decls[j];
                    if (ef->kind == NODE_EXTERN_FN_DECL) {
                        sym_add(idx, ef->extern_fn_decl.name, ef->loc,
                                ef->resolved_type, NODE_FN_DECL, NULL);
                        // Store param names for signature help
                        SymbolEntry *fe = symbol_index_lookup(idx, ef->extern_fn_decl.name);
                        if (fe && ef->extern_fn_decl.param_count > 0) {
                            fe->param_name_count = ef->extern_fn_decl.param_count;
                            fe->param_names = calloc(ef->extern_fn_decl.param_count, sizeof(char *));
                            for (int k = 0; k < ef->extern_fn_decl.param_count; k++) {
                                fe->param_names[k] = strdup(ef->extern_fn_decl.params[k].name);
                            }
                        }
                    }
                }
                break;

            default:
                break;
        }
    }
}

// Get type name string from an AST type node (without sema)
static const char *ast_type_name(AstNode *type_node) {
    if (!type_node) return NULL;
    if (type_node->kind == NODE_TYPE_NAME) return type_node->type_name.name;
    if (type_node->kind == NODE_TYPE_PTR) return "*byte";
    return NULL;
}

// Store param info (names + type strings) from a function declaration's AST
static void store_fn_params(SymbolIndex *idx, const char *fn_name,
                            Param *params, int param_count, AstNode *return_type) {
    SymbolEntry *fe = symbol_index_lookup(idx, fn_name);
    if (!fe || param_count == 0) return;

    fe->param_name_count = param_count;
    fe->param_names = calloc(param_count, sizeof(char *));
    fe->param_type_strs = calloc(param_count, sizeof(char *));
    for (int j = 0; j < param_count; j++) {
        fe->param_names[j] = strdup(params[j].name);
        const char *ts = ast_type_name(params[j].type);
        fe->param_type_strs[j] = ts ? strdup(ts) : strdup("?");
    }
    const char *rts = ast_type_name(return_type);
    if (rts) fe->return_type_str = strdup(rts);
}

// Index pub symbols from an imported module file
static void index_module_file(SymbolIndex *idx, const char *module_path) {
    FILE *f = fopen(module_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(size + 1);
    if (!source) { fclose(f); return; }
    size_t rd = fread(source, 1, size, f);
    source[rd] = '\0';
    fclose(f);

    Arena arena = arena_create(256 * 1024);
    Lexer lexer = lexer_create(source, module_path, &arena);
    lexer_tokenize(&lexer);

    Parser parser = parser_create(lexer.tokens, lexer.token_count, &arena, module_path);
    AstNode *prog = parser_parse(&parser);

    if (prog && prog->kind == NODE_PROGRAM) {
        for (int i = 0; i < prog->program.decl_count; i++) {
            AstNode *d = prog->program.decls[i];
            if (d->kind == NODE_FN_DECL && d->fn_decl.is_pub) {
                sym_add(idx, d->fn_decl.name, d->loc, NULL, NODE_FN_DECL, NULL);
                store_fn_params(idx, d->fn_decl.name,
                                d->fn_decl.params, d->fn_decl.param_count,
                                d->fn_decl.return_type);
            } else if (d->kind == NODE_SHAPE_DECL && d->shape_decl.is_pub) {
                sym_add(idx, d->shape_decl.name, d->loc, NULL, NODE_SHAPE_DECL, NULL);
            } else if (d->kind == NODE_EXTERN_BLOCK) {
                for (int j = 0; j < d->extern_block.decl_count; j++) {
                    AstNode *ef = d->extern_block.decls[j];
                    if (ef->kind == NODE_EXTERN_FN_DECL) {
                        sym_add(idx, ef->extern_fn_decl.name, ef->loc,
                                NULL, NODE_FN_DECL, NULL);
                        store_fn_params(idx, ef->extern_fn_decl.name,
                                        ef->extern_fn_decl.params,
                                        ef->extern_fn_decl.param_count,
                                        ef->extern_fn_decl.return_type);
                    }
                }
            }
        }
    }

    arena_destroy(&arena);
    free(source);
}

// Resolve a module path from a use declaration relative to the main file
static char *resolve_use_path(const char *main_filepath, const char *module_path) {
    // Get directory of the main file
    char *dir_copy = strdup(main_filepath);
    char *dir = dirname(dir_copy);

    char path[1024];
    int off = snprintf(path, sizeof(path), "%s/", dir);
    for (const char *p = module_path; *p; p++) {
        if (*p == '.') path[off++] = '/';
        else path[off++] = *p;
    }
    path[off] = '\0';
    strncat(path, ".mix", sizeof(path) - strlen(path) - 1);

    free(dir_copy);

    // Check if file exists
    struct stat st;
    if (stat(path, &st) == 0) return strdup(path);
    return NULL;
}

void symbol_index_build_with_imports(SymbolIndex *idx, AstNode *program,
                                      const char *filepath) {
    // Build the main file's symbols
    symbol_index_build(idx, program);

    if (!program || program->kind != NODE_PROGRAM || !filepath) return;

    // Process use declarations to index imported module symbols
    for (int i = 0; i < program->program.decl_count; i++) {
        AstNode *decl = program->program.decls[i];
        if (decl->kind == NODE_USE_DECL) {
            char *mod_path = resolve_use_path(filepath, decl->use_decl.module_path);
            if (mod_path) {
                index_module_file(idx, mod_path);
                free(mod_path);
            }
        }
    }
}
