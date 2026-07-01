#include "tree_sitter/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
    NEWLINE,
    INDENT,
    DEDENT,
};

typedef struct {
    uint16_t indents[256];
    uint16_t depth;
    bool pending_dedent;
} Scanner;

void *tree_sitter_mix_external_scanner_create() {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    scanner->indents[0] = 0;
    scanner->depth = 1;
    scanner->pending_dedent = false;
    return scanner;
}

void tree_sitter_mix_external_scanner_destroy(void *payload) {
    free(payload);
}

unsigned tree_sitter_mix_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    size_t size = 0;
    buffer[size++] = (char)scanner->pending_dedent;
    for (uint16_t i = 0; i < scanner->depth && size + 2 <= TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
        buffer[size++] = (char)(scanner->indents[i] & 0xFF);
        buffer[size++] = (char)((scanner->indents[i] >> 8) & 0xFF);
    }
    return size;
}

void tree_sitter_mix_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->depth = 1;
    scanner->indents[0] = 0;
    scanner->pending_dedent = false;
    size_t i = 0;
    if (length > 0) {
        scanner->pending_dedent = (bool)buffer[i++];
    }
    for (; i + 1 < length && scanner->depth < 256; i += 2) {
        scanner->indents[scanner->depth++] =
            (unsigned char)buffer[i] | ((unsigned char)buffer[i + 1] << 8);
    }
}

bool tree_sitter_mix_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;

    // If a prior NEWLINE flagged that a dedent is needed, finish it now.
    if (valid_symbols[DEDENT] && scanner->pending_dedent && scanner->depth > 1) {
        scanner->depth--;
        scanner->pending_dedent = false;
        lexer->result_symbol = DEDENT;
        return true;
    }

    // Handle EOF — emit DEDENT tokens to close remaining open blocks.
    if (lexer->lookahead == '\0' && lexer->eof(lexer)) {
        if (valid_symbols[DEDENT] && scanner->depth > 1) {
            scanner->depth--;
            scanner->pending_dedent = false;
            lexer->result_symbol = DEDENT;
            return true;
        }
        if (valid_symbols[NEWLINE] && scanner->depth > 1) {
            scanner->pending_dedent = true;
            lexer->result_symbol = NEWLINE;
            return true;
        }
        return false;
    }

    // Only act when we're at a newline character.
    if (lexer->lookahead != '\n') {
        return false;
    }

    // Consume the newline as part of the token we will emit.
    lexer->advance(lexer, false);  // \n becomes part of the token
    lexer->mark_end(lexer);        // token ends right after \n

    // Read past any preceding whitespace of the next line(s).
    uint16_t indent_length = 0;

    for (;;) {
        switch (lexer->lookahead) {
            case ' ':  indent_length++; lexer->advance(lexer, true); break;
            case '\t': indent_length += 8; lexer->advance(lexer, true); break;
            case '\r':
            case '\f': indent_length = 0; lexer->advance(lexer, true); break;
            case '\n':
                // Blank line — skip it.
                lexer->advance(lexer, true);
                indent_length = 0;
                break;
            case '/': {
                lexer->advance(lexer, true);
                if (lexer->lookahead == '/') {
                    while (lexer->lookahead && lexer->lookahead != '\n') {
                        lexer->advance(lexer, true);
                    }
                    indent_length = 0;
                    break;
                }
                goto decide;
            }
            case '\0':
                // EOF after scanning past whitespace/blank lines.
                goto decide;
            default:
                goto decide;
        }
    }

decide:
    {
        uint16_t current = scanner->indents[scanner->depth - 1];

        if (valid_symbols[INDENT] && indent_length > current) {
            if (scanner->depth < 256) {
                scanner->indents[scanner->depth++] = indent_length;
            }
            scanner->pending_dedent = false;
            lexer->result_symbol = INDENT;
            return true;
        }

        if (valid_symbols[DEDENT] && indent_length < current) {
            if (scanner->depth > 1) {
                scanner->depth--;
            }
            scanner->pending_dedent = false;
            lexer->result_symbol = DEDENT;
            return true;
        }

        if (valid_symbols[NEWLINE]) {
            if (indent_length < current) {
                scanner->pending_dedent = true;
            }
            lexer->result_symbol = NEWLINE;
            return true;
        }
    }

    return false;
}
