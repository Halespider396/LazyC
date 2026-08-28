#include "lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

static int at_end(Lexer *lx) { return lx->pos >= lx->len; }

static char peek(Lexer *lx) {
    return at_end(lx) ? '\0' : lx->src[lx->pos];
}

static char peek_next(Lexer *lx) {
    return (lx->pos + 1 >= lx->len) ? '\0' : lx->src[lx->pos + 1];
}

static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static int match(Lexer *lx, char expected) {
    if (at_end(lx) || lx->src[lx->pos] != expected) return 0;
    lx->pos++;
    return 1;
}

void lexer_init(Lexer *lx, const char *source, size_t length) {
    lx->src = source;
    lx->pos = 0;
    lx->len = length;
    lx->line = 1;
    lx->col = 1;
}

/* Returns 1 if an unterminated block comment was hit (lexer positioned at EOF,
 * unterminated_start left pointing at the opening slash-star). Returns 0 otherwise. */
static int skip_whitespace_and_comments(Lexer *lx, const char **unterminated_start, int *unterminated_line, int *unterminated_col) {
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c == '/' && peek_next(lx) == '/') {
            while (!at_end(lx) && peek(lx) != '\n') advance(lx);
        } else if (c == '/' && peek_next(lx) == '*') {
            const char *start = lx->src + lx->pos;
            int start_line = lx->line, start_col = lx->col;
            advance(lx); advance(lx);
            while (!at_end(lx) && !(peek(lx) == '*' && peek_next(lx) == '/')) advance(lx);
            if (!at_end(lx)) {
                advance(lx); advance(lx);
            } else {
                *unterminated_start = start;
                *unterminated_line = start_line;
                *unterminated_col = start_col;
                return 1;
            }
        } else {
            break;
        }
    }
    return 0;
}

typedef struct { const char *word; TokenType type; } Keyword;

static const Keyword keywords[] = {
    {"work",     TOK_WORK},
    {"yield",    TOK_YIELD},
    {"extern",   TOK_EXTERN},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"struct",   TOK_STRUCT},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"true",     TOK_TRUE},
    {"false",    TOK_FALSE},
    {"null",     TOK_NULL},
};
#define NUM_KEYWORDS (sizeof(keywords) / sizeof(keywords[0]))

static TokenType ident_or_keyword(const char *start, size_t length) {
    for (size_t i = 0; i < NUM_KEYWORDS; i++) {
        size_t klen = strlen(keywords[i].word);
        if (klen == length && strncmp(start, keywords[i].word, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static Token make_token_at(TokenType type, const char *start, size_t length, int line, int col) {
    Token t;
    t.type = type;
    t.start = start;
    t.length = length;
    t.line = line;
    t.col = col;
    t.int_val = 0;
    t.float_val = 0.0;
    t.error_msg = NULL;
    return t;
}

/* Convenience: token ending at the lexer's *current* position, using its current line/col.
 * NOTE: only correct for single-char tokens scanned via the start/advance pattern below,
 * where col has already moved past the token; callers needing the token's own start
 * position pass it explicitly via make_token_at. */
/* Used for tokens we know are single-line and single-char-per-advance (operators,
 * punctuation, and the fixed-width literal/identifier scanners below all call
 * make_token_at directly with a captured start line/col instead of this). */
static Token make_token(Lexer *lx, TokenType type, const char *start, size_t length) {
    int start_col = lx->col - (int)length;
    if (start_col < 1) start_col = 1;
    return make_token_at(type, start, length, lx->line, start_col);
}

static Token make_error(const char *msg, const char *start, size_t length, int line, int col) {
    Token t = make_token_at(TOK_ERROR, start, length, line, col);
    t.error_msg = msg;
    return t;
}

static Token scan_number(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    int start_line = lx->line, start_col = lx->col;
    int is_float = 0;
    while (isdigit((unsigned char)peek(lx))) advance(lx);
    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        is_float = 1;
        advance(lx); /* consume '.' */
        while (isdigit((unsigned char)peek(lx))) advance(lx);
    }
    size_t length = (size_t)((lx->src + lx->pos) - start);
    Token t = make_token_at(is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, start, length, start_line, start_col);
    if (is_float) {
        t.float_val = strtod(start, NULL);
    } else {
        t.int_val = strtol(start, NULL, 10);
    }
    return t;
}

static Token scan_string(Lexer *lx) {
    const char *start = lx->src + lx->pos; /* points at opening quote */
    int start_line = lx->line, start_col = lx->col;
    advance(lx); /* consume opening " */
    while (!at_end(lx) && peek(lx) != '"') {
        if (peek(lx) == '\\' && !at_end(lx)) advance(lx); /* skip escaped char */
        advance(lx);
    }
    if (at_end(lx)) {
        size_t length = (size_t)((lx->src + lx->pos) - start);
        return make_error("unterminated string literal", start, length, start_line, start_col);
    }
    advance(lx); /* consume closing " */
    size_t length = (size_t)((lx->src + lx->pos) - start);
    return make_token_at(TOK_STRING_LIT, start, length, start_line, start_col);
}

static Token scan_char(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    int start_line = lx->line, start_col = lx->col;
    advance(lx); /* consume opening ' */

    if (peek(lx) == '\'') {
        /* empty char literal '' */
        advance(lx);
        size_t length = (size_t)((lx->src + lx->pos) - start);
        return make_error("empty char literal", start, length, start_line, start_col);
    }
    if (at_end(lx) || peek(lx) == '\n') {
        size_t length = (size_t)((lx->src + lx->pos) - start);
        return make_error("unterminated char literal", start, length, start_line, start_col);
    }

    if (peek(lx) == '\\' && !at_end(lx)) advance(lx); /* escape */
    if (!at_end(lx)) advance(lx); /* the char itself */

    if (peek(lx) != '\'') {
        size_t length = (size_t)((lx->src + lx->pos) - start);
        return make_error("unterminated char literal", start, length, start_line, start_col);
    }
    advance(lx); /* closing quote */
    size_t length = (size_t)((lx->src + lx->pos) - start);
    return make_token_at(TOK_CHAR_LIT, start, length, start_line, start_col);
}

static Token scan_identifier(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    int start_line = lx->line, start_col = lx->col;
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    size_t length = (size_t)((lx->src + lx->pos) - start);
    TokenType type = ident_or_keyword(start, length);
    return make_token_at(type, start, length, start_line, start_col);
}

Token lexer_next(Lexer *lx) {
    const char *unterm_start;
    int unterm_line, unterm_col;
    if (skip_whitespace_and_comments(lx, &unterm_start, &unterm_line, &unterm_col)) {
        size_t length = (size_t)((lx->src + lx->pos) - unterm_start);
        return make_error("unterminated block comment", unterm_start, length, unterm_line, unterm_col);
    }

    if (at_end(lx)) return make_token_at(TOK_EOF, lx->src + lx->pos, 0, lx->line, lx->col);

    char c = peek(lx);

    if (isalpha((unsigned char)c) || c == '_') return scan_identifier(lx);
    if (isdigit((unsigned char)c)) return scan_number(lx);
    if (c == '"') return scan_string(lx);
    if (c == '\'') return scan_char(lx);

    const char *start = lx->src + lx->pos;
    advance(lx);

    switch (c) {
        case '(': return make_token(lx, TOK_LPAREN, start, 1);
        case ')': return make_token(lx, TOK_RPAREN, start, 1);
        case '{': return make_token(lx, TOK_LBRACE, start, 1);
        case '}': return make_token(lx, TOK_RBRACE, start, 1);
        case '[': return make_token(lx, TOK_LBRACKET, start, 1);
        case ']': return make_token(lx, TOK_RBRACKET, start, 1);
        case ';': return make_token(lx, TOK_SEMI, start, 1);
        case ',': return make_token(lx, TOK_COMMA, start, 1);
        case ':': return make_token(lx, TOK_COLON, start, 1);
        case '.': return make_token(lx, TOK_DOT, start, 1);

        case '+':
            if (match(lx, '+')) return make_token(lx, TOK_INC, start, 2);
            if (match(lx, '=')) return make_token(lx, TOK_PLUS_EQ, start, 2);
            return make_token(lx, TOK_PLUS, start, 1);
        case '-':
            if (match(lx, '-')) return make_token(lx, TOK_DEC, start, 2);
            if (match(lx, '=')) return make_token(lx, TOK_MINUS_EQ, start, 2);
            if (match(lx, '>')) return make_token(lx, TOK_ARROW, start, 2);
            return make_token(lx, TOK_MINUS, start, 1);
        case '*':
            if (match(lx, '=')) return make_token(lx, TOK_STAR_EQ, start, 2);
            return make_token(lx, TOK_STAR, start, 1);
        case '/':
            if (match(lx, '=')) return make_token(lx, TOK_SLASH_EQ, start, 2);
            return make_token(lx, TOK_SLASH, start, 1);
        case '%':
            return make_token(lx, TOK_PERCENT, start, 1);

        case '=':
            if (match(lx, '=')) return make_token(lx, TOK_EQ, start, 2);
            return make_token(lx, TOK_ASSIGN, start, 1);
        case '!':
            if (match(lx, '=')) return make_token(lx, TOK_NEQ, start, 2);
            return make_token(lx, TOK_NOT, start, 1);
        case '<':
            if (match(lx, '=')) return make_token(lx, TOK_LE, start, 2);
            if (match(lx, '<')) return make_token(lx, TOK_SHL, start, 2);
            return make_token(lx, TOK_LT, start, 1);
        case '>':
            if (match(lx, '=')) return make_token(lx, TOK_GE, start, 2);
            if (match(lx, '>')) return make_token(lx, TOK_SHR, start, 2);
            return make_token(lx, TOK_GT, start, 1);

        case '&':
            if (match(lx, '&')) return make_token(lx, TOK_AND, start, 2);
            return make_token(lx, TOK_AMP, start, 1);
        case '|':
            if (match(lx, '|')) return make_token(lx, TOK_OR, start, 2);
            return make_token(lx, TOK_BITOR, start, 1);
        case '^':
            return make_token(lx, TOK_BITXOR, start, 1);
        case '~':
            return make_token(lx, TOK_BITNOT, start, 1);

        default:
            return make_error("unexpected character", start, 1, lx->line, lx->col - 1);
    }
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_EOF: return "EOF";
        case TOK_IDENT: return "IDENT";
        case TOK_INT_LIT: return "INT_LIT";
        case TOK_FLOAT_LIT: return "FLOAT_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        case TOK_CHAR_LIT: return "CHAR_LIT";
        case TOK_WORK: return "WORK";
        case TOK_EXTERN: return "EXTERN";
        case TOK_YIELD: return "YIELD";
        case TOK_IF: return "IF";
        case TOK_ELSE: return "ELSE";
        case TOK_WHILE: return "WHILE";
        case TOK_FOR: return "FOR";
        case TOK_STRUCT: return "STRUCT";
        case TOK_BREAK: return "BREAK";
        case TOK_CONTINUE: return "CONTINUE";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NULL: return "NULL";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_ASSIGN: return "ASSIGN";
        case TOK_EQ: return "EQ";
        case TOK_NEQ: return "NEQ";
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_LE: return "LE";
        case TOK_GE: return "GE";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_NOT: return "NOT";
        case TOK_BITAND: return "BITAND";
        case TOK_BITOR: return "BITOR";
        case TOK_BITXOR: return "BITXOR";
        case TOK_BITNOT: return "BITNOT";
        case TOK_SHL: return "SHL";
        case TOK_SHR: return "SHR";
        case TOK_PLUS_EQ: return "PLUS_EQ";
        case TOK_MINUS_EQ: return "MINUS_EQ";
        case TOK_STAR_EQ: return "STAR_EQ";
        case TOK_SLASH_EQ: return "SLASH_EQ";
        case TOK_INC: return "INC";
        case TOK_DEC: return "DEC";
        case TOK_AMP: return "AMP";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_SEMI: return "SEMI";
        case TOK_COMMA: return "COMMA";
        case TOK_DOT: return "DOT";
        case TOK_ARROW: return "ARROW";
        case TOK_COLON: return "COLON";
        case TOK_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
