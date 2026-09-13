#ifndef LAZYC_LEXER_H
#define LAZYC_LEXER_H

#include <stddef.h>

typedef enum {
    TOK_EOF = 0,

    /* literals & identifiers */
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_CHAR_LIT,

    /* keywords */
    TOK_WORK,       /* work   -> function decl */
    TOK_YIELD,      /* yield  -> return        */
    TOK_EXTERN,     /* extern -> declares an external (e.g. libc) function, no body */
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_STRUCT,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,

    /* operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_ASSIGN, TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_BITAND, TOK_BITOR, TOK_BITXOR, TOK_BITNOT,
    TOK_SHL, TOK_SHR,
    TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ,
    TOK_INC, TOK_DEC,
    TOK_AMP,        /* & used for address-of / pointer type */
    TOK_STAR_PTR,   /* reuse TOK_STAR for deref/pointer decl */

    /* punctuation */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COMMA, TOK_DOT, TOK_ARROW, TOK_COLON,

    TOK_UNKNOWN,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char *start;  /* pointer into source buffer, not null-terminated */
    size_t length;
    int line;
    int col;            /* 1-based column of the token's first character */
    /* pre-parsed literal values, valid only for the matching literal type */
    long int_val;
    double float_val;
    const char *error_msg; /* set only when type == TOK_ERROR, static string */
} Token;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int line;
    int col;
} Lexer;

void lexer_init(Lexer *lx, const char *source, size_t length);
Token lexer_next(Lexer *lx);
const char *token_type_name(TokenType t);

#endif
