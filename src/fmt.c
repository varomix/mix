#include "fmt.h"
#include "arena.h"
#include "errors.h"
#include "lexer.h"
#include "token.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

// --- Comment side-channel ---
//
// The lexer skips `// ...` comments, so we re-scan the source to recover
// them. For each comment we record (line, col, end_col, text) so we can
// re-emit it at the right place — either standalone on its own line, or
// trailing on the line it shares with code.

typedef struct {
    int line;          // 1-based
    int col;           // 1-based, where // starts
    char *text;        // owned malloc'd string of the comment body (incl. //)
    bool standalone;   // true if no non-whitespace before // on this line
} FmtComment;

typedef struct {
    FmtComment *items;
    int count;
    int cap;
} FmtCommentList;

static void cl_push(FmtCommentList *cl, FmtComment c) {
    if (cl->count >= cl->cap) {
        cl->cap = cl->cap ? cl->cap * 2 : 32;
        cl->items = realloc(cl->items, sizeof(FmtComment) * cl->cap);
    }
    cl->items[cl->count++] = c;
}

static void cl_free(FmtCommentList *cl) {
    for (int i = 0; i < cl->count; i++) free(cl->items[i].text);
    free(cl->items);
}

// Walk the source looking for // comments. We respect string literals so a
// // inside a string isn't mistaken for a comment.
static void scan_comments(const char *src, FmtCommentList *cl) {
    const char *p = src;
    int line = 1;
    int col = 1;
    bool line_has_code = false;
    while (*p) {
        if (*p == '\n') {
            line++; col = 1; p++;
            line_has_code = false;
            continue;
        }
        if (*p == '"') {
            // Skip string literal (handle escapes)
            p++; col++;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '\\' && p[1]) { p += 2; col += 2; continue; }
                p++; col++;
            }
            if (*p == '"') { p++; col++; }
            line_has_code = true;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            int start_col = col;
            const char *cstart = p;
            while (*p && *p != '\n') { p++; col++; }
            int len = (int)(p - cstart);
            // Strip trailing whitespace from the comment text.
            while (len > 0 && (cstart[len-1] == ' ' || cstart[len-1] == '\t')) len--;
            char *text = malloc(len + 1);
            memcpy(text, cstart, len);
            text[len] = '\0';
            FmtComment c = { line, start_col, text, !line_has_code };
            cl_push(cl, c);
            continue;
        }
        if (*p != ' ' && *p != '\t') line_has_code = true;
        if (*p == '\t') { col += 4 - ((col - 1) % 4); }
        else col++;
        p++;
    }
}

// --- Output buffer with simple write helpers ---

typedef struct {
    FILE *out;
    int indent_level;     // number of 4-space indents
    bool at_line_start;   // true if next write is the first non-indent on this line
} FmtOut;

static void fout_indent(FmtOut *o) {
    for (int i = 0; i < o->indent_level; i++) fputs("    ", o->out);
}

static void fout_str(FmtOut *o, const char *s) {
    if (o->at_line_start) {
        fout_indent(o);
        o->at_line_start = false;
    }
    fputs(s, o->out);
}

static void fout_strn(FmtOut *o, const char *s, int n) {
    if (o->at_line_start) {
        fout_indent(o);
        o->at_line_start = false;
    }
    fwrite(s, 1, n, o->out);
}

static void fout_newline(FmtOut *o) {
    fputc('\n', o->out);
    o->at_line_start = true;
}

// --- Spacing rules ---

// True if this token kind is followed by a space when joined with the next
// token. Conservative: defaults to "yes, space after" unless there's a
// reason not to.
static bool space_after(TokenKind k, TokenKind next) {
    // Open brackets: no space after.
    if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) return false;
    // Before any closing bracket: no space.
    if (next == TOK_RPAREN || next == TOK_RBRACKET || next == TOK_RBRACE) return false;
    // Before a comma: no space.
    if (next == TOK_COMMA) return false;
    // Comma: one space after (unless followed by a closing bracket — covered above).
    if (k == TOK_COMMA) return true;
    // Dots: no spaces around. Includes member access and ranges.
    if (k == TOK_DOT || next == TOK_DOT) return false;
    if (k == TOK_DOTDOT || next == TOK_DOTDOT) return false;
    if (k == TOK_DOTDOT_EQ || next == TOK_DOTDOT_EQ) return false;
    // ? and ~ and ! suffix operators: no space before; usually no space after.
    if (next == TOK_QUESTION) return false;
    if (next == TOK_BANG) return false;
    // @: directive prefix, no space after.
    if (k == TOK_AT) return false;
    // Function-call open paren: no space between IDENT and (.
    if (next == TOK_LPAREN &&
        (k == TOK_IDENT || k == TOK_IDENT_MUT ||
         k == TOK_RPAREN || k == TOK_RBRACKET)) return false;
    // Type subscript: Ident[Type] — no space between IDENT and [.
    if (next == TOK_LBRACKET && (k == TOK_IDENT || k == TOK_IDENT_MUT)) return false;
    // Colon: no space before, one space after.
    if (next == TOK_COLON) return false;
    if (k == TOK_COLON) return true;
    // Default: one space.
    return true;
}

// (Unary-prefix detection is handled inline in format_tokens via prev_prev.)


// --- Token text emission ---

static void emit_token_text(FmtOut *o, Token *t) {
    // String literals are stored with start pointing inside the quotes —
    // restore them.
    if (t->kind == TOK_STRING_LIT) {
        fout_str(o, "\"");
        fout_strn(o, t->start, t->length);
        fout_str(o, "\"");
        return;
    }
    fout_strn(o, t->start, t->length);
}

// Fixed-text tokens (keywords, operators) where t->start may not include
// a normalized form. Most tokens just use t->start[0..length] directly,
// which works because the source preserves their text.
//
// Exception: INDENT/DEDENT/NEWLINE have no text and never emit through here.

// --- Comment emission helpers ---

// Emit any standalone comments whose source line is <= `up_to_line`,
// starting from index `*idx`. Returns updated index.
static int emit_standalone_comments_until(FmtOut *o, FmtCommentList *cl,
                                          int idx, int up_to_line) {
    while (idx < cl->count && cl->items[idx].standalone &&
           cl->items[idx].line <= up_to_line) {
        fout_str(o, cl->items[idx].text);
        fout_newline(o);
        idx++;
    }
    return idx;
}

// True if `prev` puts the current operator in a position where it's a
// unary prefix (i.e., the operator's operand comes right after with no
// space). Examples: `-x` after `=`, `(`, `[`, `,`, `=>`, another binary op.
static bool is_unary_position(TokenKind prev) {
    switch (prev) {
        case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE:
        case TOK_COMMA: case TOK_COLON: case TOK_EQ: case TOK_FAT_ARROW:
        case TOK_ARROW: case TOK_PLUS: case TOK_MINUS:
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
        case TOK_EQEQ: case TOK_NEQ:
        case TOK_LT: case TOK_GT: case TOK_LTE: case TOK_GTE:
        case TOK_AND: case TOK_OR: case TOK_NOT:
        case TOK_PLUS_EQ: case TOK_MINUS_EQ: case TOK_STAR_EQ: case TOK_SLASH_EQ:
        case TOK_NEWLINE:
            return true;
        default:
            return false;
    }
}

static bool is_top_level_decl_start(TokenKind k) {
    return k == TOK_SHAPE || k == TOK_UNION || k == TOK_EXTERN ||
           k == TOK_USE || k == TOK_PUB || k == TOK_AT;
}

// --- Main format loop ---

static int format_tokens(Token *toks, int n_toks, FmtCommentList *cl, FILE *out) {
    FmtOut o = { out, 0, true };
    int comment_idx = 0;
    int last_emitted_line = 0;
    bool line_has_content = false;
    bool seen_first_decl = false;
    TokenKind prev_kind = TOK_NEWLINE;
    TokenKind prev_prev_kind = TOK_NEWLINE;

    // Emit all leading standalone comments before the first real token, so
    // they sit at the top of the file rather than after the first decl.
    {
        int first_code_line = INT_MAX;
        for (int j = 0; j < n_toks; j++) {
            if (toks[j].kind == TOK_NEWLINE || toks[j].kind == TOK_INDENT ||
                toks[j].kind == TOK_DEDENT || toks[j].kind == TOK_EOF) continue;
            first_code_line = toks[j].line;
            break;
        }
        comment_idx = emit_standalone_comments_until(&o, cl, comment_idx,
                                                     first_code_line - 1);
    }

    for (int i = 0; i < n_toks; i++) {
        Token *t = &toks[i];

        if (t->kind == TOK_EOF) break;

        if (t->kind == TOK_NEWLINE) {
            if (line_has_content && comment_idx < cl->count) {
                FmtComment *c = &cl->items[comment_idx];
                if (!c->standalone && c->line == last_emitted_line) {
                    fout_str(&o, "  ");
                    fout_str(&o, c->text);
                    comment_idx++;
                }
            }
            if (line_has_content) fout_newline(&o);
            line_has_content = false;
            int next_code_line = -1;
            for (int j = i + 1; j < n_toks; j++) {
                if (toks[j].kind == TOK_NEWLINE || toks[j].kind == TOK_INDENT ||
                    toks[j].kind == TOK_DEDENT) continue;
                next_code_line = toks[j].line;
                break;
            }
            if (next_code_line < 0) next_code_line = INT_MAX;
            comment_idx = emit_standalone_comments_until(
                &o, cl, comment_idx, next_code_line - 1);
            prev_prev_kind = prev_kind;
            prev_kind = TOK_NEWLINE;
            continue;
        }
        if (t->kind == TOK_INDENT) {
            o.indent_level++;
            continue;
        }
        if (t->kind == TOK_DEDENT) {
            if (o.indent_level > 0) o.indent_level--;
            continue;
        }

        // Insert a blank line between top-level declarations.
        if (o.indent_level == 0 && !line_has_content && seen_first_decl &&
            (is_top_level_decl_start(t->kind) ||
             t->kind == TOK_IDENT)) {
            fout_newline(&o);
        }

        if (line_has_content) {
            bool space = space_after(prev_kind, t->kind);
            // Suppress space between a unary prefix operator and its operand.
            if (space && (prev_kind == TOK_MINUS || prev_kind == TOK_STAR ||
                          prev_kind == TOK_AMPERSAND || prev_kind == TOK_NOT) &&
                is_unary_position(prev_prev_kind)) {
                space = false;
            }
            if (space) fout_str(&o, " ");
        }

        emit_token_text(&o, t);
        line_has_content = true;
        last_emitted_line = t->line;
        if (o.indent_level == 0) seen_first_decl = true;
        prev_prev_kind = prev_kind;
        prev_kind = t->kind;
    }
    if (line_has_content) fout_newline(&o);
    // Trailing standalone comments after the last token.
    while (comment_idx < cl->count) {
        if (cl->items[comment_idx].standalone) {
            fout_str(&o, cl->items[comment_idx].text);
            fout_newline(&o);
        }
        comment_idx++;
    }
    return 0;
}

// --- Public entry point ---

int mix_format(const char *source, const char *filename, FILE *out) {
    Arena arena = arena_create(64 * 1024);

    // Suppress lex/parse diagnostics — fmt should be silent on partial code.
    DiagnosticCallback prev_cb = NULL;
    void *prev_ud = NULL;
    errors_get_callback(&prev_cb, &prev_ud);
    errors_set_callback(NULL, NULL);
    errors_reset();

    Lexer lex = lexer_create(source, filename ? filename : "<stdin>", &arena);
    lexer_tokenize(&lex);
    if (mix_error_count() > 0) {
        errors_set_callback(prev_cb, prev_ud);
        arena_destroy(&arena);
        return 1;
    }

    FmtCommentList cl = {0};
    scan_comments(source, &cl);

    int rc = format_tokens(lex.tokens, lex.token_count, &cl, out);

    cl_free(&cl);
    free(lex.tokens);
    errors_set_callback(prev_cb, prev_ud);
    arena_destroy(&arena);
    return rc;
}
