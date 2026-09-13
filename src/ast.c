#include "ast.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Expr *ast_new_expr(ExprKind kind, int line, int col) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

Stmt *ast_new_stmt(StmtKind kind, int line, int col) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

/* ---------------------------------------------------------------------- */
/* Debug printer: emits a simple indented s-expression style tree.         */
/* ---------------------------------------------------------------------- */

static void indent(int n) { for (int i = 0; i < n; i++) fputs("  ", stdout); }

static void print_type(const Type *t) {
    for (int i = 0; i < t->ptr_depth; i++) putchar('*');
    fputs(t->name ? t->name : "<?>", stdout);
    if (t->is_array) fputs("[]", stdout);
}

static void print_expr(const Expr *e, int depth) {
    if (!e) { indent(depth); printf("<null-expr>\n"); return; }
    indent(depth);
    switch (e->kind) {
        case EXPR_INT_LIT:    printf("Int(%ld)\n", e->as.int_val); break;
        case EXPR_FLOAT_LIT:  printf("Float(%f)\n", e->as.float_val); break;
        case EXPR_STRING_LIT: printf("String(%s)\n", e->as.str_lit.value); break;
        case EXPR_CHAR_LIT:   printf("Char('%c')\n", e->as.char_val); break;
        case EXPR_BOOL_LIT:   printf("Bool(%s)\n", e->as.bool_val ? "true" : "false"); break;
        case EXPR_NULL_LIT:   printf("Null\n"); break;
        case EXPR_IDENT:      printf("Ident(%s)\n", e->as.ident); break;
        case EXPR_UNARY:
            printf("Unary(%s)\n", token_type_name(e->as.unary.op));
            print_expr(e->as.unary.operand, depth + 1);
            break;
        case EXPR_POSTFIX:
            printf("Postfix(%s)\n", token_type_name(e->as.postfix.op));
            print_expr(e->as.postfix.operand, depth + 1);
            break;
        case EXPR_BINARY:
            printf("Binary(%s)\n", token_type_name(e->as.binary.op));
            print_expr(e->as.binary.left, depth + 1);
            print_expr(e->as.binary.right, depth + 1);
            break;
        case EXPR_ASSIGN:
            printf("Assign(%s)\n", token_type_name(e->as.assign.op));
            print_expr(e->as.assign.target, depth + 1);
            print_expr(e->as.assign.value, depth + 1);
            break;
        case EXPR_CALL:
            printf("Call\n");
            print_expr(e->as.call.callee, depth + 1);
            for (size_t i = 0; i < e->as.call.arg_count; i++)
                print_expr(e->as.call.args[i], depth + 1);
            break;
        case EXPR_INDEX:
            printf("Index\n");
            print_expr(e->as.index.array, depth + 1);
            print_expr(e->as.index.index, depth + 1);
            break;
        case EXPR_MEMBER:
            printf("Member(%s %s)\n", e->as.member.arrow ? "->" : ".", e->as.member.field);
            print_expr(e->as.member.object, depth + 1);
            break;
    }
}

static void print_stmt(const Stmt *s, int depth) {
    if (!s) { indent(depth); printf("<null-stmt>\n"); return; }
    indent(depth);
    switch (s->kind) {
        case STMT_EXPR:
            printf("ExprStmt\n");
            print_expr(s->as.expr_stmt.expr, depth + 1);
            break;
        case STMT_VAR_DECL:
            printf("VarDecl(%s: ", s->as.var_decl.name);
            print_type(&s->as.var_decl.type);
            printf(")\n");
            if (s->as.var_decl.init) print_expr(s->as.var_decl.init, depth + 1);
            break;
        case STMT_BLOCK:
            printf("Block\n");
            for (size_t i = 0; i < s->as.block.count; i++)
                print_stmt(s->as.block.stmts[i], depth + 1);
            break;
        case STMT_IF:
            printf("If\n");
            print_expr(s->as.if_stmt.cond, depth + 1);
            print_stmt(s->as.if_stmt.then_branch, depth + 1);
            if (s->as.if_stmt.else_branch) print_stmt(s->as.if_stmt.else_branch, depth + 1);
            break;
        case STMT_WHILE:
            printf("While\n");
            print_expr(s->as.while_stmt.cond, depth + 1);
            print_stmt(s->as.while_stmt.body, depth + 1);
            break;
        case STMT_FOR:
            printf("For\n");
            if (s->as.for_stmt.init) print_stmt(s->as.for_stmt.init, depth + 1);
            if (s->as.for_stmt.cond) print_expr(s->as.for_stmt.cond, depth + 1);
            if (s->as.for_stmt.post) print_expr(s->as.for_stmt.post, depth + 1);
            print_stmt(s->as.for_stmt.body, depth + 1);
            break;
        case STMT_BREAK:    printf("Break\n"); break;
        case STMT_CONTINUE: printf("Continue\n"); break;
        case STMT_YIELD:
            printf("Yield\n");
            if (s->as.yield_stmt.value) print_expr(s->as.yield_stmt.value, depth + 1);
            break;
    }
}

void ast_print_program(const Program *prog) {
    for (size_t i = 0; i < prog->count; i++) {
        Decl *d = &prog->decls[i];
        switch (d->kind) {
            case DECL_FUNC: {
                FuncDecl *f = &d->as.func;
                printf("%sFunc %s(", f->is_extern ? "Extern " : "", f->name);
                for (size_t p = 0; p < f->param_count; p++) {
                    if (p) printf(", ");
                    printf("%s: ", f->params[p].name);
                    print_type(&f->params[p].type);
                }
                printf(") -> ");
                if (f->has_return_type) print_type(&f->return_type); else printf("void");
                printf("\n");
                if (f->is_extern) { indent(1); printf("(extern — no body)\n"); }
                else print_stmt(f->body, 1);
                break;
            }
            case DECL_STRUCT: {
                StructDecl *sd = &d->as.struct_decl;
                printf("Struct %s\n", sd->name);
                for (size_t fidx = 0; fidx < sd->field_count; fidx++) {
                    indent(1);
                    printf("%s: ", sd->fields[fidx].name);
                    print_type(&sd->fields[fidx].type);
                    printf("\n");
                }
                break;
            }
            case DECL_VAR:
                printf("Global");
                print_stmt(d->as.var, 0);
                break;
        }
        printf("\n");
    }
}

/* ---------------------------------------------------------------------- */
/* Cleanup                                                                  */
/* ---------------------------------------------------------------------- */

static void free_type(Type *t) { free(t->name); }

static void free_expr(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_STRING_LIT: free(e->as.str_lit.value); break;
        case EXPR_IDENT: free(e->as.ident); break;
        case EXPR_UNARY: free_expr(e->as.unary.operand); break;
        case EXPR_POSTFIX: free_expr(e->as.postfix.operand); break;
        case EXPR_BINARY: free_expr(e->as.binary.left); free_expr(e->as.binary.right); break;
        case EXPR_ASSIGN: free_expr(e->as.assign.target); free_expr(e->as.assign.value); break;
        case EXPR_CALL:
            free_expr(e->as.call.callee);
            for (size_t i = 0; i < e->as.call.arg_count; i++) free_expr(e->as.call.args[i]);
            free(e->as.call.args);
            break;
        case EXPR_INDEX: free_expr(e->as.index.array); free_expr(e->as.index.index); break;
        case EXPR_MEMBER: free_expr(e->as.member.object); free(e->as.member.field); break;
        default: break;
    }
    free(e);
}

static void free_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_EXPR: free_expr(s->as.expr_stmt.expr); break;
        case STMT_VAR_DECL:
            free(s->as.var_decl.name);
            free_type(&s->as.var_decl.type);
            free_expr(s->as.var_decl.init);
            break;
        case STMT_BLOCK:
            for (size_t i = 0; i < s->as.block.count; i++) free_stmt(s->as.block.stmts[i]);
            free(s->as.block.stmts);
            break;
        case STMT_IF:
            free_expr(s->as.if_stmt.cond);
            free_stmt(s->as.if_stmt.then_branch);
            free_stmt(s->as.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            free_expr(s->as.while_stmt.cond);
            free_stmt(s->as.while_stmt.body);
            break;
        case STMT_FOR:
            free_stmt(s->as.for_stmt.init);
            free_expr(s->as.for_stmt.cond);
            free_expr(s->as.for_stmt.post);
            free_stmt(s->as.for_stmt.body);
            break;
        case STMT_YIELD: free_expr(s->as.yield_stmt.value); break;
        default: break;
    }
    free(s);
}

void ast_free_program(Program *prog) {
    for (size_t i = 0; i < prog->count; i++) {
        Decl *d = &prog->decls[i];
        switch (d->kind) {
            case DECL_FUNC:
                free(d->as.func.name);
                free_type(&d->as.func.return_type);
                for (size_t p = 0; p < d->as.func.param_count; p++) {
                    free(d->as.func.params[p].name);
                    free_type(&d->as.func.params[p].type);
                }
                free(d->as.func.params);
                free_stmt(d->as.func.body);
                break;
            case DECL_STRUCT:
                free(d->as.struct_decl.name);
                for (size_t f = 0; f < d->as.struct_decl.field_count; f++) {
                    free(d->as.struct_decl.fields[f].name);
                    free_type(&d->as.struct_decl.fields[f].type);
                }
                free(d->as.struct_decl.fields);
                break;
            case DECL_VAR:
                free_stmt(d->as.var);
                break;
        }
    }
    free(prog->decls);
    prog->decls = NULL;
    prog->count = 0;
}
