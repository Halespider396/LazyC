#ifndef LAZYC_AST_H
#define LAZYC_AST_H

#include <stddef.h>

/* ---- Types ---------------------------------------------------------- */
/* A LazyC type is written as a base name with zero or more leading '*'
 * (pointer depth) and an optional trailing '[]' (array-of marker).
 * e.g.  int          -> ptr_depth=0, is_array=0
 *       *int         -> ptr_depth=1
 *       **char       -> ptr_depth=2
 *       int[]        -> is_array=1
 */
typedef struct {
    char *name;       /* base type name, e.g. "int", "MyStruct" */
    int ptr_depth;    /* number of leading '*' */
    int is_array;     /* trailing '[]' present */
} Type;

/* ---- Expressions ------------------------------------------------------ */
typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_STRING_LIT,
    EXPR_CHAR_LIT,
    EXPR_BOOL_LIT,
    EXPR_NULL_LIT,
    EXPR_IDENT,
    EXPR_UNARY,       /* op, operand (prefix: - ! ~ & * ++ --) */
    EXPR_POSTFIX,     /* operand, op (postfix: ++ --) */
    EXPR_BINARY,      /* left, op, right */
    EXPR_ASSIGN,      /* target, op (=, +=, -=, *=, /=), value */
    EXPR_CALL,        /* callee, args[] */
    EXPR_INDEX,       /* array, index */
    EXPR_MEMBER,      /* object, field, arrow (.->) */
} ExprKind;

typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int line, col;

    union {
        long int_val;
        double float_val;
        struct { char *value; } str_lit;   /* raw text between quotes, unescaped as-is */
        char char_val;
        int bool_val;
        char *ident;

        struct {
            int op;             /* TokenType of the operator */
            Expr *operand;
        } unary;

        struct {
            int op;
            Expr *operand;
        } postfix;

        struct {
            int op;
            Expr *left;
            Expr *right;
        } binary;

        struct {
            int op;             /* TOK_ASSIGN, TOK_PLUS_EQ, ... */
            Expr *target;
            Expr *value;
        } assign;

        struct {
            Expr *callee;
            Expr **args;
            size_t arg_count;
        } call;

        struct {
            Expr *array;
            Expr *index;
        } index;

        struct {
            Expr *object;
            char *field;
            int arrow;          /* 1 if accessed via -> */
        } member;
    } as;
};

/* ---- Statements --------------------------------------------------------- */
typedef enum {
    STMT_EXPR,
    STMT_VAR_DECL,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_YIELD,
} StmtKind;

typedef struct Stmt Stmt;

struct Stmt {
    StmtKind kind;
    int line, col;

    union {
        struct { Expr *expr; } expr_stmt;

        struct {
            char *name;
            Type type;
            Expr *init;         /* NULL if uninitialized */
        } var_decl;

        struct {
            Stmt **stmts;
            size_t count;
        } block;

        struct {
            Expr *cond;
            Stmt *then_branch;
            Stmt *else_branch;  /* NULL, or another STMT_IF, or a STMT_BLOCK */
        } if_stmt;

        struct {
            Expr *cond;
            Stmt *body;
        } while_stmt;

        struct {
            Stmt *init;         /* NULL, var-decl or expr-stmt */
            Expr *cond;         /* NULL means "always true" */
            Expr *post;         /* NULL means no post-expr */
            Stmt *body;
        } for_stmt;

        struct { Expr *value; } yield_stmt; /* value NULL for bare `yield;` */
    } as;
};

/* ---- Top level ----------------------------------------------------------- */
typedef struct {
    char *name;
    Type type;
} Param;

typedef struct {
    char *name;
    Type return_type;      /* has_return_type == 0 for a void-like work */
    int has_return_type;
    Param *params;
    size_t param_count;
    Stmt *body;             /* STMT_BLOCK, or NULL when is_extern */
    int is_extern;          /* declared via `extern work name(...) type;` — no body,
                              * resolved externally (e.g. a real libc function) */
} FuncDecl;

typedef struct {
    char *name;
    Type type;
} FieldDecl;

typedef struct {
    char *name;
    FieldDecl *fields;
    size_t field_count;
} StructDecl;

typedef enum {
    DECL_FUNC,
    DECL_STRUCT,
    DECL_VAR,       /* global variable */
} DeclKind;

typedef struct {
    DeclKind kind;
    union {
        FuncDecl func;
        StructDecl struct_decl;
        Stmt *var;   /* STMT_VAR_DECL, reused for globals */
    } as;
} Decl;

typedef struct {
    Decl *decls;
    size_t count;
} Program;

/* ---- Constructors (heap-allocated; program tree owns everything) -------- */
Expr *ast_new_expr(ExprKind kind, int line, int col);
Stmt *ast_new_stmt(StmtKind kind, int line, int col);

/* ---- Debug printing ------------------------------------------------------ */
void ast_print_program(const Program *prog);

/* ---- Cleanup -------------------------------------------------------------- */
void ast_free_program(Program *prog);

#endif
