#ifndef TOKEN_H
#define TOKEN_H

typedef enum {
    // Structure
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,
    TOK_EOF,

    // Literals
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NONE,

    // Identifiers
    TOK_IDENT,
    TOK_IDENT_MUT,      // identifier ending with !

    // Keywords
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_MATCH,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_DONE,
    TOK_SHAPE,
    TOK_UNION,
    TOK_EXTERN,
    TOK_USE,
    TOK_PUB,
    TOK_TYPE,
    TOK_ZONE,
    TOK_DEFER,
    TOK_UNSAFE,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_GO,
    TOK_RUN,
    TOK_WAIT,
    TOK_STREAM,
    TOK_YIELD,
    TOK_SHARED,
    TOK_REPEAT,
    TOK_AS,
    TOK_THEN,
    TOK_SET,

    // Type keywords
    TOK_INT,
    TOK_FLOAT,
    TOK_BOOL,
    TOK_BYTE,
    TOK_STR,
    TOK_INT8,
    TOK_INT16,
    TOK_INT32,
    TOK_INT64,
    TOK_UINT8,
    TOK_UINT16,
    TOK_UINT32,
    TOK_UINT64,
    TOK_FLOAT32,
    TOK_FLOAT64,

    // Operators
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LTE,
    TOK_GTE,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,
    TOK_STAR_EQ,
    TOK_SLASH_EQ,
    TOK_ARROW,          // ->
    TOK_FAT_ARROW,      // =>
    TOK_AMPERSAND,       // &
    TOK_DOTDOT,          // ..
    TOK_DOTDOT_EQ,       // ..=
    TOK_PIPE,            // |
    TOK_QUESTION,        // ?
    TOK_TILDE,           // ~
    TOK_BANG,            // !

    // Delimiters
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COLON,
    TOK_COMMA,
    TOK_DOT,

    // Compile-time
    TOK_AT,

    TOK_COUNT            // number of token types
} TokenKind;

struct Token {
    TokenKind kind;
    const char *start;   // pointer into source buffer
    int length;
    int line;
    int col;
    union {
        int64_t int_val;
        double float_val;
    } value;
};

const char *token_kind_name(TokenKind kind);

#endif // TOKEN_H
