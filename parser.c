#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- dynamic array helpers (tiny, local to this file) ------------------ */

#define DA_INIT_CAP 4

#define da_push(arr, count, cap, item) \
    do { \
        if ((count) >= (cap)) { \
            (cap) = (cap) == 0 ? DA_INIT_CAP : (cap) * 2; \
            (arr) = realloc((arr), (cap) * sizeof(*(arr))); \
        } \
        (arr)[(count)++] = (item); \
    } while (0)

/* ---- low-level token plumbing ------------------------------------------ */

static void report(Parser *p, Token *t, const char *msg) {
    if (p->panic_mode) return; /* suppress cascades until we resync */
    p->panic_mode = 1;
    p->had_error = 1;
    fprintf(stderr, "%d:%d: parse error at '%.*s': %s\n",
            t->line, t->col, (int)t->length, t->start, msg);
}

/* Advances p->current, silently reporting and skipping any lexer-level
 * TOK_ERROR tokens (unterminated strings/comments/etc.) along the way. */
static void advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        Token t = lexer_next(&p->lx);
        if (t.type == TOK_ERROR) {
            report(p, &t, t.error_msg ? t.error_msg : "lexer error");
            continue;
        }
        p->current = t;
        break;
    }
}

static int check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static int matchp(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance(p);
    return 1;
}

static void expect(Parser *p, TokenType type, const char *msg) {
    if (check(p, type)) { advance(p); return; }
    report(p, &p->current, msg);
}

static char *tok_dup(Token *t) {
    char *s = malloc(t->length + 1);
    memcpy(s, t->start, t->length);
    s[t->length] = '\0';
    return s;
}

/* Peeks one token past p->current without consuming it. Cheap because
 * Lexer is a small value type — copy it, pull one token, throw the copy
 * away. Lexer-level errors during lookahead are swallowed (real parsing
 * will hit and report them properly when we actually advance onto them). */
static Token peek_second(Parser *p) {
    Lexer snapshot = p->lx;
    Token t;
    do {
        t = lexer_next(&snapshot);
    } while (t.type == TOK_ERROR);
    return t;
}

/* Skips tokens until we're at a plausible statement/declaration boundary,
 * so one bad token doesn't cascade into hundreds of spurious errors. */
static void synchronize(Parser *p) {
    p->panic_mode = 0;
    while (!check(p, TOK_EOF)) {
        if (p->previous.type == TOK_SEMI) return;
        switch (p->current.type) {
            case TOK_WORK:
            case TOK_STRUCT:
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_YIELD:
            case TOK_BREAK:
            case TOK_CONTINUE:
            case TOK_RBRACE:
                return;
            default:
                break;
        }
        advance(p);
    }
}

void parser_init(Parser *p, const char *source, size_t length) {
    lexer_init(&p->lx, source, length);
    p->had_error = 0;
    p->panic_mode = 0;
    /* prime p->current; p->previous starts uninitialized until first advance */
    p->current.type = TOK_EOF;
    advance(p);
}

/* ---- types --------------------------------------------------------------- */

static Type parse_type(Parser *p) {
    Type t;
    t.name = NULL;
    t.ptr_depth = 0;
    t.is_array = 0;

    while (matchp(p, TOK_STAR)) t.ptr_depth++;

    if (check(p, TOK_IDENT)) {
        t.name = tok_dup(&p->current);
        advance(p);
    } else {
        report(p, &p->current, "expected a type name");
        t.name = strdup("<error>");
    }

    if (matchp(p, TOK_LBRACKET)) {
        expect(p, TOK_RBRACKET, "expected ']' after '[' in array type");
        t.is_array = 1;
    }
    return t;
}

/* True if `current name` looks like the start of a `name type` declaration
 * (i.e. current is IDENT and the token after it starts a type: another
 * IDENT, or a leading '*'). Used to disambiguate var-decls from
 * expression-statements that also start with an identifier. */
static int looks_like_decl(Parser *p) {
    if (!check(p, TOK_IDENT)) return 0;
    Token nxt = peek_second(p);
    return nxt.type == TOK_IDENT || nxt.type == TOK_STAR;
}

/* ---- expressions ---------------------------------------------------------- */

static Expr *parse_expr(Parser *p);
static Expr *parse_assignment(Parser *p);

static Expr **parse_arg_list(Parser *p, size_t *out_count) {
    Expr **args = NULL;
    size_t count = 0, cap = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            Expr *arg = parse_assignment(p);
            da_push(args, count, cap, arg);
        } while (matchp(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "expected ')' after argument list");
    *out_count = count;
    return args;
}

static Expr *parse_primary(Parser *p) {
    Token t = p->current;

    if (matchp(p, TOK_INT_LIT)) {
        Expr *e = ast_new_expr(EXPR_INT_LIT, t.line, t.col);
        e->as.int_val = t.int_val;
        return e;
    }
    if (matchp(p, TOK_FLOAT_LIT)) {
        Expr *e = ast_new_expr(EXPR_FLOAT_LIT, t.line, t.col);
        e->as.float_val = t.float_val;
        return e;
    }
    if (matchp(p, TOK_STRING_LIT)) {
        Expr *e = ast_new_expr(EXPR_STRING_LIT, t.line, t.col);
        e->as.str_lit.value = tok_dup(&t);
        return e;
    }
    if (matchp(p, TOK_CHAR_LIT)) {
        Expr *e = ast_new_expr(EXPR_CHAR_LIT, t.line, t.col);
        /* raw text includes quotes; take whatever's between them verbatim
         * (escape decoding is a lexer/semantic concern, not this parser's) */
        e->as.char_val = t.length >= 3 ? t.start[1] : '\0';
        return e;
    }
    if (matchp(p, TOK_TRUE)) {
        Expr *e = ast_new_expr(EXPR_BOOL_LIT, t.line, t.col);
        e->as.bool_val = 1;
        return e;
    }
    if (matchp(p, TOK_FALSE)) {
        Expr *e = ast_new_expr(EXPR_BOOL_LIT, t.line, t.col);
        e->as.bool_val = 0;
        return e;
    }
    if (matchp(p, TOK_NULL)) {
        return ast_new_expr(EXPR_NULL_LIT, t.line, t.col);
    }
    if (matchp(p, TOK_IDENT)) {
        Expr *e = ast_new_expr(EXPR_IDENT, t.line, t.col);
        e->as.ident = tok_dup(&t);
        return e;
    }
    if (matchp(p, TOK_LPAREN)) {
        Expr *inner = parse_expr(p);
        expect(p, TOK_RPAREN, "expected ')' after expression");
        return inner;
    }

    report(p, &t, "expected an expression");
    advance(p); /* don't get stuck */
    return ast_new_expr(EXPR_NULL_LIT, t.line, t.col);
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        Token t = p->current;
        if (matchp(p, TOK_LPAREN)) {
            Expr *call = ast_new_expr(EXPR_CALL, t.line, t.col);
            call->as.call.callee = e;
            call->as.call.args = parse_arg_list(p, &call->as.call.arg_count);
            e = call;
        } else if (matchp(p, TOK_LBRACKET)) {
            Expr *idx = ast_new_expr(EXPR_INDEX, t.line, t.col);
            idx->as.index.array = e;
            idx->as.index.index = parse_expr(p);
            expect(p, TOK_RBRACKET, "expected ']' after index expression");
            e = idx;
        } else if (check(p, TOK_DOT) || check(p, TOK_ARROW)) {
            int arrow = check(p, TOK_ARROW);
            advance(p);
            if (!check(p, TOK_IDENT)) {
                report(p, &p->current, "expected field name after '.'/'->'");
                break;
            }
            Expr *m = ast_new_expr(EXPR_MEMBER, t.line, t.col);
            m->as.member.object = e;
            m->as.member.field = tok_dup(&p->current);
            m->as.member.arrow = arrow;
            advance(p);
            e = m;
        } else if (check(p, TOK_INC) || check(p, TOK_DEC)) {
            int op = p->current.type;
            Expr *post = ast_new_expr(EXPR_POSTFIX, t.line, t.col);
            post->as.postfix.op = op;
            post->as.postfix.operand = e;
            advance(p);
            e = post;
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    Token t = p->current;
    if (check(p, TOK_NOT) || check(p, TOK_MINUS) || check(p, TOK_BITNOT) ||
        check(p, TOK_AMP) || check(p, TOK_STAR) ||
        check(p, TOK_INC) || check(p, TOK_DEC)) {
        int op = t.type;
        advance(p);
        Expr *operand = parse_unary(p);
        Expr *e = ast_new_expr(EXPR_UNARY, t.line, t.col);
        e->as.unary.op = op;
        e->as.unary.operand = operand;
        return e;
    }
    return parse_postfix(p);
}

/* Generic left-associative binary level: parses `next` on both sides and
 * folds while the current token is one of `ops` (zero-terminated by
 * TOK_EOF, which never appears mid-stream so it's a safe sentinel). */
static Expr *parse_binary_level(Parser *p, Expr *(*next)(Parser *), const TokenType *ops) {
    Expr *left = next(p);
    for (;;) {
        int matched = 0;
        for (const TokenType *op = ops; *op != TOK_EOF; op++) {
            if (check(p, *op)) {
                Token t = p->current;
                advance(p);
                Expr *right = next(p);
                Expr *bin = ast_new_expr(EXPR_BINARY, t.line, t.col);
                bin->as.binary.op = t.type;
                bin->as.binary.left = left;
                bin->as.binary.right = right;
                left = bin;
                matched = 1;
                break;
            }
        }
        if (!matched) break;
    }
    return left;
}

static Expr *parse_multiplicative(Parser *p) {
    static const TokenType ops[] = { TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_EOF };
    return parse_binary_level(p, parse_unary, ops);
}
static Expr *parse_additive(Parser *p) {
    static const TokenType ops[] = { TOK_PLUS, TOK_MINUS, TOK_EOF };
    return parse_binary_level(p, parse_multiplicative, ops);
}
static Expr *parse_shift(Parser *p) {
    static const TokenType ops[] = { TOK_SHL, TOK_SHR, TOK_EOF };
    return parse_binary_level(p, parse_additive, ops);
}
static Expr *parse_bitand(Parser *p) {
    static const TokenType ops[] = { TOK_AMP, TOK_EOF };
    return parse_binary_level(p, parse_shift, ops);
}
static Expr *parse_bitxor(Parser *p) {
    static const TokenType ops[] = { TOK_BITXOR, TOK_EOF };
    return parse_binary_level(p, parse_bitand, ops);
}
static Expr *parse_bitor(Parser *p) {
    static const TokenType ops[] = { TOK_BITOR, TOK_EOF };
    return parse_binary_level(p, parse_bitxor, ops);
}
static Expr *parse_relational(Parser *p) {
    static const TokenType ops[] = { TOK_LT, TOK_GT, TOK_LE, TOK_GE, TOK_EOF };
    return parse_binary_level(p, parse_bitor, ops);
}
static Expr *parse_equality(Parser *p) {
    static const TokenType ops[] = { TOK_EQ, TOK_NEQ, TOK_EOF };
    return parse_binary_level(p, parse_relational, ops);
}
static Expr *parse_logic_and(Parser *p) {
    static const TokenType ops[] = { TOK_AND, TOK_EOF };
    return parse_binary_level(p, parse_equality, ops);
}
static Expr *parse_logic_or(Parser *p) {
    static const TokenType ops[] = { TOK_OR, TOK_EOF };
    return parse_binary_level(p, parse_logic_and, ops);
}

static Expr *parse_assignment(Parser *p) {
    Expr *target = parse_logic_or(p);
    if (check(p, TOK_ASSIGN) || check(p, TOK_PLUS_EQ) || check(p, TOK_MINUS_EQ) ||
        check(p, TOK_STAR_EQ) || check(p, TOK_SLASH_EQ)) {
        Token t = p->current;
        advance(p);
        Expr *value = parse_assignment(p); /* right-associative */
        Expr *e = ast_new_expr(EXPR_ASSIGN, t.line, t.col);
        e->as.assign.op = t.type;
        e->as.assign.target = target;
        e->as.assign.value = value;
        return e;
    }
    return target;
}

static Expr *parse_expr(Parser *p) { return parse_assignment(p); }

/* ---- statements ------------------------------------------------------------ */

static Stmt *parse_stmt(Parser *p);

static Stmt *parse_var_decl(Parser *p, int consume_semi) {
    Token t = p->current;
    Stmt *s = ast_new_stmt(STMT_VAR_DECL, t.line, t.col);
    s->as.var_decl.name = tok_dup(&t);
    advance(p); /* the name */
    s->as.var_decl.type = parse_type(p);
    if (matchp(p, TOK_ASSIGN)) {
        s->as.var_decl.init = parse_expr(p);
    } else {
        s->as.var_decl.init = NULL;
    }
    if (consume_semi) expect(p, TOK_SEMI, "expected ';' after variable declaration");
    return s;
}

static Stmt *parse_block(Parser *p) {
    Token t = p->current;
    expect(p, TOK_LBRACE, "expected '{'");
    Stmt *s = ast_new_stmt(STMT_BLOCK, t.line, t.col);
    Stmt **stmts = NULL;
    size_t count = 0, cap = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Stmt *inner = parse_stmt(p);
        da_push(stmts, count, cap, inner);
        if (p->panic_mode) synchronize(p);
    }
    expect(p, TOK_RBRACE, "expected '}' to close block");
    s->as.block.stmts = stmts;
    s->as.block.count = count;
    return s;
}

static Stmt *parse_if(Parser *p) {
    Token t = p->current;
    advance(p); /* 'if' */
    expect(p, TOK_LPAREN, "expected '(' after 'if'");
    Expr *cond = parse_expr(p);
    expect(p, TOK_RPAREN, "expected ')' after if condition");
    Stmt *then_branch = parse_block(p);
    Stmt *else_branch = NULL;
    if (matchp(p, TOK_ELSE)) {
        else_branch = check(p, TOK_IF) ? parse_if(p) : parse_block(p);
    }
    Stmt *s = ast_new_stmt(STMT_IF, t.line, t.col);
    s->as.if_stmt.cond = cond;
    s->as.if_stmt.then_branch = then_branch;
    s->as.if_stmt.else_branch = else_branch;
    return s;
}

static Stmt *parse_while(Parser *p) {
    Token t = p->current;
    advance(p); /* 'while' */
    expect(p, TOK_LPAREN, "expected '(' after 'while'");
    Expr *cond = parse_expr(p);
    expect(p, TOK_RPAREN, "expected ')' after while condition");
    Stmt *body = parse_block(p);
    Stmt *s = ast_new_stmt(STMT_WHILE, t.line, t.col);
    s->as.while_stmt.cond = cond;
    s->as.while_stmt.body = body;
    return s;
}

/* for (init?; cond?; post?) { body } — C-style three-clause for. */
static Stmt *parse_for(Parser *p) {
    Token t = p->current;
    advance(p); /* 'for' */
    expect(p, TOK_LPAREN, "expected '(' after 'for'");

    Stmt *init = NULL;
    if (!check(p, TOK_SEMI)) {
        if (looks_like_decl(p)) {
            init = parse_var_decl(p, 0);
        } else {
            Stmt *es = ast_new_stmt(STMT_EXPR, p->current.line, p->current.col);
            es->as.expr_stmt.expr = parse_expr(p);
            init = es;
        }
    }
    expect(p, TOK_SEMI, "expected ';' after for-loop initializer");

    Expr *cond = check(p, TOK_SEMI) ? NULL : parse_expr(p);
    expect(p, TOK_SEMI, "expected ';' after for-loop condition");

    Expr *post = check(p, TOK_RPAREN) ? NULL : parse_expr(p);
    expect(p, TOK_RPAREN, "expected ')' after for-loop clauses");

    Stmt *body = parse_block(p);

    Stmt *s = ast_new_stmt(STMT_FOR, t.line, t.col);
    s->as.for_stmt.init = init;
    s->as.for_stmt.cond = cond;
    s->as.for_stmt.post = post;
    s->as.for_stmt.body = body;
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    Token t = p->current;

    if (check(p, TOK_LBRACE)) return parse_block(p);
    if (check(p, TOK_IF)) return parse_if(p);
    if (check(p, TOK_WHILE)) return parse_while(p);
    if (check(p, TOK_FOR)) return parse_for(p);

    if (matchp(p, TOK_BREAK)) {
        expect(p, TOK_SEMI, "expected ';' after 'break'");
        return ast_new_stmt(STMT_BREAK, t.line, t.col);
    }
    if (matchp(p, TOK_CONTINUE)) {
        expect(p, TOK_SEMI, "expected ';' after 'continue'");
        return ast_new_stmt(STMT_CONTINUE, t.line, t.col);
    }
    if (matchp(p, TOK_YIELD)) {
        Stmt *s = ast_new_stmt(STMT_YIELD, t.line, t.col);
        s->as.yield_stmt.value = check(p, TOK_SEMI) ? NULL : parse_expr(p);
        expect(p, TOK_SEMI, "expected ';' after 'yield'");
        return s;
    }

    if (looks_like_decl(p)) return parse_var_decl(p, 1);

    Stmt *s = ast_new_stmt(STMT_EXPR, t.line, t.col);
    s->as.expr_stmt.expr = parse_expr(p);
    expect(p, TOK_SEMI, "expected ';' after expression");
    return s;
}

/* ---- top-level declarations ------------------------------------------------ */

static FuncDecl parse_func_decl(Parser *p, int is_extern) {
    FuncDecl f = {0};
    f.is_extern = is_extern;
    advance(p); /* 'work' */

    if (check(p, TOK_IDENT)) {
        f.name = tok_dup(&p->current);
        advance(p);
    } else {
        report(p, &p->current, "expected function name after 'work'");
        f.name = strdup("<error>");
    }

    expect(p, TOK_LPAREN, "expected '(' after function name");

    Param *params = NULL;
    size_t count = 0, cap = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            Param prm = {0};
            if (check(p, TOK_IDENT)) {
                prm.name = tok_dup(&p->current);
                advance(p);
            } else {
                report(p, &p->current, "expected parameter name");
                prm.name = strdup("<error>");
            }
            prm.type = parse_type(p);
            da_push(params, count, cap, prm);
        } while (matchp(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "expected ')' after parameter list");
    f.params = params;
    f.param_count = count;

    if (check(p, TOK_LBRACE) || (is_extern && check(p, TOK_SEMI))) {
        f.has_return_type = 0;
    } else {
        f.return_type = parse_type(p);
        f.has_return_type = 1;
    }

    if (is_extern) {
        expect(p, TOK_SEMI, "expected ';' after extern function declaration (extern functions have no body)");
        f.body = NULL;
    } else {
        f.body = parse_block(p);
    }
    return f;
}

static StructDecl parse_struct_decl(Parser *p) {
    StructDecl sd = {0};
    advance(p); /* 'struct' */

    if (check(p, TOK_IDENT)) {
        sd.name = tok_dup(&p->current);
        advance(p);
    } else {
        report(p, &p->current, "expected struct name after 'struct'");
        sd.name = strdup("<error>");
    }

    expect(p, TOK_LBRACE, "expected '{' to begin struct body");

    FieldDecl *fields = NULL;
    size_t count = 0, cap = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        FieldDecl fd = {0};
        if (check(p, TOK_IDENT)) {
            fd.name = tok_dup(&p->current);
            advance(p);
        } else {
            report(p, &p->current, "expected field name in struct body");
            fd.name = strdup("<error>");
        }
        fd.type = parse_type(p);
        expect(p, TOK_SEMI, "expected ';' after struct field");
        da_push(fields, count, cap, fd);
        if (p->panic_mode) synchronize(p);
    }
    expect(p, TOK_RBRACE, "expected '}' to close struct body");

    sd.fields = fields;
    sd.field_count = count;
    return sd;
}

Program parser_parse_program(Parser *p) {
    Program prog = {0};
    Decl *decls = NULL;
    size_t count = 0, cap = 0;

    while (!check(p, TOK_EOF)) {
        Decl d = {0};
        if (check(p, TOK_WORK)) {
            d.kind = DECL_FUNC;
            d.as.func = parse_func_decl(p, 0);
        } else if (check(p, TOK_EXTERN)) {
            advance(p); /* 'extern' */
            if (!check(p, TOK_WORK)) {
                report(p, &p->current, "expected 'work' after 'extern'");
                if (p->panic_mode) synchronize(p);
                continue;
            }
            d.kind = DECL_FUNC;
            d.as.func = parse_func_decl(p, 1);
        } else if (check(p, TOK_STRUCT)) {
            d.kind = DECL_STRUCT;
            d.as.struct_decl = parse_struct_decl(p);
        } else if (check(p, TOK_IDENT)) {
            d.kind = DECL_VAR;
            d.as.var = parse_var_decl(p, 1);
        } else {
            report(p, &p->current, "expected 'work', 'struct', or a variable declaration");
            advance(p);
            if (p->panic_mode) synchronize(p);
            continue;
        }
        da_push(decls, count, cap, d);
        if (p->panic_mode) synchronize(p);
    }

    prog.decls = decls;
    prog.count = count;
    return prog;
}
