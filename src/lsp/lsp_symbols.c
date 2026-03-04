#include "lsp_symbols.h"

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

static void sym_add(SymbolIndex *idx, const char *name, SrcLoc loc,
                    MixType *type, NodeKind kind, const char *container) {
    SymbolEntry *e = calloc(1, sizeof(SymbolEntry));
    e->name = strdup(name);
    e->def_loc = loc;
    e->type = type;
    e->decl_kind = kind;
    e->container = container ? strdup(container) : NULL;

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

            default:
                break;
        }
    }
}
