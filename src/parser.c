#include "parser.h"
#include "lexer.h"
#include "errors.h"

Parser parser_create(Token *tokens, int token_count, Arena *arena, const char *filename) {
    Parser p = {0};
    p.tokens = tokens;
    p.token_count = token_count;
    p.pos = 0;
    p.arena = arena;
    p.filename = filename;
    return p;
}

// --- Helpers ---

static Token *current(Parser *p) {
    return &p->tokens[p->pos];
}

static Token *peek_at(Parser *p, int offset) {
    int idx = p->pos + offset;
    if (idx >= p->token_count) return &p->tokens[p->token_count - 1]; // EOF
    return &p->tokens[idx];
}

static SrcLoc tok_loc(Parser *p) {
    Token *t = current(p);
    return (SrcLoc){p->filename, t->line, t->col};
}

static bool check(Parser *p, TokenKind kind) {
    return current(p)->kind == kind;
}

static bool at_end(Parser *p) {
    return current(p)->kind == TOK_EOF;
}

static Token *advance_tok(Parser *p) {
    Token *t = current(p);
    if (!at_end(p)) p->pos++;
    return t;
}

static Token *expect(Parser *p, TokenKind kind) {
    if (current(p)->kind != kind) {
        mix_error(tok_loc(p), "expected %s, got %s",
                  token_kind_name(kind), token_kind_name(current(p)->kind));
        return current(p);
    }
    return advance_tok(p);
}

static bool match_tok(Parser *p, TokenKind kind) {
    if (check(p, kind)) {
        advance_tok(p);
        return true;
    }
    return false;
}

static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) advance_tok(p);
}

static char *tok_str(Parser *p, Token *t) {
    // For IDENT_MUT, don't include the ! in the name
    int len = t->length;
    if (t->kind == TOK_IDENT_MUT) len--; // strip trailing !
    return arena_strndup(p->arena, t->start, len);
}

// --- Forward declarations ---
static AstNode *parse_expr(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_type(Parser *p);

// --- Type parsing ---

static AstNode *parse_type(Parser *p) {
    SrcLoc loc = tok_loc(p);

    if (match_tok(p, TOK_REF) || match_tok(p, TOK_REF_MUT)) {
        TokenKind op = p->tokens[p->pos - 1].kind;
        AstNode *node = ast_new(p->arena, NODE_TYPE_REF, loc);
        node->type_ref.is_mutable = (op == TOK_REF_MUT);
        node->type_ref.base_type = parse_type(p);
        return node;
    }

    // Pointer type: *T
    if (match_tok(p, TOK_STAR)) {
        AstNode *node = ast_new(p->arena, NODE_TYPE_PTR, loc);
        node->type_ptr.base_type = parse_type(p);
        return node;
    }

    // List type: [T]
    if (match_tok(p, TOK_LBRACKET)) {
        AstNode *elem_type = parse_type(p);
        expect(p, TOK_RBRACKET);
        // Represent as NODE_TYPE_NAME with name "[T]" — sema resolves to TYPE_LIST
        AstNode *node = ast_new(p->arena, NODE_TYPE_NAME, loc);
        node->type_name.type_kind = TOK_LBRACKET; // marker for list type
        node->type_name.name = "list";
        // Store element type as resolved_type temporarily
        node->resolved_type = NULL; // will be resolved by sema
        // We need to stash the element type node — use the resolved_type pointer
        // Actually, let's add a list_elem_type field or reuse the existing mechanism
        // Simplest: store elem_type in node->type_ptr.base_type (reuse ptr struct)
        node->type_ptr.base_type = elem_type;
        return node;
    }

    // Set type: set[T]
    if (check(p, TOK_SET) && p->pos + 1 < p->token_count &&
        p->tokens[p->pos + 1].kind == TOK_LBRACKET) {
        advance_tok(p); // skip 'set'
        expect(p, TOK_LBRACKET);
        AstNode *elem_type = parse_type(p);
        expect(p, TOK_RBRACKET);
        AstNode *node = ast_new(p->arena, NODE_TYPE_NAME, loc);
        node->type_name.type_kind = TOK_SET;
        node->type_name.name = "set";
        node->type_ptr.base_type = elem_type;
        return node;
    }

    // Named type or keyword type
    Token *t = current(p);
    AstNode *node = ast_new(p->arena, NODE_TYPE_NAME, loc);

    if (t->kind >= TOK_INT && t->kind <= TOK_FLOAT64) {
        node->type_name.type_kind = t->kind;
        node->type_name.name = tok_str(p, t);
        advance_tok(p);
    } else if (t->kind == TOK_IDENT) {
        node->type_name.type_kind = TOK_IDENT;
        node->type_name.name = tok_str(p, t);
        advance_tok(p);
    } else {
        mix_error(loc, "expected type, got %s", token_kind_name(t->kind));
        node->type_name.name = "error";
        node->type_name.type_kind = TOK_INT;
    }

    // Generic instantiation: Name[T1, T2, ...]. Only attempt for IDENT
    // (named) types, not keywords like `int`.
    if (node->type_name.type_kind == TOK_IDENT && check(p, TOK_LBRACKET)) {
        advance_tok(p); // skip '['
        AstNode **args = NULL;
        int n = 0, cap = 0;
        do {
            if (n >= cap) {
                cap = cap ? cap * 2 : 4;
                AstNode **na = arena_alloc(p->arena, sizeof(AstNode*) * cap);
                if (args) memcpy(na, args, sizeof(AstNode*) * n);
                args = na;
            }
            args[n++] = parse_type(p);
        } while (match_tok(p, TOK_COMMA));
        expect(p, TOK_RBRACKET);
        node->type_name.type_args = args;
        node->type_name.type_arg_count = n;
    }

    // Check for optional postfix: T?
    if (match_tok(p, TOK_QUESTION)) {
        AstNode *opt = ast_new(p->arena, NODE_TYPE_OPTIONAL, loc);
        opt->type_optional.inner_type = node;
        return opt;
    }

    return node;
}

// --- Expression parsing (Pratt / precedence climbing) ---

typedef enum {
    PREC_NONE = 0,
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_BIT_OR,      // |
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_RANGE,       // ..  ..=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * / %
    PREC_UNARY,       // - not * &
    PREC_CALL,        // () [] .
} Precedence;

static AstNode *parse_expr_prec(Parser *p, Precedence min_prec);

static Precedence get_precedence(TokenKind kind) {
    switch (kind) {
        case TOK_OR:       return PREC_OR;
        case TOK_AND:      return PREC_AND;
        case TOK_PIPE:     return PREC_BIT_OR;
        case TOK_EQEQ: case TOK_NEQ: return PREC_EQUALITY;
        case TOK_LT: case TOK_GT: case TOK_LTE: case TOK_GTE: return PREC_COMPARISON;
        case TOK_DOTDOT: case TOK_DOTDOT_EQ: return PREC_RANGE;
        case TOK_PLUS: case TOK_MINUS: return PREC_TERM;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return PREC_FACTOR;
        default: return PREC_NONE;
    }
}

static AstNode *parse_primary(Parser *p) {
    SrcLoc loc = tok_loc(p);
    Token *t = current(p);

    switch (t->kind) {
        case TOK_INT_LIT: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_INT_LIT, loc);
            node->int_lit.value = t->value.int_val;
            return node;
        }
        case TOK_FLOAT_LIT: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_FLOAT_LIT, loc);
            node->float_lit.value = t->value.float_val;
            return node;
        }
        case TOK_STRING_LIT: {
            advance_tok(p);
            char *raw = arena_strndup(p->arena, t->start, t->length);
            int raw_len = t->length;

            // Check if string contains interpolation {expr}
            bool has_interp = false;
            for (int ci = 0; ci < raw_len; ci++) {
                if (raw[ci] == '{') { has_interp = true; break; }
            }

            if (!has_interp) {
                AstNode *node = ast_new(p->arena, NODE_STRING_LIT, loc);
                node->string_lit.value = raw;
                node->string_lit.length = raw_len;
                return node;
            }

            // Parse interpolated string: split into parts and expressions
            AstNode *node = ast_new(p->arena, NODE_STRING_INTERP, loc);
            char **parts = NULL;
            int *part_lens = NULL;
            AstNode **exprs = NULL;
            int part_count = 0, expr_count = 0;
            int part_cap = 0, expr_cap = 0;

            int seg_start = 0;
            for (int ci = 0; ci <= raw_len; ci++) {
                if (ci == raw_len || raw[ci] == '{') {
                    // Add string part
                    int seg_len = ci - seg_start;
                    if (part_count >= part_cap) {
                        part_cap = part_cap ? part_cap * 2 : 8;
                        char **np = arena_alloc(p->arena, sizeof(char*) * part_cap);
                        int *nl = arena_alloc(p->arena, sizeof(int) * part_cap);
                        if (parts) { memcpy(np, parts, sizeof(char*) * part_count); memcpy(nl, part_lens, sizeof(int) * part_count); }
                        parts = np; part_lens = nl;
                    }
                    parts[part_count] = arena_strndup(p->arena, raw + seg_start, seg_len);
                    part_lens[part_count] = seg_len;
                    part_count++;

                    if (ci < raw_len && raw[ci] == '{') {
                        // Find matching }
                        int depth = 1;
                        int expr_start = ci + 1;
                        ci++;
                        while (ci < raw_len && depth > 0) {
                            if (raw[ci] == '{') depth++;
                            else if (raw[ci] == '}') depth--;
                            if (depth > 0) ci++;
                        }
                        // raw[expr_start..ci) is the expression text
                        int expr_len = ci - expr_start;
                        char *expr_text = arena_strndup(p->arena, raw + expr_start, expr_len);

                        // Parse the expression text as a mini program
                        Arena *a = p->arena;
                        Lexer sub_lex = lexer_create(expr_text, p->filename, a);
                        lexer_tokenize(&sub_lex);
                        Parser sub_parser = parser_create(sub_lex.tokens, sub_lex.token_count, a, p->filename);
                        AstNode *expr2 = parse_expr(&sub_parser);
                        free(sub_lex.tokens);

                        if (expr_count >= expr_cap) {
                            expr_cap = expr_cap ? expr_cap * 2 : 8;
                            AstNode **ne = arena_alloc(p->arena, sizeof(AstNode*) * expr_cap);
                            if (exprs) memcpy(ne, exprs, sizeof(AstNode*) * expr_count);
                            exprs = ne;
                        }
                        exprs[expr_count++] = expr2;
                        seg_start = ci + 1;
                    }
                }
            }

            node->string_interp.parts = parts;
            node->string_interp.part_lengths = part_lens;
            node->string_interp.exprs = exprs;
            node->string_interp.expr_count = expr_count;
            return node;
        }
        case TOK_TRUE: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_BOOL_LIT, loc);
            node->bool_lit.value = true;
            return node;
        }
        case TOK_FALSE: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_BOOL_LIT, loc);
            node->bool_lit.value = false;
            return node;
        }
        case TOK_NONE: {
            advance_tok(p);
            return ast_new(p->arena, NODE_NONE_LIT, loc);
        }
        case TOK_LBRACKET: {
            // List literal or comprehension: [expr, ...] or [expr for var in iter]
            advance_tok(p); // skip [

            if (check(p, TOK_RBRACKET)) {
                // Empty list
                advance_tok(p);
                AstNode *node = ast_new(p->arena, NODE_LIST_LIT, loc);
                node->list_lit.elements = NULL;
                node->list_lit.element_count = 0;
                return node;
            }

            // Parse first expression
            AstNode *first = parse_expr(p);

            // Check for comprehension: [expr for var in iter if cond]
            if (check(p, TOK_FOR)) {
                advance_tok(p); // skip 'for'
                Token *var_tok = expect(p, TOK_IDENT);
                expect(p, TOK_IN);
                AstNode *iterable = parse_expr(p);
                AstNode *cond = NULL;
                if (check(p, TOK_IF)) {
                    advance_tok(p); // skip 'if'
                    cond = parse_expr(p);
                }
                expect(p, TOK_RBRACKET);

                AstNode *node = ast_new(p->arena, NODE_LIST_COMP, loc);
                node->list_comp.expr = first;
                node->list_comp.var_name = tok_str(p, var_tok);
                node->list_comp.iterable = iterable;
                node->list_comp.condition = cond;
                return node;
            }

            // Regular list literal
            AstNode **elements = NULL;
            int elem_count = 0;
            int elem_cap = 8;
            elements = arena_alloc(p->arena, sizeof(AstNode*) * elem_cap);
            elements[elem_count++] = first;

            while (match_tok(p, TOK_COMMA)) {
                AstNode *elem = parse_expr(p);
                if (elem_count >= elem_cap) {
                    elem_cap *= 2;
                    AstNode **ne = arena_alloc(p->arena, sizeof(AstNode*) * elem_cap);
                    memcpy(ne, elements, sizeof(AstNode*) * elem_count);
                    elements = ne;
                }
                elements[elem_count++] = elem;
            }

            expect(p, TOK_RBRACKET);
            AstNode *node = ast_new(p->arena, NODE_LIST_LIT, loc);
            node->list_lit.elements = elements;
            node->list_lit.element_count = elem_count;
            return node;
        }
        // Type-as-cast: int32(expr), uint8(expr), float(expr), etc.
        case TOK_INT: case TOK_FLOAT: case TOK_BOOL: case TOK_BYTE:
        case TOK_INT8: case TOK_INT16: case TOK_INT32: case TOK_INT64:
        case TOK_UINT8: case TOK_UINT16: case TOK_UINT32: case TOK_UINT64:
        case TOK_FLOAT32: case TOK_FLOAT64: {
            // Only treat as cast if followed by '('
            if (peek_at(p, 1)->kind == TOK_LPAREN) {
                TokenKind target = t->kind;
                advance_tok(p); // skip type keyword
                expect(p, TOK_LPAREN);
                AstNode *val = parse_expr(p);
                expect(p, TOK_RPAREN);
                AstNode *node = ast_new(p->arena, NODE_CAST_EXPR, loc);
                node->cast_expr.target_type = target;
                node->cast_expr.value = val;
                return node;
            }
            // Otherwise fall through to error (type keyword used as expression)
            mix_error(loc, "unexpected type keyword '%s' in expression", token_kind_name(t->kind));
            advance_tok(p);
            return ast_new(p->arena, NODE_INT_LIT, loc);
        }
        case TOK_SET: {
            // Set literal: set{expr, expr, ...}
            advance_tok(p); // skip 'set'
            expect(p, TOK_LBRACE);
            AstNode *node = ast_new(p->arena, NODE_SET_LIT, loc);
            AstNode **elements = NULL;
            int elem_count = 0, elem_cap = 0;

            if (!check(p, TOK_RBRACE)) {
                do {
                    AstNode *elem = parse_expr(p);
                    if (elem_count >= elem_cap) {
                        elem_cap = elem_cap ? elem_cap * 2 : 8;
                        AstNode **ne = arena_alloc(p->arena, sizeof(AstNode*) * elem_cap);
                        if (elements) memcpy(ne, elements, sizeof(AstNode*) * elem_count);
                        elements = ne;
                    }
                    elements[elem_count++] = elem;
                } while (match_tok(p, TOK_COMMA));
            }
            expect(p, TOK_RBRACE);
            node->set_lit.elements = elements;
            node->set_lit.element_count = elem_count;
            return node;
        }
        case TOK_LBRACE: {
            // Map literal: {key: val, key: val, ...}
            advance_tok(p); // skip {
            AstNode *node = ast_new(p->arena, NODE_MAP_LIT, loc);
            AstNode **keys = NULL;
            AstNode **values = NULL;
            int entry_count = 0;
            int entry_cap = 0;

            if (!check(p, TOK_RBRACE)) {
                do {
                    AstNode *key = parse_expr(p);
                    expect(p, TOK_COLON);
                    AstNode *val = parse_expr(p);
                    if (entry_count >= entry_cap) {
                        entry_cap = entry_cap ? entry_cap * 2 : 8;
                        AstNode **nk = arena_alloc(p->arena, sizeof(AstNode*) * entry_cap);
                        AstNode **nv = arena_alloc(p->arena, sizeof(AstNode*) * entry_cap);
                        if (keys) { memcpy(nk, keys, sizeof(AstNode*) * entry_count);
                                    memcpy(nv, values, sizeof(AstNode*) * entry_count); }
                        keys = nk; values = nv;
                    }
                    keys[entry_count] = key;
                    values[entry_count] = val;
                    entry_count++;
                } while (match_tok(p, TOK_COMMA));
            }
            expect(p, TOK_RBRACE);
            node->map_lit.keys = keys;
            node->map_lit.values = values;
            node->map_lit.entry_count = entry_count;
            return node;
        }
        case TOK_IDENT:
        case TOK_IDENT_MUT: {
            char *name = tok_str(p, t);
            bool is_mut = (t->kind == TOK_IDENT_MUT);
            advance_tok(p);

            // Lambda: ident => expr
            if (check(p, TOK_FAT_ARROW)) {
                advance_tok(p); // skip =>
                AstNode *node = ast_new(p->arena, NODE_LAMBDA, loc);
                node->lambda.param_count = 1;
                node->lambda.param_names = arena_alloc(p->arena, sizeof(char*));
                node->lambda.param_names[0] = name;
                node->lambda.body = parse_expr(p);
                node->lambda.generated_name = NULL;
                return node;
            }

            // Generic shape constructor: PascalCase[Type, ...](args).
            // Disambiguate from `nums[0]` (index) by requiring the name to
            // start with an uppercase letter — MIX convention is PascalCase
            // for shapes, snake_case for values.
            AstNode **type_args = NULL;
            int type_arg_count = 0;
            if (check(p, TOK_LBRACKET) &&
                name[0] >= 'A' && name[0] <= 'Z') {
                advance_tok(p); // skip '['
                int cap = 0;
                do {
                    if (type_arg_count >= cap) {
                        cap = cap ? cap * 2 : 4;
                        AstNode **na = arena_alloc(p->arena, sizeof(AstNode*) * cap);
                        if (type_args) memcpy(na, type_args, sizeof(AstNode*) * type_arg_count);
                        type_args = na;
                    }
                    type_args[type_arg_count++] = parse_type(p);
                } while (match_tok(p, TOK_COMMA));
                expect(p, TOK_RBRACKET);
            }

            // Check for function call or shape literal: ident(
            if (check(p, TOK_LPAREN)) {
                advance_tok(p); // skip (

                // Look ahead to distinguish named shape literal from function call
                // Named shape literal: Name(field: val, ...) — IDENT followed by COLON
                // Positional shapes (no labels) are parsed as regular calls and
                // rewritten to shape literals in sema when the name resolves to a shape
                bool is_shape_lit = false;

                if (check(p, TOK_IDENT) && peek_at(p, 1)->kind == TOK_COLON) {
                    is_shape_lit = true;
                }

                if (is_shape_lit) {
                    // Parse shape literal
                    AstNode *node = ast_new(p->arena, NODE_SHAPE_LIT, loc);
                    node->shape_lit.shape_name = name;
                    node->shape_lit.type_args = type_args;
                    node->shape_lit.type_arg_count = type_arg_count;

                    char **field_names = NULL;
                    AstNode **field_values = NULL;
                    int field_count = 0;
                    int field_cap = 0;

                    do {
                        Token *fname = expect(p, TOK_IDENT);
                        expect(p, TOK_COLON);
                        AstNode *fval = parse_expr(p);

                        if (field_count >= field_cap) {
                            field_cap = field_cap ? field_cap * 2 : 8;
                            char **nn = arena_alloc(p->arena, sizeof(char*) * field_cap);
                            AstNode **nv = arena_alloc(p->arena, sizeof(AstNode*) * field_cap);
                            if (field_names) { memcpy(nn, field_names, sizeof(char*) * field_count); memcpy(nv, field_values, sizeof(AstNode*) * field_count); }
                            field_names = nn;
                            field_values = nv;
                        }
                        field_names[field_count] = tok_str(p, fname);
                        field_values[field_count] = fval;
                        field_count++;
                    } while (match_tok(p, TOK_COMMA));

                    expect(p, TOK_RPAREN);
                    node->shape_lit.field_names = field_names;
                    node->shape_lit.field_values = field_values;
                    node->shape_lit.field_count = field_count;
                    return node;
                }

                // Regular function call (positional shapes also land here — sema rewrites)
                AstNode *node = ast_new(p->arena, NODE_CALL_EXPR, loc);
                node->call.name = name;
                node->call.is_mutable_call = is_mut;

                // Parse arguments
                AstNode **args = NULL;
                int arg_count = 0;
                int arg_cap = 0;

                if (!check(p, TOK_RPAREN)) {
                    do {
                        AstNode *arg = parse_expr(p);
                        if (arg_count >= arg_cap) {
                            arg_cap = arg_cap ? arg_cap * 2 : 8;
                            AstNode **new_args = arena_alloc(p->arena, sizeof(AstNode*) * arg_cap);
                            if (args) memcpy(new_args, args, sizeof(AstNode*) * arg_count);
                            args = new_args;
                        }
                        args[arg_count++] = arg;
                    } while (match_tok(p, TOK_COMMA));
                }

                expect(p, TOK_RPAREN);
                node->call.args = args;
                node->call.arg_count = arg_count;
                return node;
            }

            AstNode *node = ast_new(p->arena, NODE_IDENT, loc);
            node->ident.name = name;
            node->ident.type_args = type_args;
            node->ident.type_arg_count = type_arg_count;
            node->ident.is_mutable = is_mut;
            return node;
        }
        case TOK_LPAREN: {
            advance_tok(p); // skip (
            // Check for multi-param lambda: (a, b) => expr
            // Detect: IDENT COMMA or IDENT RPAREN FAT_ARROW
            if (check(p, TOK_IDENT) &&
                (peek_at(p, 1)->kind == TOK_COMMA ||
                 (peek_at(p, 1)->kind == TOK_RPAREN && peek_at(p, 2)->kind == TOK_FAT_ARROW))) {
                // Parse lambda params
                char **params = NULL;
                int param_count = 0;
                int param_cap = 0;
                do {
                    Token *pt = expect(p, TOK_IDENT);
                    if (param_count >= param_cap) {
                        param_cap = param_cap ? param_cap * 2 : 4;
                        char **np = arena_alloc(p->arena, sizeof(char*) * param_cap);
                        if (params) memcpy(np, params, sizeof(char*) * param_count);
                        params = np;
                    }
                    params[param_count++] = tok_str(p, pt);
                } while (match_tok(p, TOK_COMMA));
                expect(p, TOK_RPAREN);
                expect(p, TOK_FAT_ARROW);

                AstNode *node = ast_new(p->arena, NODE_LAMBDA, loc);
                node->lambda.param_count = param_count;
                node->lambda.param_names = params;
                node->lambda.body = parse_expr(p);
                node->lambda.generated_name = NULL;
                return node;
            }
            AstNode *expr = parse_expr(p);
            expect(p, TOK_RPAREN);
            return expr;
        }
        // Unary prefix ops bind looser than postfix `.field` / `[idx]` /
        // method-call. Use parse_expr_prec(PREC_UNARY) (which runs the
        // postfix loop after parse_primary) so `-s.vel.x` parses as
        // `-(s.vel.x)`, not `(-s).vel.x`.
        case TOK_MINUS: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_UNARY_EXPR, loc);
            node->unary.op = TOK_MINUS;
            node->unary.operand = parse_expr_prec(p, PREC_UNARY);
            return node;
        }
        case TOK_NOT: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_UNARY_EXPR, loc);
            node->unary.op = TOK_NOT;
            node->unary.operand = parse_expr_prec(p, PREC_UNARY);
            return node;
        }
        case TOK_STAR: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_UNARY_EXPR, loc);
            node->unary.op = TOK_STAR;
            node->unary.operand = parse_expr_prec(p, PREC_UNARY);
            return node;
        }
        case TOK_AMPERSAND: {
            advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_UNARY_EXPR, loc);
            node->unary.op = TOK_AMPERSAND;
            node->unary.operand = parse_expr_prec(p, PREC_UNARY);
            return node;
        }
        case TOK_REF:
        case TOK_REF_MUT: {
            TokenKind op = advance_tok(p)->kind;
            AstNode *node = ast_new(p->arena, NODE_UNARY_EXPR, loc);
            node->unary.op = op;
            node->unary.operand = parse_expr_prec(p, PREC_UNARY);
            return node;
        }
        case TOK_SHARED: {
            // shared int(expr) or shared(expr)
            advance_tok(p); // skip 'shared'
            // Skip optional type keyword (e.g., 'int')
            TokenKind tk = current(p)->kind;
            if (tk >= TOK_INT && tk <= TOK_FLOAT64) {
                advance_tok(p);
            }
            expect(p, TOK_LPAREN);
            AstNode *init = parse_expr(p);
            expect(p, TOK_RPAREN);
            AstNode *node = ast_new(p->arena, NODE_SHARED_EXPR, loc);
            node->shared_expr.init_expr = init;
            return node;
        }
        case TOK_GO: {
            // go compute(5)
            advance_tok(p); // skip 'go'
            AstNode *call = parse_expr(p);
            AstNode *node = ast_new(p->arena, NODE_GO_EXPR, loc);
            node->go_expr.call_expr = call;
            return node;
        }
        case TOK_WAIT: {
            // wait t
            advance_tok(p); // skip 'wait'
            AstNode *handle = parse_expr(p);
            AstNode *node = ast_new(p->arena, NODE_WAIT_EXPR, loc);
            node->wait_expr.handle_expr = handle;
            return node;
        }
        default:
            mix_error(loc, "expected expression, got %s", token_kind_name(t->kind));
            advance_tok(p);
            return ast_new(p->arena, NODE_NONE_LIT, loc);
    }
}

static AstNode *parse_expr_prec(Parser *p, Precedence min_prec) {
    AstNode *left = parse_primary(p);

    // Handle postfix operators: .field, .method(args), [index]
    while (check(p, TOK_DOT) || check(p, TOK_LBRACKET)) {
        if (check(p, TOK_LBRACKET)) {
            SrcLoc idx_loc = tok_loc(p);
            advance_tok(p); // skip [

            // Check for [..end] or [..=end] — no start
            if (check(p, TOK_DOTDOT) || check(p, TOK_DOTDOT_EQ)) {
                bool inc = (current(p)->kind == TOK_DOTDOT_EQ);
                advance_tok(p); // skip .. or ..=
                AstNode *end_expr = check(p, TOK_RBRACKET) ? NULL : parse_expr_prec(p, PREC_RANGE);
                expect(p, TOK_RBRACKET);
                AstNode *node = ast_new(p->arena, NODE_SLICE_EXPR, idx_loc);
                node->slice_expr.object = left;
                node->slice_expr.start = NULL;
                node->slice_expr.end = end_expr;
                node->slice_expr.inclusive = inc;
                left = node;
                continue;
            }

            // Parse first expression, stopping before ..
            AstNode *first = parse_expr_prec(p, PREC_RANGE);

            // Check for slice: [first..end] or [first..=end] or [first..]
            if (check(p, TOK_DOTDOT) || check(p, TOK_DOTDOT_EQ)) {
                bool inc = (current(p)->kind == TOK_DOTDOT_EQ);
                advance_tok(p); // skip .. or ..=
                AstNode *end_expr = check(p, TOK_RBRACKET) ? NULL : parse_expr_prec(p, PREC_RANGE);
                expect(p, TOK_RBRACKET);
                AstNode *node = ast_new(p->arena, NODE_SLICE_EXPR, idx_loc);
                node->slice_expr.object = left;
                node->slice_expr.start = first;
                node->slice_expr.end = end_expr;
                node->slice_expr.inclusive = inc;
                left = node;
                continue;
            }

            // Regular index: [expr]
            expect(p, TOK_RBRACKET);
            AstNode *node = ast_new(p->arena, NODE_INDEX_EXPR, idx_loc);
            node->index_expr.object = left;
            node->index_expr.index = first;
            left = node;
            continue;
        }
        SrcLoc dot_loc = tok_loc(p);
        advance_tok(p); // skip .
        Token *field = current(p);
        if (field->kind != TOK_IDENT && field->kind != TOK_IDENT_MUT &&
            field->kind != TOK_REPEAT && field->kind != TOK_SET &&
            field->kind != TOK_UNION) {
            mix_error(dot_loc, "expected field or method name after '.'");
            break;
        }
        char *field_name = tok_str(p, field);
        bool is_mut = (field->kind == TOK_IDENT_MUT);
        advance_tok(p);

        // Check for method call: obj.name(args)
        if (check(p, TOK_LPAREN)) {
            advance_tok(p); // skip (

            // Qualified variant constructor: `AppError.NotFound(path: "x")`.
            // Detected by the same heuristic as bare shape literals — first
            // arg is `IDENT :`. Build a NODE_SHAPE_LIT keyed on the variant
            // name; sema looks it up directly (variants are global).
            if (check(p, TOK_IDENT) && peek_at(p, 1)->kind == TOK_COLON) {
                AstNode *node = ast_new(p->arena, NODE_SHAPE_LIT, dot_loc);
                node->shape_lit.shape_name = field_name;
                char **field_names = NULL;
                AstNode **field_values = NULL;
                int field_count = 0, field_cap = 0;
                do {
                    Token *fname = expect(p, TOK_IDENT);
                    expect(p, TOK_COLON);
                    AstNode *fval = parse_expr(p);
                    if (field_count >= field_cap) {
                        field_cap = field_cap ? field_cap * 2 : 8;
                        char **nn = arena_alloc(p->arena, sizeof(char*) * field_cap);
                        AstNode **nv = arena_alloc(p->arena, sizeof(AstNode*) * field_cap);
                        if (field_names) {
                            memcpy(nn, field_names, sizeof(char*) * field_count);
                            memcpy(nv, field_values, sizeof(AstNode*) * field_count);
                        }
                        field_names = nn;
                        field_values = nv;
                    }
                    field_names[field_count] = tok_str(p, fname);
                    field_values[field_count] = fval;
                    field_count++;
                } while (match_tok(p, TOK_COMMA));
                expect(p, TOK_RPAREN);
                node->shape_lit.field_names = field_names;
                node->shape_lit.field_values = field_values;
                node->shape_lit.field_count = field_count;
                left = node;
                continue;
            }

            AstNode *node = ast_new(p->arena, NODE_METHOD_CALL, dot_loc);
            node->method_call.object = left;
            node->method_call.method_name = field_name;
            node->method_call.is_mutable_call = is_mut;

            AstNode **args = NULL;
            int arg_count = 0;
            int arg_cap = 0;

            if (!check(p, TOK_RPAREN)) {
                do {
                    AstNode *arg = parse_expr(p);
                    if (arg_count >= arg_cap) {
                        arg_cap = arg_cap ? arg_cap * 2 : 8;
                        AstNode **na = arena_alloc(p->arena, sizeof(AstNode*) * arg_cap);
                        if (args) memcpy(na, args, sizeof(AstNode*) * arg_count);
                        args = na;
                    }
                    args[arg_count++] = arg;
                } while (match_tok(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN);
            node->method_call.args = args;
            node->method_call.arg_count = arg_count;
            left = node;
        } else {
            // Field access
            AstNode *node = ast_new(p->arena, NODE_FIELD_EXPR, dot_loc);
            node->field_expr.object = left;
            node->field_expr.field_name = field_name;
            left = node;
        }
    }

    // Handle "expr?" — try operator for result unwrapping/propagation
    if (check(p, TOK_QUESTION)) {
        SrcLoc try_loc = tok_loc(p);
        advance_tok(p); // skip '?'
        AstNode *node = ast_new(p->arena, NODE_TRY_EXPR, try_loc);
        node->try_expr.expr = left;
        left = node;
    }

    // Handle "expr else default" for optionals
    if (check(p, TOK_ELSE)) {
        SrcLoc else_loc = tok_loc(p);
        advance_tok(p); // skip 'else'
        AstNode *fallback = parse_expr(p);
        AstNode *node = ast_new(p->arena, NODE_ELSE_EXPR, else_loc);
        node->else_expr.value = left;
        node->else_expr.fallback = fallback;
        left = node;
    }

    while (true) {
        TokenKind op = current(p)->kind;
        Precedence prec = get_precedence(op);
        if (prec <= min_prec) break;

        SrcLoc loc = tok_loc(p);
        advance_tok(p); // consume operator

        AstNode *right = parse_expr_prec(p, prec);
        AstNode *bin = ast_new(p->arena, NODE_BINARY_EXPR, loc);
        bin->binary.op = op;
        bin->binary.left = left;
        bin->binary.right = right;
        left = bin;
    }

    return left;
}

static AstNode *parse_expr(Parser *p) {
    return parse_expr_prec(p, PREC_NONE);
}

// --- Statement parsing ---

static AstNode *parse_block(Parser *p) {
    SrcLoc loc = tok_loc(p);
    expect(p, TOK_INDENT);

    AstNode **stmts = NULL;
    int stmt_count = 0;
    int stmt_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT) || at_end(p)) break;

        AstNode *stmt = parse_stmt(p);
        if (stmt_count >= stmt_cap) {
            stmt_cap = stmt_cap ? stmt_cap * 2 : 8;
            AstNode **new_stmts = arena_alloc(p->arena, sizeof(AstNode*) * stmt_cap);
            if (stmts) memcpy(new_stmts, stmts, sizeof(AstNode*) * stmt_count);
            stmts = new_stmts;
        }
        stmts[stmt_count++] = stmt;

        skip_newlines(p);
    }

    expect(p, TOK_DEDENT);

    AstNode *block = ast_new(p->arena, NODE_BLOCK, loc);
    block->block.stmts = stmts;
    block->block.stmt_count = stmt_count;
    return block;
}

static AstNode *parse_if_stmt(Parser *p) {
    SrcLoc loc = tok_loc(p);
    expect(p, TOK_IF);

    AstNode *node = ast_new(p->arena, NODE_IF_STMT, loc);
    node->if_stmt.condition = parse_expr(p);
    skip_newlines(p);
    node->if_stmt.then_block = parse_block(p);

    skip_newlines(p);
    if (check(p, TOK_ELSE)) {
        advance_tok(p);
        skip_newlines(p);
        if (check(p, TOK_IF)) {
            node->if_stmt.else_block = parse_if_stmt(p);
        } else {
            node->if_stmt.else_block = parse_block(p);
        }
    }

    return node;
}

static AstNode *parse_while_stmt(Parser *p) {
    SrcLoc loc = tok_loc(p);
    expect(p, TOK_WHILE);

    AstNode *node = ast_new(p->arena, NODE_WHILE_STMT, loc);
    node->while_stmt.condition = parse_expr(p);
    skip_newlines(p);
    node->while_stmt.body = parse_block(p);
    return node;
}

static AstNode *parse_for_stmt(Parser *p) {
    SrcLoc loc = tok_loc(p);
    expect(p, TOK_FOR);

    AstNode *node = ast_new(p->arena, NODE_FOR_STMT, loc);
    node->for_stmt.var_is_mutable = false;

    // Parse: for var in expr
    //     or for var! in expr
    //     or for idx, var in expr
    //     or for idx, var! in expr
    Token *first = advance_tok(p);
    if (first->kind != TOK_IDENT && first->kind != TOK_IDENT_MUT) {
        mix_error(tok_loc(p), "expected identifier after 'for'");
        return node;
    }
    char *first_name = tok_str(p, first);

    if (match_tok(p, TOK_COMMA)) {
        Token *second = advance_tok(p);
        if (second->kind != TOK_IDENT && second->kind != TOK_IDENT_MUT) {
            mix_error(tok_loc(p), "expected loop variable name after ','");
            return node;
        }
        node->for_stmt.index_name = first_name;
        node->for_stmt.var_name = tok_str(p, second);
        node->for_stmt.var_is_mutable = (second->kind == TOK_IDENT_MUT);
    } else {
        node->for_stmt.var_name = first_name;
        node->for_stmt.index_name = NULL;
        node->for_stmt.var_is_mutable = (first->kind == TOK_IDENT_MUT);
    }

    expect(p, TOK_IN);
    node->for_stmt.iterable = parse_expr(p);
    skip_newlines(p);
    node->for_stmt.body = parse_block(p);
    return node;
}

static AstNode *parse_stmt(Parser *p) {
    skip_newlines(p);
    SrcLoc loc = tok_loc(p);

    // unsafe block
    if (check(p, TOK_UNSAFE)) {
        advance_tok(p);
        skip_newlines(p);
        AstNode *node = ast_new(p->arena, NODE_UNSAFE_BLOCK, loc);
        node->unsafe_block.body = parse_block(p);
        return node;
    }

    // zone [:name] block
    if (check(p, TOK_ZONE)) {
        advance_tok(p);
        AstNode *node = ast_new(p->arena, NODE_ZONE_STMT, loc);
        node->zone_stmt.name = NULL;
        // Optional :name
        if (match_tok(p, TOK_COLON)) {
            Token *name_tok = expect(p, TOK_IDENT);
            node->zone_stmt.name = tok_str(p, name_tok);
        }
        skip_newlines(p);
        node->zone_stmt.body = parse_block(p);
        return node;
    }

    // *ptr = val (dereference assignment — typically inside unsafe)
    if (check(p, TOK_STAR) && peek_at(p, 1)->kind == TOK_IDENT) {
        // Check if this looks like *ident = expr
        // Save position and try parsing
        int save_pos = p->pos;
        advance_tok(p); // skip *
        AstNode *ptr_expr = parse_primary(p);
        if (check(p, TOK_EQ)) {
            advance_tok(p); // skip =
            AstNode *node = ast_new(p->arena, NODE_DEREF_ASSIGN, loc);
            node->deref_assign.ptr_expr = ptr_expr;
            node->deref_assign.value = parse_expr(p);
            return node;
        }
        // Not an assignment — restore and fall through to expression
        p->pos = save_pos;
    }

    // defer stmt
    if (check(p, TOK_DEFER)) {
        advance_tok(p);
        AstNode *node = ast_new(p->arena, NODE_DEFER_STMT, loc);
        node->defer_stmt.stmt = parse_stmt(p);
        return node;
    }

    // fail expr
    if (check(p, TOK_IDENT) && current(p)->length == 4 && memcmp(current(p)->start, "fail", 4) == 0) {
        advance_tok(p);
        AstNode *node = ast_new(p->arena, NODE_FAIL_STMT, loc);
        node->fail_stmt.value = parse_expr(p);
        return node;
    }

    // done [expr]  or  => [expr]
    if (check(p, TOK_DONE) || check(p, TOK_FAT_ARROW)) {
        advance_tok(p);
        AstNode *node = ast_new(p->arena, NODE_DONE_STMT, loc);
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_DEDENT) && !at_end(p)) {
            node->done_stmt.value = parse_expr(p);
        }
        return node;
    }

    // break
    if (check(p, TOK_BREAK)) {
        advance_tok(p);
        return ast_new(p->arena, NODE_BREAK_STMT, loc);
    }

    // continue
    if (check(p, TOK_CONTINUE)) {
        advance_tok(p);
        return ast_new(p->arena, NODE_CONTINUE_STMT, loc);
    }

    // if
    if (check(p, TOK_IF)) return parse_if_stmt(p);

    // while
    if (check(p, TOK_WHILE)) return parse_while_stmt(p);

    // for
    if (check(p, TOK_FOR)) return parse_for_stmt(p);

    // match expr
    //     pattern => expr_or_block
    //     _       => expr_or_block
    if (check(p, TOK_MATCH)) {
        advance_tok(p);
        AstNode *node = ast_new(p->arena, NODE_MATCH_STMT, loc);
        node->match_stmt.subject = parse_expr(p);
        skip_newlines(p);
        expect(p, TOK_INDENT);

        struct MatchArm *arms = NULL;
        int arm_count = 0;
        int arm_cap = 0;

        while (!check(p, TOK_DEDENT) && !at_end(p)) {
            skip_newlines(p);
            if (check(p, TOK_DEDENT)) break;

            struct MatchArm arm = {0};

            // Check for wildcard _
            Token *t2 = current(p);
            if (t2->kind == TOK_IDENT && t2->length == 1 && t2->start[0] == '_') {
                advance_tok(p);
                arm.is_wildcard = true;
                arm.pattern = NULL;
            } else {
                arm.is_wildcard = false;
                arm.pattern = parse_expr(p);
            }

            expect(p, TOK_FAT_ARROW);

            // Body: if next is NEWLINE+INDENT, parse block; otherwise single expr
            if (check(p, TOK_NEWLINE) || check(p, TOK_INDENT)) {
                skip_newlines(p);
                if (check(p, TOK_INDENT)) {
                    arm.body = parse_block(p);
                } else {
                    arm.body = parse_expr(p);
                }
            } else {
                arm.body = parse_expr(p);
            }

            if (arm_count >= arm_cap) {
                arm_cap = arm_cap ? arm_cap * 2 : 8;
                struct MatchArm *na = arena_alloc(p->arena, sizeof(struct MatchArm) * arm_cap);
                if (arms) memcpy(na, arms, sizeof(struct MatchArm) * arm_count);
                arms = na;
            }
            arms[arm_count++] = arm;
            skip_newlines(p);
        }

        expect(p, TOK_DEDENT);
        node->match_stmt.arms = arms;
        node->match_stmt.arm_count = arm_count;
        return node;
    }

    // Variable declaration or assignment:
    // x = expr            (immutable decl)
    // x: type = expr      (immutable decl with type)
    // x! = expr           (mutable decl)
    // x! += expr          (compound assignment)
    if ((check(p, TOK_IDENT) || check(p, TOK_IDENT_MUT)) &&
        (peek_at(p, 1)->kind == TOK_EQ || peek_at(p, 1)->kind == TOK_COLON ||
         peek_at(p, 1)->kind == TOK_PLUS_EQ || peek_at(p, 1)->kind == TOK_MINUS_EQ ||
         peek_at(p, 1)->kind == TOK_STAR_EQ || peek_at(p, 1)->kind == TOK_SLASH_EQ)) {

        Token *name_tok = advance_tok(p);
        char *name = tok_str(p, name_tok);
        bool is_mut = (name_tok->kind == TOK_IDENT_MUT);

        // Compound assignment: x! += expr
        if (check(p, TOK_PLUS_EQ) || check(p, TOK_MINUS_EQ) ||
            check(p, TOK_STAR_EQ) || check(p, TOK_SLASH_EQ)) {
            Token *op_tok = advance_tok(p);
            AstNode *node = ast_new(p->arena, NODE_ASSIGN, loc);
            node->assign.name = name;
            node->assign.op = op_tok->kind;
            node->assign.value = parse_expr(p);
            return node;
        }

        // Type annotation: x: type = expr
        AstNode *type_ann = NULL;
        if (match_tok(p, TOK_COLON)) {
            type_ann = parse_type(p);
        }

        expect(p, TOK_EQ);

        AstNode *node = ast_new(p->arena, NODE_VAR_DECL, loc);
        node->var_decl.name = name;
        node->var_decl.is_mutable = is_mut;
        node->var_decl.type_ann = type_ann;
        node->var_decl.init_expr = parse_expr(p);
        return node;
    }

    // Expression statement (function calls, etc.)
    AstNode *expr = parse_expr(p);

    // Check for index assignment: expr[idx] = val
    if (expr->kind == NODE_INDEX_EXPR && check(p, TOK_EQ)) {
        advance_tok(p); // skip =
        AstNode *node = ast_new(p->arena, NODE_INDEX_ASSIGN, loc);
        node->index_assign.object = expr->index_expr.object;
        node->index_assign.index = expr->index_expr.index;
        node->index_assign.value = parse_expr(p);
        return node;
    }

    // Check for field assignment: expr.field {=, +=, -=, *=, /=} val.
    // The same path covers `s.x = v`, `s.x! = v`, `self.radius! += amount`,
    // etc. The trailing `!` on the field name is already absorbed by the
    // postfix-dot loop (it accepts TOK_IDENT_MUT and strips the `!`).
    if (expr->kind == NODE_FIELD_EXPR &&
        (check(p, TOK_EQ) || check(p, TOK_PLUS_EQ) || check(p, TOK_MINUS_EQ) ||
         check(p, TOK_STAR_EQ) || check(p, TOK_SLASH_EQ))) {
        Token *op_tok = advance_tok(p);
        AstNode *node = ast_new(p->arena, NODE_FIELD_ASSIGN, loc);
        node->field_assign.object = expr->field_expr.object;
        node->field_assign.field_name = expr->field_expr.field_name;
        node->field_assign.value = parse_expr(p);
        node->field_assign.op = op_tok->kind;
        return node;
    }

    AstNode *node = ast_new(p->arena, NODE_EXPR_STMT, loc);
    node->expr_stmt.expr = expr;
    return node;
}

// --- Top-level parsing ---

static Param *parse_params(Parser *p, int *count) {
    Param *params = NULL;
    int param_count = 0;
    int param_cap = 0;

    expect(p, TOK_LPAREN);

    if (!check(p, TOK_RPAREN)) {
        do {
            Token *name_tok = current(p);
            if (name_tok->kind != TOK_IDENT && name_tok->kind != TOK_IDENT_MUT) {
                mix_error(tok_loc(p), "expected parameter name");
                break;
            }
            advance_tok(p);

            Param param = {0};
            param.name = tok_str(p, name_tok);
            param.is_mutable = (name_tok->kind == TOK_IDENT_MUT);

            // Optional type annotation
            if (match_tok(p, TOK_COLON)) {
                param.type = parse_type(p);
            }

            // Optional default value: `name: T = expr`. Sema fills in
            // missing trailing args at the call site with these.
            if (match_tok(p, TOK_EQ)) {
                param.default_value = parse_expr(p);
            }

            if (param_count >= param_cap) {
                param_cap = param_cap ? param_cap * 2 : 8;
                Param *new_params = arena_alloc(p->arena, sizeof(Param) * param_cap);
                if (params) memcpy(new_params, params, sizeof(Param) * param_count);
                params = new_params;
            }
            params[param_count++] = param;
        } while (match_tok(p, TOK_COMMA));
    }

    expect(p, TOK_RPAREN);
    *count = param_count;
    return params;
}

static AstNode *parse_fn_decl(Parser *p) {
    SrcLoc loc = tok_loc(p);
    Token *name_tok = advance_tok(p); // ident

    AstNode *node = ast_new(p->arena, NODE_FN_DECL, loc);
    node->fn_decl.name = tok_str(p, name_tok);
    if (name_tok->kind == TOK_IDENT_MUT) node->fn_decl.has_mutation = true;

    // Parse parameters
    node->fn_decl.params = parse_params(p, &node->fn_decl.param_count);

    // Optional return type: -> type
    if (match_tok(p, TOK_ARROW)) {
        node->fn_decl.return_type = parse_type(p);
    }

    // Optional effect markers: ~ !
    while (check(p, TOK_TILDE) || check(p, TOK_BANG)) {
        if (match_tok(p, TOK_TILDE)) node->fn_decl.has_side_effects = true;
        if (match_tok(p, TOK_BANG)) node->fn_decl.has_mutation = true;
    }

    skip_newlines(p);

    // Parse body
    node->fn_decl.body = parse_block(p);

    return node;
}

static AstNode *parse_extern_fn_decl(Parser *p) {
    SrcLoc loc = tok_loc(p);
    Token *name_tok = current(p);
    if (name_tok->kind != TOK_IDENT) {
        mix_error(loc, "expected function name in extern block");
        return ast_new(p->arena, NODE_NONE_LIT, loc);
    }
    advance_tok(p);

    AstNode *node = ast_new(p->arena, NODE_EXTERN_FN_DECL, loc);
    node->extern_fn_decl.name = tok_str(p, name_tok);
    if (name_tok->kind == TOK_IDENT_MUT) node->extern_fn_decl.has_mutation = true;

    // Optional C symbol name: gl_Clear "glad_glClear" (...)
    if (check(p, TOK_STRING_LIT)) {
        Token *cname_tok = advance_tok(p);
        node->extern_fn_decl.c_name = arena_strndup(p->arena, cname_tok->start, cname_tok->length);
    }

    node->extern_fn_decl.params = parse_params(p, &node->extern_fn_decl.param_count);

    if (match_tok(p, TOK_ARROW)) {
        node->extern_fn_decl.return_type = parse_type(p);
    }

    while (check(p, TOK_TILDE) || check(p, TOK_BANG)) {
        if (match_tok(p, TOK_TILDE)) node->extern_fn_decl.has_side_effects = true;
        if (match_tok(p, TOK_BANG)) node->extern_fn_decl.has_mutation = true;
    }

    return node;
}

static AstNode *parse_extern_block(Parser *p) {
    SrcLoc loc = tok_loc(p);
    expect(p, TOK_EXTERN);

    // Library name string
    Token *lib_tok = expect(p, TOK_STRING_LIT);

    AstNode *node = ast_new(p->arena, NODE_EXTERN_BLOCK, loc);
    node->extern_block.lib_name = arena_strndup(p->arena, lib_tok->start, lib_tok->length);

    skip_newlines(p);
    expect(p, TOK_INDENT);

    AstNode **decls = NULL;
    int decl_count = 0;
    int decl_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        AstNode *decl = parse_extern_fn_decl(p);
        if (decl_count >= decl_cap) {
            decl_cap = decl_cap ? decl_cap * 2 : 8;
            AstNode **new_decls = arena_alloc(p->arena, sizeof(AstNode*) * decl_cap);
            if (decls) memcpy(new_decls, decls, sizeof(AstNode*) * decl_count);
            decls = new_decls;
        }
        decls[decl_count++] = decl;

        skip_newlines(p);
    }

    expect(p, TOK_DEDENT);

    node->extern_block.decls = decls;
    node->extern_block.decl_count = decl_count;
    return node;
}

// Parse shape declaration:
//   shape Vec2
//       x, y: float
//   or with methods:
//   shape Circle
//       radius: float
//       area() -> float
//           3.14 * radius * radius
static AstNode *parse_shape_decl(Parser *p) {
    SrcLoc loc = tok_loc(p);
    // Accept either 'shape' or 'union' keyword
    if (check(p, TOK_UNION))
        advance_tok(p);
    else
        expect(p, TOK_SHAPE);

    Token *name_tok = expect(p, TOK_IDENT);
    AstNode *node = ast_new(p->arena, NODE_SHAPE_DECL, loc);
    node->shape_decl.name = tok_str(p, name_tok);

    // Optional generic type params: shape Name[T, U]
    if (check(p, TOK_LBRACKET)) {
        advance_tok(p); // skip '['
        char **type_params = NULL;
        int tp_count = 0;
        int tp_cap = 0;
        do {
            Token *tp = expect(p, TOK_IDENT);
            if (tp_count >= tp_cap) {
                tp_cap = tp_cap ? tp_cap * 2 : 4;
                char **np = arena_alloc(p->arena, sizeof(char*) * tp_cap);
                if (type_params) memcpy(np, type_params, sizeof(char*) * tp_count);
                type_params = np;
            }
            type_params[tp_count++] = tok_str(p, tp);
        } while (match_tok(p, TOK_COMMA));
        expect(p, TOK_RBRACKET);
        node->shape_decl.type_params = type_params;
        node->shape_decl.type_param_count = tp_count;
    }

    skip_newlines(p);
    expect(p, TOK_INDENT);

    ShapeField *fields = NULL;
    int field_count = 0;
    int field_cap = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_cap = 0;

    ShapeVariantDecl *variants = NULL;
    int variant_count = 0;
    int variant_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        // Variant: UpperCase(field: type, ...) — no body
        // Detect: IDENT( where first param has : and after ) is NEWLINE/DEDENT
        if (check(p, TOK_IDENT) && peek_at(p, 1)->kind == TOK_LPAREN) {
            // Check if this is a variant (has field: type params) or method (has body)
            // Heuristic: if first char is uppercase and param has colon, likely variant
            // But we need to look ahead past the params to check for body
            Token *name_t = current(p);
            bool starts_upper = (name_t->start[0] >= 'A' && name_t->start[0] <= 'Z');

            if (starts_upper) {
                // Save position, try parsing as variant
                int save_pos2 = p->pos;
                char *vname = tok_str(p, name_t);
                advance_tok(p); // skip name

                int pc = 0;
                Param *vparams = parse_params(p, &pc);

                // Check if next is NOT -> or INDENT (would be a method)
                skip_newlines(p);
                bool has_body = check(p, TOK_INDENT) || check(p, TOK_ARROW);

                if (!has_body && pc > 0) {
                    // It's a variant declaration
                    if (variant_count >= variant_cap) {
                        variant_cap = variant_cap ? variant_cap * 2 : 8;
                        ShapeVariantDecl *nv = arena_alloc(p->arena, sizeof(ShapeVariantDecl) * variant_cap);
                        if (variants) memcpy(nv, variants, sizeof(ShapeVariantDecl) * variant_count);
                        variants = nv;
                    }
                    variants[variant_count].name = vname;
                    variants[variant_count].fields = vparams;
                    variants[variant_count].field_count = pc;
                    variant_count++;
                    continue;
                }

                // Not a variant — restore and fall through to method parsing
                p->pos = save_pos2;
            }
        }

        // Operator method: +(other) or ==(other) etc.
        TokenKind op_kind = current(p)->kind;
        bool is_op_method = false;
        const char *op_name = NULL;
        if ((op_kind == TOK_PLUS || op_kind == TOK_MINUS || op_kind == TOK_STAR ||
             op_kind == TOK_SLASH || op_kind == TOK_PERCENT || op_kind == TOK_EQEQ ||
             op_kind == TOK_NEQ || op_kind == TOK_LT || op_kind == TOK_GT ||
             op_kind == TOK_LTE || op_kind == TOK_GTE) &&
            peek_at(p, 1)->kind == TOK_LPAREN) {
            is_op_method = true;
            switch (op_kind) {
                case TOK_PLUS: op_name = "op_add"; break;
                case TOK_MINUS: op_name = "op_sub"; break;
                case TOK_STAR: op_name = "op_mul"; break;
                case TOK_SLASH: op_name = "op_div"; break;
                case TOK_PERCENT: op_name = "op_mod"; break;
                case TOK_EQEQ: op_name = "op_eq"; break;
                case TOK_NEQ: op_name = "op_neq"; break;
                case TOK_LT: op_name = "op_lt"; break;
                case TOK_GT: op_name = "op_gt"; break;
                case TOK_LTE: op_name = "op_lte"; break;
                case TOK_GTE: op_name = "op_gte"; break;
                default: op_name = "op_unknown"; break;
            }
        }

        if (is_op_method) {
            SrcLoc method_loc = tok_loc(p);
            advance_tok(p); // skip operator token
            // Parse like a function: (params) [-> type] [~ !] INDENT body DEDENT
            AstNode *method = ast_new(p->arena, NODE_FN_DECL, method_loc);
            method->fn_decl.name = arena_strdup(p->arena, op_name);
            method->fn_decl.params = parse_params(p, &method->fn_decl.param_count);
            if (match_tok(p, TOK_ARROW)) method->fn_decl.return_type = parse_type(p);
            while (check(p, TOK_TILDE) || check(p, TOK_BANG)) {
                if (match_tok(p, TOK_TILDE)) method->fn_decl.has_side_effects = true;
                if (match_tok(p, TOK_BANG)) method->fn_decl.has_mutation = true;
            }
            skip_newlines(p);
            method->fn_decl.body = parse_block(p);

            if (method_count >= method_cap) {
                method_cap = method_cap ? method_cap * 2 : 8;
                AstNode **nm = arena_alloc(p->arena, sizeof(AstNode*) * method_cap);
                if (methods) memcpy(nm, methods, sizeof(AstNode*) * method_count);
                methods = nm;
            }
            methods[method_count++] = method;
            skip_newlines(p);
            continue;
        }

        // Method: ident( or ident!( → parse as fn_decl
        if ((check(p, TOK_IDENT) || check(p, TOK_IDENT_MUT)) && peek_at(p, 1)->kind == TOK_LPAREN) {
            AstNode *method = parse_fn_decl(p);
            if (method_count >= method_cap) {
                method_cap = method_cap ? method_cap * 2 : 8;
                AstNode **nm = arena_alloc(p->arena, sizeof(AstNode*) * method_cap);
                if (methods) memcpy(nm, methods, sizeof(AstNode*) * method_count);
                methods = nm;
            }
            methods[method_count++] = method;
            skip_newlines(p);
            continue;
        }

        // Field: name[, name...]: type
        // Collect names until we hit ':'
        char *names[32];
        int name_count = 0;

        Token *ft = expect(p, TOK_IDENT);
        names[name_count++] = tok_str(p, ft);

        while (match_tok(p, TOK_COMMA)) {
            Token *nt = expect(p, TOK_IDENT);
            names[name_count++] = tok_str(p, nt);
        }

        expect(p, TOK_COLON);
        AstNode *ftype = parse_type(p);

        // Create a field entry for each name
        for (int i = 0; i < name_count; i++) {
            if (field_count >= field_cap) {
                field_cap = field_cap ? field_cap * 2 : 16;
                ShapeField *nf = arena_alloc(p->arena, sizeof(ShapeField) * field_cap);
                if (fields) memcpy(nf, fields, sizeof(ShapeField) * field_count);
                fields = nf;
            }
            fields[field_count].name = names[i];
            fields[field_count].type = ftype;
            fields[field_count].offset = 0;
            fields[field_count].size = 0;
            field_count++;
        }

        skip_newlines(p);
    }

    expect(p, TOK_DEDENT);

    node->shape_decl.fields = fields;
    node->shape_decl.field_count = field_count;
    node->shape_decl.methods = methods;
    node->shape_decl.method_count = method_count;
    node->shape_decl.variants = variants;
    node->shape_decl.variant_count = variant_count;
    return node;
}

static AstNode *parse_top_level(Parser *p) {
    skip_newlines(p);
    if (at_end(p)) return NULL;

    // use module.path
    // use alias module.path
    // use module: name1, name2
    if (check(p, TOK_USE)) {
        SrcLoc loc = tok_loc(p);
        advance_tok(p); // skip 'use'

        // use c "header.h" [link "lib"]
        if (check(p, TOK_IDENT) && current(p)->length == 1 && current(p)->start[0] == 'c'
            && peek_at(p, 1)->kind == TOK_STRING_LIT) {
            advance_tok(p); // skip 'c'
            Token *header_tok = expect(p, TOK_STRING_LIT);
            AstNode *node = ast_new(p->arena, NODE_USE_C_DECL, loc);
            node->use_c_decl.header_path = arena_strndup(p->arena, header_tok->start, header_tok->length);
            node->use_c_decl.lib_name = NULL;
            node->use_c_decl.source_path = NULL;
            // Optional: link "libname"
            if (check(p, TOK_IDENT) && current(p)->length == 4
                && strncmp(current(p)->start, "link", 4) == 0) {
                advance_tok(p); // skip 'link'
                Token *lib_tok = expect(p, TOK_STRING_LIT);
                node->use_c_decl.lib_name = arena_strndup(p->arena, lib_tok->start, lib_tok->length);
            }
            // Optional: source "file.c"
            if (check(p, TOK_IDENT) && current(p)->length == 6
                && strncmp(current(p)->start, "source", 6) == 0) {
                advance_tok(p); // skip 'source'
                Token *src_tok = expect(p, TOK_STRING_LIT);
                node->use_c_decl.source_path = arena_strndup(p->arena, src_tok->start, src_tok->length);
            }
            return node;
        }

        AstNode *node = ast_new(p->arena, NODE_USE_DECL, loc);
        node->use_decl.alias = NULL;
        node->use_decl.imports = NULL;
        node->use_decl.import_count = 0;

        // Path-style use: starts with `..`. Slash-separated segments,
        // each segment is `..` or an identifier. Lets a demo do
        // `use ../../mixel` to import a sibling-of-grandparent module.
        // No alias here (filesystem-shaped paths and aliases mix awkwardly);
        // selective imports (`: name1, name2`) still work via the colon
        // branch below.
        if (check(p, TOK_DOTDOT)) {
            char path[256];
            int off = 0;
            // First segment must be `..` (we already see TOK_DOTDOT).
            advance_tok(p);
            off += snprintf(path + off, sizeof(path) - off, "..");
            while (match_tok(p, TOK_SLASH)) {
                if (off >= (int)sizeof(path) - 4) {
                    mix_error(loc, "use path too long");
                    break;
                }
                if (check(p, TOK_DOTDOT)) {
                    advance_tok(p);
                    off += snprintf(path + off, sizeof(path) - off, "/..");
                } else {
                    // Accept any word-like token (TOK_IDENT or a keyword
                    // whose source text starts with a letter/underscore) so
                    // directory names like `shared` or `int` work even
                    // though they collide with reserved words.
                    Token *seg = current(p);
                    if (seg->length > 0 &&
                        (((unsigned char)seg->start[0] >= 'A' &&
                          (unsigned char)seg->start[0] <= 'Z') ||
                         ((unsigned char)seg->start[0] >= 'a' &&
                          (unsigned char)seg->start[0] <= 'z') ||
                         seg->start[0] == '_')) {
                        advance_tok(p);
                        off += snprintf(path + off, sizeof(path) - off,
                                        "/%.*s", seg->length, seg->start);
                    } else {
                        mix_error(tok_loc(p),
                                  "expected identifier or `..` after `/` in use path");
                        break;
                    }
                }
            }
            node->use_decl.module_path = arena_strdup(p->arena, path);
            // Fall through to colon check below for selective imports.
            goto use_decl_colon_check;
        }

        // First identifier
        Token *first = expect(p, TOK_IDENT);
        char path[256];
        snprintf(path, sizeof(path), "%.*s", first->length, first->start);

        // Check for: use alias path.to.module (second ident without dot)
        // vs: use path.to.module[: name1, name2]
        // vs: use module: name1, name2 (colon for selective)
        if (check(p, TOK_DOT)) {
            // use path.to.module[: name1, name2]
            while (match_tok(p, TOK_DOT)) {
                Token *seg = expect(p, TOK_IDENT);
                int len = (int)strlen(path);
                snprintf(path + len, sizeof(path) - len, ".%.*s", seg->length, seg->start);
            }
            node->use_decl.module_path = arena_strdup(p->arena, path);
            // Fall through to colon check below
        } else if (check(p, TOK_IDENT)) {
            // use alias path.to.module[: ...]
            node->use_decl.alias = arena_strdup(p->arena, path);
            Token *path_start = expect(p, TOK_IDENT);
            snprintf(path, sizeof(path), "%.*s", path_start->length, path_start->start);
            while (match_tok(p, TOK_DOT)) {
                Token *seg = expect(p, TOK_IDENT);
                int len = (int)strlen(path);
                snprintf(path + len, sizeof(path) - len, ".%.*s", seg->length, seg->start);
            }
            node->use_decl.module_path = arena_strdup(p->arena, path);
            // Fall through to colon check
        } else {
            // use module (simple single-segment)
            node->use_decl.module_path = arena_strdup(p->arena, path);
        }

use_decl_colon_check:
        // Optional selective imports: ... : name1, name2
        if (match_tok(p, TOK_COLON)) {
            // Disallow combining with alias for now — it's confusing semantically.
            if (node->use_decl.alias) {
                mix_error(loc, "selective imports (':') cannot be combined with an alias");
            }
            char **imports = NULL;
            int import_count = 0;
            int import_cap = 0;
            do {
                Token *name = expect(p, TOK_IDENT);
                if (import_count >= import_cap) {
                    import_cap = import_cap ? import_cap * 2 : 8;
                    char **ni = arena_alloc(p->arena, sizeof(char*) * import_cap);
                    if (imports) memcpy(ni, imports, sizeof(char*) * import_count);
                    imports = ni;
                }
                imports[import_count++] = tok_str(p, name);
            } while (match_tok(p, TOK_COMMA));

            node->use_decl.imports = imports;
            node->use_decl.import_count = import_count;
        }

        return node;
    }

    // pub — marks next declaration as public
    bool is_pub = false;
    if (check(p, TOK_PUB)) {
        advance_tok(p);
        is_pub = true;
    }

    // Module-level mutable: `name! = expr` or `pub name! = expr`. Constant
    // initializer only (validated in sema). The parser produces a regular
    // NODE_VAR_DECL with is_global set so sema/emitters can treat it as a
    // module-scope variable rather than a stack local.
    if (check(p, TOK_IDENT_MUT) && peek_at(p, 1)->kind == TOK_EQ) {
        SrcLoc loc = tok_loc(p);
        Token *name_tok = advance_tok(p);
        advance_tok(p); // skip =
        AstNode *node = ast_new(p->arena, NODE_VAR_DECL, loc);
        node->var_decl.name = tok_str(p, name_tok);
        node->var_decl.is_mutable = true;
        node->var_decl.is_pub = is_pub;
        node->var_decl.is_global = true;
        node->var_decl.init_expr = parse_expr(p);
        return node;
    }

    // extern "lib"
    if (check(p, TOK_EXTERN)) {
        return parse_extern_block(p);
    }

    // shape Name
    if (check(p, TOK_SHAPE)) {
        AstNode *decl = parse_shape_decl(p);
        decl->shape_decl.is_pub = is_pub;
        return decl;
    }

    // union Name
    if (check(p, TOK_UNION)) {
        AstNode *decl = parse_shape_decl(p);
        decl->shape_decl.is_pub = is_pub;
        decl->shape_decl.is_union = true;
        return decl;
    }

    // @const or @T generic type params
    if (check(p, TOK_AT)) {
        SrcLoc at_loc = tok_loc(p);
        advance_tok(p); // skip @
        Token *kw = current(p);

        // @const NAME = expr
        if (kw->kind == TOK_IDENT && kw->length == 5 && memcmp(kw->start, "const", 5) == 0) {
            advance_tok(p); // skip 'const'
            Token *name_tok = expect(p, TOK_IDENT);
            expect(p, TOK_EQ);
            AstNode *node = ast_new(p->arena, NODE_CONST_DECL, at_loc);
            node->const_decl.name = tok_str(p, name_tok);
            node->const_decl.value = parse_expr(p);
            node->const_decl.is_pub = is_pub;
            return node;
        }

        // @os, @arch, @debug, @release — conditional compilation
        if (kw->kind == TOK_IDENT && (
            (kw->length == 2 && memcmp(kw->start, "os", 2) == 0) ||
            (kw->length == 4 && memcmp(kw->start, "arch", 4) == 0) ||
            (kw->length == 5 && memcmp(kw->start, "debug", 5) == 0) ||
            (kw->length == 7 && memcmp(kw->start, "release", 7) == 0))) {
            char *cond_name = tok_str(p, kw);
            advance_tok(p); // skip condition name
            char *cond_value = NULL;

            // Optional == "value"
            if (check(p, TOK_EQEQ)) {
                advance_tok(p); // skip ==
                Token *val_tok = expect(p, TOK_STRING_LIT);
                cond_value = arena_strndup(p->arena, val_tok->start, val_tok->length);
            }

            skip_newlines(p);
            expect(p, TOK_INDENT);

            AstNode **decls = NULL;
            int decl_count = 0;
            int decl_cap = 0;

            while (!check(p, TOK_DEDENT) && !at_end(p)) {
                skip_newlines(p);
                if (check(p, TOK_DEDENT)) break;
                AstNode *d = parse_top_level(p);
                if (d) {
                    if (decl_count >= decl_cap) {
                        decl_cap = decl_cap ? decl_cap * 2 : 8;
                        AstNode **nd = arena_alloc(p->arena, sizeof(AstNode*) * decl_cap);
                        if (decls) memcpy(nd, decls, sizeof(AstNode*) * decl_count);
                        decls = nd;
                    }
                    decls[decl_count++] = d;
                }
                skip_newlines(p);
            }
            expect(p, TOK_DEDENT);

            AstNode *node = ast_new(p->arena, NODE_COND_DECL, at_loc);
            node->cond_decl.condition_name = cond_name;
            node->cond_decl.condition_value = cond_value;
            node->cond_decl.decls = decls;
            node->cond_decl.decl_count = decl_count;
            node->cond_decl.active = false;
            return node;
        }

        // @T or @T, K — generic type parameters before a function
        if (kw->kind == TOK_IDENT) {
            char **type_params = NULL;
            int tp_count = 0;
            int tp_cap = 0;

            do {
                Token *tp = expect(p, TOK_IDENT);
                if (tp_count >= tp_cap) {
                    tp_cap = tp_cap ? tp_cap * 2 : 4;
                    char **np = arena_alloc(p->arena, sizeof(char*) * tp_cap);
                    if (type_params) memcpy(np, type_params, sizeof(char*) * tp_count);
                    type_params = np;
                }
                type_params[tp_count++] = tok_str(p, tp);
            } while (match_tok(p, TOK_COMMA));

            // Parse optional 'has' constraints: @T has +, ==
            char **constraints = NULL;
            int cons_count = 0;
            int cons_cap = 0;

            skip_newlines(p);
            if (check(p, TOK_IDENT) && current(p)->length == 3 &&
                memcmp(current(p)->start, "has", 3) == 0) {
                advance_tok(p); // skip 'has'
                do {
                    Token *ct = current(p);
                    char *cname = NULL;
                    if (ct->kind == TOK_IDENT) {
                        cname = tok_str(p, ct);
                        advance_tok(p);
                    } else if (ct->kind == TOK_PLUS) {
                        cname = "+"; advance_tok(p);
                    } else if (ct->kind == TOK_MINUS) {
                        cname = "-"; advance_tok(p);
                    } else if (ct->kind == TOK_STAR) {
                        cname = "*"; advance_tok(p);
                    } else if (ct->kind == TOK_SLASH) {
                        cname = "/"; advance_tok(p);
                    } else if (ct->kind == TOK_EQEQ) {
                        cname = "=="; advance_tok(p);
                    } else if (ct->kind == TOK_NEQ) {
                        cname = "!="; advance_tok(p);
                    } else if (ct->kind == TOK_LT) {
                        cname = "<"; advance_tok(p);
                    } else if (ct->kind == TOK_GT) {
                        cname = ">"; advance_tok(p);
                    } else if (ct->kind == TOK_LTE) {
                        cname = "<="; advance_tok(p);
                    } else if (ct->kind == TOK_GTE) {
                        cname = ">="; advance_tok(p);
                    }
                    if (cname) {
                        if (cons_count >= cons_cap) {
                            cons_cap = cons_cap ? cons_cap * 2 : 8;
                            char **nc = arena_alloc(p->arena, sizeof(char*) * cons_cap);
                            if (constraints) memcpy(nc, constraints, sizeof(char*) * cons_count);
                            constraints = nc;
                        }
                        constraints[cons_count++] = cname;
                    }
                } while (match_tok(p, TOK_COMMA));
            }

            skip_newlines(p);

            // Next should be a function declaration
            if (check(p, TOK_IDENT) && peek_at(p, 1)->kind == TOK_LPAREN) {
                AstNode *decl = parse_fn_decl(p);
                decl->fn_decl.is_pub = is_pub;
                decl->fn_decl.type_params = type_params;
                decl->fn_decl.type_param_count = tp_count;
                decl->fn_decl.constraints = constraints;
                decl->fn_decl.constraint_count = cons_count;
                return decl;
            }

            mix_error(at_loc, "expected function declaration after @%s", type_params[0]);
            return NULL;
        }

        mix_error(at_loc, "expected 'const' or type parameter after '@'");
        advance_tok(p);
        return NULL;
    }

    // type Name = Type
    if (check(p, TOK_TYPE)) {
        SrcLoc type_loc = tok_loc(p);
        advance_tok(p); // skip 'type'
        Token *name_tok = expect(p, TOK_IDENT);
        expect(p, TOK_EQ);
        AstNode *node = ast_new(p->arena, NODE_TYPE_ALIAS, type_loc);
        node->type_alias.name = tok_str(p, name_tok);
        node->type_alias.target_type = parse_type(p);
        node->type_alias.is_pub = is_pub;
        return node;
    }

    // Function declaration: ident(...) [-> type] [~ !] NEWLINE INDENT
    if (check(p, TOK_IDENT) && peek_at(p, 1)->kind == TOK_LPAREN) {
        AstNode *decl = parse_fn_decl(p);
        decl->fn_decl.is_pub = is_pub;
        return decl;
    }

    mix_error(tok_loc(p), "expected function, shape, use, or extern declaration at top level, got %s",
              token_kind_name(current(p)->kind));
    advance_tok(p);
    return NULL;
}

AstNode *parser_parse(Parser *p) {
    SrcLoc loc = {p->filename, 1, 1};
    AstNode *program = ast_new(p->arena, NODE_PROGRAM, loc);

    AstNode **decls = NULL;
    int decl_count = 0;
    int decl_cap = 0;

    while (!at_end(p)) {
        AstNode *decl = parse_top_level(p);
        if (!decl) continue;

        if (decl_count >= decl_cap) {
            decl_cap = decl_cap ? decl_cap * 2 : 16;
            AstNode **new_decls = arena_alloc(p->arena, sizeof(AstNode*) * decl_cap);
            if (decls) memcpy(new_decls, decls, sizeof(AstNode*) * decl_count);
            decls = new_decls;
        }
        decls[decl_count++] = decl;
    }

    program->program.decls = decls;
    program->program.decl_count = decl_count;
    return program;
}
