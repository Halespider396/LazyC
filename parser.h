#ifndef LAZYC_PARSER_H
#define LAZYC_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer lx;
    Token current;
    Token previous;
    int had_error;
    int panic_mode;
} Parser;

void parser_init(Parser *p, const char *source, size_t length);

/* Parses a full translation unit. Check p->had_error afterward;
 * on error, whatever partial tree was built is still returned
 * (and still safe to pass to ast_free_program). */
Program parser_parse_program(Parser *p);

#endif
