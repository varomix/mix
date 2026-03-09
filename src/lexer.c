#include "lexer.h"
#include "errors.h"
#include <inttypes.h>

// --- Token kind names ---

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOK_NEWLINE:    return "NEWLINE";
        case TOK_INDENT:     return "INDENT";
        case TOK_DEDENT:     return "DEDENT";
        case TOK_EOF:        return "EOF";
        case TOK_INT_LIT:    return "INT_LIT";
        case TOK_FLOAT_LIT:  return "FLOAT_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        case TOK_TRUE:       return "TRUE";
        case TOK_FALSE:      return "FALSE";
        case TOK_NONE:       return "NONE";
        case TOK_IDENT:      return "IDENT";
        case TOK_IDENT_MUT:  return "IDENT_MUT";
        case TOK_IF:         return "IF";
        case TOK_ELSE:       return "ELSE";
        case TOK_WHILE:      return "WHILE";
        case TOK_FOR:        return "FOR";
        case TOK_IN:         return "IN";
        case TOK_MATCH:      return "MATCH";
        case TOK_BREAK:      return "BREAK";
        case TOK_CONTINUE:   return "CONTINUE";
        case TOK_DONE:       return "DONE";
        case TOK_SHAPE:      return "SHAPE";
        case TOK_EXTERN:     return "EXTERN";
        case TOK_USE:        return "USE";
        case TOK_PUB:        return "PUB";
        case TOK_TYPE:       return "TYPE";
        case TOK_ZONE:       return "ZONE";
        case TOK_DEFER:      return "DEFER";
        case TOK_UNSAFE:     return "UNSAFE";
        case TOK_AND:        return "AND";
        case TOK_OR:         return "OR";
        case TOK_NOT:        return "NOT";
        case TOK_GO:         return "GO";
        case TOK_RUN:        return "RUN";
        case TOK_WAIT:       return "WAIT";
        case TOK_STREAM:     return "STREAM";
        case TOK_YIELD:      return "YIELD";
        case TOK_SHARED:     return "SHARED";
        case TOK_REPEAT:     return "REPEAT";
        case TOK_AS:         return "AS";
        case TOK_THEN:       return "THEN";
        case TOK_SET:        return "SET";
        case TOK_INT:        return "INT";
        case TOK_FLOAT:      return "FLOAT";
        case TOK_BOOL:       return "BOOL";
        case TOK_BYTE:       return "BYTE";
        case TOK_STR:        return "STR";
        case TOK_INT8:       return "INT8";
        case TOK_INT16:      return "INT16";
        case TOK_INT32:      return "INT32";
        case TOK_INT64:      return "INT64";
        case TOK_UINT8:      return "UINT8";
        case TOK_UINT16:     return "UINT16";
        case TOK_UINT32:     return "UINT32";
        case TOK_UINT64:     return "UINT64";
        case TOK_FLOAT32:    return "FLOAT32";
        case TOK_FLOAT64:    return "FLOAT64";
        case TOK_PLUS:       return "PLUS";
        case TOK_MINUS:      return "MINUS";
        case TOK_STAR:       return "STAR";
        case TOK_SLASH:      return "SLASH";
        case TOK_PERCENT:    return "PERCENT";
        case TOK_EQ:         return "EQ";
        case TOK_EQEQ:      return "EQEQ";
        case TOK_NEQ:        return "NEQ";
        case TOK_LT:        return "LT";
        case TOK_GT:        return "GT";
        case TOK_LTE:       return "LTE";
        case TOK_GTE:       return "GTE";
        case TOK_PLUS_EQ:   return "PLUS_EQ";
        case TOK_MINUS_EQ:  return "MINUS_EQ";
        case TOK_STAR_EQ:   return "STAR_EQ";
        case TOK_SLASH_EQ:  return "SLASH_EQ";
        case TOK_ARROW:     return "ARROW";
        case TOK_FAT_ARROW: return "FAT_ARROW";
        case TOK_AMPERSAND: return "AMPERSAND";
        case TOK_DOTDOT:    return "DOTDOT";
        case TOK_DOTDOT_EQ: return "DOTDOT_EQ";
        case TOK_PIPE:      return "PIPE";
        case TOK_QUESTION:  return "QUESTION";
        case TOK_TILDE:     return "TILDE";
        case TOK_BANG:      return "BANG";
        case TOK_LPAREN:    return "LPAREN";
        case TOK_RPAREN:    return "RPAREN";
        case TOK_LBRACKET:  return "LBRACKET";
        case TOK_RBRACKET:  return "RBRACKET";
        case TOK_LBRACE:    return "LBRACE";
        case TOK_RBRACE:    return "RBRACE";
        case TOK_COLON:     return "COLON";
        case TOK_COMMA:     return "COMMA";
        case TOK_DOT:       return "DOT";
        case TOK_AT:        return "AT";
        case TOK_COUNT:     return "COUNT";
    }
    return "UNKNOWN";
}

// --- Lexer helpers ---

Lexer lexer_create(const char *source, const char *filename, Arena *arena) {
    Lexer lex = {0};
    lex.source = source;
    lex.current = source;
    lex.filename = filename;
    lex.line = 1;
    lex.col = 1;
    lex.indent_stack[0] = 0;
    lex.indent_top = 0;
    lex.paren_depth = 0;
    lex.token_capacity = 1024;
    lex.tokens = malloc(sizeof(Token) * lex.token_capacity);
    lex.token_count = 0;
    lex.at_line_start = true;
    lex.arena = arena;
    return lex;
}

static void emit_token(Lexer *lex, Token tok) {
    if (lex->token_count >= lex->token_capacity) {
        lex->token_capacity *= 2;
        lex->tokens = realloc(lex->tokens, sizeof(Token) * lex->token_capacity);
    }
    lex->tokens[lex->token_count++] = tok;
}

static Token make_token(Lexer *lex, TokenKind kind, const char *start, int length) {
    Token tok = {0};
    tok.kind = kind;
    tok.start = start;
    tok.length = length;
    tok.line = lex->line;
    tok.col = lex->col - length;
    if (tok.col < 1) tok.col = 1;
    return tok;
}

static char peek(Lexer *lex) {
    return *lex->current;
}

static char peek_next(Lexer *lex) {
    if (*lex->current == '\0') return '\0';
    return lex->current[1];
}

static char advance(Lexer *lex) {
    char c = *lex->current;
    lex->current++;
    lex->col++;
    return c;
}

static bool match(Lexer *lex, char expected) {
    if (*lex->current == expected) {
        advance(lex);
        return true;
    }
    return false;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

// --- Keyword lookup ---

typedef struct { const char *name; TokenKind kind; } Keyword;

static Keyword keywords[] = {
    {"if", TOK_IF}, {"else", TOK_ELSE}, {"while", TOK_WHILE},
    {"for", TOK_FOR}, {"in", TOK_IN}, {"match", TOK_MATCH},
    {"break", TOK_BREAK}, {"continue", TOK_CONTINUE}, {"done", TOK_DONE},
    {"shape", TOK_SHAPE}, {"extern", TOK_EXTERN}, {"use", TOK_USE},
    {"pub", TOK_PUB}, {"type", TOK_TYPE}, {"zone", TOK_ZONE},
    {"defer", TOK_DEFER}, {"unsafe", TOK_UNSAFE},
    {"and", TOK_AND}, {"or", TOK_OR}, {"not", TOK_NOT},
    {"go", TOK_GO}, {"run", TOK_RUN}, {"wait", TOK_WAIT},
    {"stream", TOK_STREAM}, {"yield", TOK_YIELD}, {"shared", TOK_SHARED},
    {"repeat", TOK_REPEAT}, {"as", TOK_AS}, {"then", TOK_THEN}, {"set", TOK_SET},
    {"true", TOK_TRUE}, {"false", TOK_FALSE}, {"none", TOK_NONE},
    // Type keywords
    {"int", TOK_INT}, {"float", TOK_FLOAT}, {"bool", TOK_BOOL},
    {"byte", TOK_BYTE}, {"str", TOK_STR},
    {"int8", TOK_INT8}, {"int16", TOK_INT16}, {"int32", TOK_INT32}, {"int64", TOK_INT64},
    {"uint8", TOK_UINT8}, {"uint16", TOK_UINT16}, {"uint32", TOK_UINT32}, {"uint64", TOK_UINT64},
    {"float32", TOK_FLOAT32}, {"float64", TOK_FLOAT64},
    {NULL, TOK_EOF}
};

static TokenKind lookup_keyword(const char *start, int length) {
    for (int i = 0; keywords[i].name != NULL; i++) {
        if ((int)strlen(keywords[i].name) == length &&
            memcmp(keywords[i].name, start, length) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

// --- Scanning functions ---

static void scan_identifier(Lexer *lex) {
    const char *start = lex->current - 1; // already advanced past first char
    int start_col = lex->col - 1;

    while (is_ident_char(peek(lex))) {
        advance(lex);
    }

    int length = (int)(lex->current - start);

    // Check for ! immediately after (no space) → mutable identifier
    if (peek(lex) == '!') {
        advance(lex);
        TokenKind kw = lookup_keyword(start, length);
        // Keywords can't be mutable
        if (kw != TOK_IDENT) {
            SrcLoc loc = {lex->filename, lex->line, start_col};
            mix_error(loc, "keyword '%.*s' cannot have '!' modifier", length, start);
            return;
        }
        Token tok = {0};
        tok.kind = TOK_IDENT_MUT;
        tok.start = start;
        tok.length = length + 1; // include the !
        tok.line = lex->line;
        tok.col = start_col;
        emit_token(lex, tok);
        return;
    }

    TokenKind kind = lookup_keyword(start, length);
    Token tok = {0};
    tok.kind = kind;
    tok.start = start;
    tok.length = length;
    tok.line = lex->line;
    tok.col = start_col;
    emit_token(lex, tok);
}

static void scan_number(Lexer *lex) {
    const char *start = lex->current - 1;
    int start_col = lex->col - 1;
    bool is_float = false;

    // Check for hex: 0x
    if (start[0] == '0' && (peek(lex) == 'x' || peek(lex) == 'X')) {
        advance(lex); // skip x
        while (isxdigit(peek(lex))) advance(lex);
        int length = (int)(lex->current - start);
        Token tok = {0};
        tok.kind = TOK_INT_LIT;
        tok.start = start;
        tok.length = length;
        tok.line = lex->line;
        tok.col = start_col;
        tok.value.int_val = strtoll(start, NULL, 16);
        emit_token(lex, tok);
        return;
    }

    // Check for binary: 0b
    if (start[0] == '0' && (peek(lex) == 'b' || peek(lex) == 'B')) {
        advance(lex); // skip b
        while (peek(lex) == '0' || peek(lex) == '1') advance(lex);
        int length = (int)(lex->current - start);
        Token tok = {0};
        tok.kind = TOK_INT_LIT;
        tok.start = start;
        tok.length = length;
        tok.line = lex->line;
        tok.col = start_col;
        tok.value.int_val = strtoll(start + 2, NULL, 2);
        emit_token(lex, tok);
        return;
    }

    while (isdigit(peek(lex))) advance(lex);

    // Check for decimal point (but not .. range operator)
    if (peek(lex) == '.' && peek_next(lex) != '.') {
        is_float = true;
        advance(lex); // skip .
        while (isdigit(peek(lex))) advance(lex);
    }

    // Check for exponent
    if (peek(lex) == 'e' || peek(lex) == 'E') {
        is_float = true;
        advance(lex);
        if (peek(lex) == '+' || peek(lex) == '-') advance(lex);
        while (isdigit(peek(lex))) advance(lex);
    }

    int length = (int)(lex->current - start);
    Token tok = {0};
    tok.kind = is_float ? TOK_FLOAT_LIT : TOK_INT_LIT;
    tok.start = start;
    tok.length = length;
    tok.line = lex->line;
    tok.col = start_col;

    if (is_float) {
        tok.value.float_val = strtod(start, NULL);
    } else {
        tok.value.int_val = strtoll(start, NULL, 10);
    }

    emit_token(lex, tok);
}

static void scan_string(Lexer *lex) {
    const char *start = lex->current; // after the opening "
    int start_col = lex->col - 1;
    int start_line = lex->line;

    while (peek(lex) != '"' && peek(lex) != '\0') {
        if (peek(lex) == '\\') {
            advance(lex); // skip backslash
            if (peek(lex) != '\0')
                advance(lex); // skip escaped char
        } else {
            if (peek(lex) == '\n') {
                lex->line++;
                lex->col = 0;
            }
            advance(lex);
        }
    }

    if (peek(lex) == '\0') {
        SrcLoc loc = {lex->filename, start_line, start_col};
        mix_error(loc, "unterminated string literal");
        return;
    }

    int str_length = (int)(lex->current - start);
    advance(lex); // skip closing "

    Token tok = {0};
    tok.kind = TOK_STRING_LIT;
    tok.start = start;
    tok.length = str_length;
    tok.line = start_line;
    tok.col = start_col;
    emit_token(lex, tok);
}

// --- Indentation processing ---

static void process_indentation(Lexer *lex) {
    // Count spaces at start of line
    int spaces = 0;
    while (peek(lex) == ' ') {
        advance(lex);
        spaces++;
    }

    // Check for tabs
    if (peek(lex) == '\t') {
        SrcLoc loc = {lex->filename, lex->line, lex->col};
        mix_error(loc, "tabs are not allowed for indentation, use 4 spaces");
        // Skip past the tab to continue
        while (peek(lex) == '\t') advance(lex);
        return;
    }

    // Skip blank lines and comment-only lines
    if (peek(lex) == '\n' || peek(lex) == '\0') return;
    if (peek(lex) == '/' && peek_next(lex) == '/') return;

    int current_indent = spaces;
    int stack_top = lex->indent_stack[lex->indent_top];

    if (current_indent > stack_top) {
        // Push new indent level
        if (lex->indent_top + 1 >= 128) {
            SrcLoc loc = {lex->filename, lex->line, 1};
            mix_error(loc, "indentation nested too deeply (max 127 levels)");
            return;
        }
        lex->indent_top++;
        lex->indent_stack[lex->indent_top] = current_indent;
        Token tok = {0};
        tok.kind = TOK_INDENT;
        tok.start = lex->current;
        tok.length = 0;
        tok.line = lex->line;
        tok.col = 1;
        emit_token(lex, tok);
    } else if (current_indent < stack_top) {
        // Pop indent levels and emit DEDENT for each
        while (lex->indent_top > 0 && lex->indent_stack[lex->indent_top] > current_indent) {
            lex->indent_top--;
            Token tok = {0};
            tok.kind = TOK_DEDENT;
            tok.start = lex->current;
            tok.length = 0;
            tok.line = lex->line;
            tok.col = 1;
            emit_token(lex, tok);
        }
        if (lex->indent_stack[lex->indent_top] != current_indent) {
            SrcLoc loc = {lex->filename, lex->line, 1};
            mix_error(loc, "inconsistent indentation");
        }
    }
    // If equal, no INDENT/DEDENT needed
}

// --- Main tokenization ---

void lexer_tokenize(Lexer *lex) {
    while (peek(lex) != '\0') {
        // At start of line, process indentation
        if (lex->at_line_start) {
            lex->at_line_start = false;
            if (lex->paren_depth == 0) {
                process_indentation(lex);
            } else {
                // Inside brackets, skip whitespace
                while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
            }
            if (peek(lex) == '\0') break;
        }

        char c = peek(lex);

        // Skip spaces (not at line start)
        if (c == ' ' || c == '\t') {
            advance(lex);
            continue;
        }

        // Newline
        if (c == '\n') {
            advance(lex);
            if (lex->paren_depth == 0) {
                // Only emit NEWLINE if last token wasn't already a NEWLINE
                if (lex->token_count > 0 && lex->tokens[lex->token_count - 1].kind != TOK_NEWLINE
                    && lex->tokens[lex->token_count - 1].kind != TOK_INDENT) {
                    Token tok = {0};
                    tok.kind = TOK_NEWLINE;
                    tok.start = lex->current - 1;
                    tok.length = 1;
                    tok.line = lex->line - 1;
                    tok.col = lex->col;
                    emit_token(lex, tok);
                }
            }
            lex->line++;
            lex->col = 1;
            lex->at_line_start = true;
            continue;
        }

        // Comment
        if (c == '/' && peek_next(lex) == '/') {
            while (peek(lex) != '\n' && peek(lex) != '\0') advance(lex);
            continue;
        }

        // Start of a real token
        advance(lex);

        // Identifiers and keywords
        if (is_ident_start(c)) {
            scan_identifier(lex);
            continue;
        }

        // Numbers
        if (isdigit(c)) {
            scan_number(lex);
            continue;
        }

        // Strings
        if (c == '"') {
            scan_string(lex);
            continue;
        }

        // Operators and delimiters
        switch (c) {
            case '(': {
                lex->paren_depth++;
                emit_token(lex, make_token(lex, TOK_LPAREN, lex->current - 1, 1));
                break;
            }
            case ')': {
                if (lex->paren_depth > 0) {
                    lex->paren_depth--;
                } else {
                    SrcLoc loc = {lex->filename, lex->line, lex->col - 1};
                    mix_error(loc, "unmatched ')'");
                }
                emit_token(lex, make_token(lex, TOK_RPAREN, lex->current - 1, 1));
                break;
            }
            case '[': {
                lex->paren_depth++;
                emit_token(lex, make_token(lex, TOK_LBRACKET, lex->current - 1, 1));
                break;
            }
            case ']': {
                if (lex->paren_depth > 0) {
                    lex->paren_depth--;
                } else {
                    SrcLoc loc = {lex->filename, lex->line, lex->col - 1};
                    mix_error(loc, "unmatched ']'");
                }
                emit_token(lex, make_token(lex, TOK_RBRACKET, lex->current - 1, 1));
                break;
            }
            case '{': {
                lex->paren_depth++;
                emit_token(lex, make_token(lex, TOK_LBRACE, lex->current - 1, 1));
                break;
            }
            case '}': {
                if (lex->paren_depth > 0) {
                    lex->paren_depth--;
                } else {
                    SrcLoc loc = {lex->filename, lex->line, lex->col - 1};
                    mix_error(loc, "unmatched '}'");
                }
                emit_token(lex, make_token(lex, TOK_RBRACE, lex->current - 1, 1));
                break;
            }
            case ':': emit_token(lex, make_token(lex, TOK_COLON, lex->current - 1, 1)); break;
            case ',': emit_token(lex, make_token(lex, TOK_COMMA, lex->current - 1, 1)); break;
            case '?': emit_token(lex, make_token(lex, TOK_QUESTION, lex->current - 1, 1)); break;
            case '~': emit_token(lex, make_token(lex, TOK_TILDE, lex->current - 1, 1)); break;
            case '@': emit_token(lex, make_token(lex, TOK_AT, lex->current - 1, 1)); break;
            case '|': emit_token(lex, make_token(lex, TOK_PIPE, lex->current - 1, 1)); break;
            case '&': emit_token(lex, make_token(lex, TOK_AMPERSAND, lex->current - 1, 1)); break;
            case '%': emit_token(lex, make_token(lex, TOK_PERCENT, lex->current - 1, 1)); break;

            case '+':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_PLUS_EQ, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_PLUS, lex->current - 1, 1));
                break;
            case '-':
                if (match(lex, '>')) emit_token(lex, make_token(lex, TOK_ARROW, lex->current - 2, 2));
                else if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_MINUS_EQ, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_MINUS, lex->current - 1, 1));
                break;
            case '*':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_STAR_EQ, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_STAR, lex->current - 1, 1));
                break;
            case '/':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_SLASH_EQ, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_SLASH, lex->current - 1, 1));
                break;
            case '=':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_EQEQ, lex->current - 2, 2));
                else if (match(lex, '>')) emit_token(lex, make_token(lex, TOK_FAT_ARROW, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_EQ, lex->current - 1, 1));
                break;
            case '!':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_NEQ, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_BANG, lex->current - 1, 1));
                break;
            case '<':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_LTE, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_LT, lex->current - 1, 1));
                break;
            case '>':
                if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_GTE, lex->current - 2, 2));
                else emit_token(lex, make_token(lex, TOK_GT, lex->current - 1, 1));
                break;
            case '.':
                if (match(lex, '.')) {
                    if (match(lex, '=')) emit_token(lex, make_token(lex, TOK_DOTDOT_EQ, lex->current - 3, 3));
                    else emit_token(lex, make_token(lex, TOK_DOTDOT, lex->current - 2, 2));
                } else {
                    emit_token(lex, make_token(lex, TOK_DOT, lex->current - 1, 1));
                }
                break;

            default: {
                SrcLoc loc = {lex->filename, lex->line, lex->col - 1};
                mix_error(loc, "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
                break;
            }
        }
    }

    // Emit remaining DEDENT tokens at EOF
    while (lex->indent_top > 0) {
        lex->indent_top--;
        Token tok = {0};
        tok.kind = TOK_DEDENT;
        tok.start = lex->current;
        tok.length = 0;
        tok.line = lex->line;
        tok.col = 1;
        emit_token(lex, tok);
    }

    // Emit EOF
    Token eof = {0};
    eof.kind = TOK_EOF;
    eof.start = lex->current;
    eof.length = 0;
    eof.line = lex->line;
    eof.col = lex->col;
    emit_token(lex, eof);
}

void lexer_print_tokens(Lexer *lex) {
    for (int i = 0; i < lex->token_count; i++) {
        Token *tok = &lex->tokens[i];
        if (tok->kind == TOK_INT_LIT) {
            printf("%3d:%-3d %-12s %.*s (%" PRId64 ")\n",
                   tok->line, tok->col, token_kind_name(tok->kind),
                   tok->length, tok->start, tok->value.int_val);
        } else if (tok->kind == TOK_FLOAT_LIT) {
            printf("%3d:%-3d %-12s %.*s (%g)\n",
                   tok->line, tok->col, token_kind_name(tok->kind),
                   tok->length, tok->start, tok->value.float_val);
        } else if (tok->kind == TOK_NEWLINE || tok->kind == TOK_INDENT || tok->kind == TOK_DEDENT || tok->kind == TOK_EOF) {
            printf("%3d:%-3d %-12s\n",
                   tok->line, tok->col, token_kind_name(tok->kind));
        } else {
            printf("%3d:%-3d %-12s %.*s\n",
                   tok->line, tok->col, token_kind_name(tok->kind),
                   tok->length, tok->start);
        }
    }
}
