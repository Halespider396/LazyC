#include "codegen_c.h"
#include "lexer.h"
#include <string.h>

/* ---- type mapping ---------------------------------------------------------
 * LazyC "string" has no C equivalent primitive — it's treated as an
 * implicit `const char *`, so a LazyC `string` (ptr_depth 0) becomes one
 * star of C pointer on top of `const char`, and each further `*` the
 * LazyC source added stacks on top of that.
 *
 * LazyC's `T[]` (no size, just "array of") has no direct C counterpart
 * either, since a real C array needs a compile-time size we were never
 * given. It's emitted as one more level of pointer indirection — the
 * same representation you'd reach for by hand for an unsized C array
 * parameter anyway. */

static const char *c_base_name(const char *lazyc_name, int *extra_stars) {
    *extra_stars = 0;
    if (strcmp(lazyc_name, "int") == 0)   return "int";
    if (strcmp(lazyc_name, "float") == 0) return "double";
    if (strcmp(lazyc_name, "char") == 0)  return "char";
    if (strcmp(lazyc_name, "bool") == 0)  return "bool";
    if (strcmp(lazyc_name, "void") == 0)  return "void";
    if (strcmp(lazyc_name, "string") == 0) { *extra_stars = 1; return "const char"; }
    if (strcmp(lazyc_name, "ptr") == 0)    { *extra_stars = 1; return "void"; }
    return lazyc_name; /* a struct type: same name on both sides */
}

static void emit_type(FILE *out, const Type *t) {
    int extra_stars;
    const char *base = c_base_name(t->name, &extra_stars);
    int stars = t->ptr_depth + extra_stars + (t->is_array ? 1 : 0);
    fputs(base, out);
    for (int i = 0; i < stars; i++) fputc('*', out);
}

/* ---- operator tokens -> C text --------------------------------------- */

static const char *op_str(int tok) {
    switch (tok) {
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_AMP: return "&";
        case TOK_BITOR: return "|";
        case TOK_BITXOR: return "^";
        case TOK_SHL: return "<<";
        case TOK_SHR: return ">>";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LE: return "<=";
        case TOK_GE: return ">=";
        case TOK_EQ: return "==";
        case TOK_NEQ: return "!=";
        case TOK_AND: return "&&";
        case TOK_OR: return "||";
        case TOK_ASSIGN: return "=";
        case TOK_PLUS_EQ: return "+=";
        case TOK_MINUS_EQ: return "-=";
        case TOK_STAR_EQ: return "*=";
        case TOK_SLASH_EQ: return "/=";
        case TOK_NOT: return "!";
        case TOK_BITNOT: return "~";
        case TOK_INC: return "++";
        case TOK_DEC: return "--";
        default: return "?";
    }
}

/* ---- expressions ------------------------------------------------------- */

static void emit_expr(FILE *out, const Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT:    fprintf(out, "%ld", e->as.int_val); break;
        case EXPR_FLOAT_LIT:  fprintf(out, "%f", e->as.float_val); break;
        case EXPR_STRING_LIT: fputs(e->as.str_lit.value, out); break; /* already quoted raw text */
        case EXPR_CHAR_LIT:   fprintf(out, "((char)0x%02x)", (unsigned char)e->as.char_val); break;
        case EXPR_BOOL_LIT:   fputs(e->as.bool_val ? "((bool)true)" : "((bool)false)", out); break;
        case EXPR_NULL_LIT:   fputs("NULL", out); break;
        case EXPR_IDENT:      fputs(e->as.ident, out); break;

        case EXPR_UNARY:
            fputs(op_str(e->as.unary.op), out);
            fputc('(', out);
            emit_expr(out, e->as.unary.operand);
            fputc(')', out);
            break;

        case EXPR_POSTFIX:
            fputc('(', out);
            emit_expr(out, e->as.postfix.operand);
            fputc(')', out);
            fputs(op_str(e->as.postfix.op), out);
            break;

        case EXPR_BINARY:
            fputc('(', out);
            emit_expr(out, e->as.binary.left);
            fprintf(out, " %s ", op_str(e->as.binary.op));
            emit_expr(out, e->as.binary.right);
            fputc(')', out);
            break;

        case EXPR_ASSIGN:
            fputc('(', out);
            emit_expr(out, e->as.assign.target);
            fprintf(out, " %s ", op_str(e->as.assign.op));
            emit_expr(out, e->as.assign.value);
            fputc(')', out);
            break;

        case EXPR_CALL: {
            if (e->as.call.callee->kind == EXPR_IDENT) {
                const char *name = e->as.call.callee->as.ident;
                if (strcmp(name, "strlen") == 0) {
                    fputs("(int)strlen(", out); emit_expr(out, e->as.call.args[0]); fputc(')', out);
                    break;
                }
                if (strcmp(name, "streq") == 0) {
                    fputs("(strcmp(", out); emit_expr(out, e->as.call.args[0]);
                    fputs(", ", out); emit_expr(out, e->as.call.args[1]); fputs(") == 0)", out);
                    break;
                }
                if (strcmp(name, "abs") == 0) {
                    fputs("__lazyc_abs(", out); emit_expr(out, e->as.call.args[0]); fputc(')', out);
                    break;
                }
                if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
                    fprintf(out, "__lazyc_%s(", name);
                    emit_expr(out, e->as.call.args[0]); fputs(", ", out);
                    emit_expr(out, e->as.call.args[1]); fputc(')', out);
                    break;
                }
            }
            emit_expr(out, e->as.call.callee);
            fputc('(', out);
            for (size_t i = 0; i < e->as.call.arg_count; i++) {
                if (i) fputs(", ", out);
                emit_expr(out, e->as.call.args[i]);
            }
            fputc(')', out);
            break;
        }

        case EXPR_INDEX:
            emit_expr(out, e->as.index.array);
            fputc('[', out);
            emit_expr(out, e->as.index.index);
            fputc(']', out);
            break;

        case EXPR_MEMBER:
            emit_expr(out, e->as.member.object);
            fputs(e->as.member.arrow ? "->" : ".", out);
            fputs(e->as.member.field, out);
            break;
    }
}

/* ---- statements ---------------------------------------------------------- */

static void emit_indent(FILE *out, int depth) { for (int i = 0; i < depth; i++) fputs("    ", out); }

static void emit_stmt(FILE *out, const Stmt *s, int depth);

static void emit_block(FILE *out, const Stmt *block, int depth) {
    fputs("{\n", out);
    for (size_t i = 0; i < block->as.block.count; i++)
        emit_stmt(out, block->as.block.stmts[i], depth + 1);
    emit_indent(out, depth);
    fputc('}', out);
}

/* Emits a var-decl or expr-stmt *without* a trailing ';' or newline —
 * used for the init clause of a C for(...) loop, which supplies its own
 * semicolons. */
static void emit_simple_stmt_inline(FILE *out, const Stmt *s) {
    if (s->kind == STMT_VAR_DECL) {
        emit_type(out, &s->as.var_decl.type);
        fprintf(out, " %s", s->as.var_decl.name);
        if (s->as.var_decl.init) {
            fputs(" = ", out);
            emit_expr(out, s->as.var_decl.init);
        }
    } else { /* STMT_EXPR */
        emit_expr(out, s->as.expr_stmt.expr);
    }
}

static int is_print_call(const Stmt *s) {
    if (s->kind != STMT_EXPR) return 0;
    const Expr *e = s->as.expr_stmt.expr;
    return e->kind == EXPR_CALL && e->as.call.callee->kind == EXPR_IDENT &&
           strcmp(e->as.call.callee->as.ident, "print") == 0;
}

static void emit_print_stmt(FILE *out, const Stmt *s) {
    const Expr *e = s->as.expr_stmt.expr;
    fputs("{ ", out);
    for (size_t i = 0; i < e->as.call.arg_count; i++) {
        if (i) fputs("printf(\" \"); ", out);
        fputs("__lazyc_print1(", out);
        emit_expr(out, e->as.call.args[i]);
        fputs("); ", out);
    }
    fputs("printf(\"\\n\"); }\n", out);
}

static void emit_stmt(FILE *out, const Stmt *s, int depth) {
    emit_indent(out, depth);
    if (is_print_call(s)) { emit_print_stmt(out, s); return; }
    switch (s->kind) {
        case STMT_EXPR:
            emit_expr(out, s->as.expr_stmt.expr);
            fputs(";\n", out);
            break;

        case STMT_VAR_DECL:
            emit_simple_stmt_inline(out, s);
            fputs(";\n", out);
            break;

        case STMT_BLOCK:
            emit_block(out, s, depth);
            fputc('\n', out);
            break;

        case STMT_IF:
            fputs("if (", out);
            emit_expr(out, s->as.if_stmt.cond);
            fputs(") ", out);
            emit_block(out, s->as.if_stmt.then_branch, depth);
            if (s->as.if_stmt.else_branch) {
                fputs(" else ", out);
                if (s->as.if_stmt.else_branch->kind == STMT_IF) {
                    /* chained `else if` — recurse without its own indent/newline */
                    fputs("", out);
                    /* print "if (...) { ... } else ..." for the nested if,
                     * reusing emit_stmt's IF case but suppressing the
                     * leading indent it would normally add */
                    const Stmt *nested = s->as.if_stmt.else_branch;
                    fputs("if (", out);
                    emit_expr(out, nested->as.if_stmt.cond);
                    fputs(") ", out);
                    emit_block(out, nested->as.if_stmt.then_branch, depth);
                    if (nested->as.if_stmt.else_branch) {
                        fputs(" else ", out);
                        emit_block(out, nested->as.if_stmt.else_branch, depth);
                    }
                } else {
                    emit_block(out, s->as.if_stmt.else_branch, depth);
                }
            }
            fputc('\n', out);
            break;

        case STMT_WHILE:
            fputs("while (", out);
            emit_expr(out, s->as.while_stmt.cond);
            fputs(") ", out);
            emit_block(out, s->as.while_stmt.body, depth);
            fputc('\n', out);
            break;

        case STMT_FOR:
            fputs("for (", out);
            if (s->as.for_stmt.init) emit_simple_stmt_inline(out, s->as.for_stmt.init);
            fputs("; ", out);
            if (s->as.for_stmt.cond) emit_expr(out, s->as.for_stmt.cond);
            fputs("; ", out);
            if (s->as.for_stmt.post) emit_expr(out, s->as.for_stmt.post);
            fputs(") ", out);
            emit_block(out, s->as.for_stmt.body, depth);
            fputc('\n', out);
            break;

        case STMT_BREAK:    fputs("break;\n", out); break;
        case STMT_CONTINUE: fputs("continue;\n", out); break;

        case STMT_YIELD:
            fputs("return", out);
            if (s->as.yield_stmt.value) {
                fputc(' ', out);
                emit_expr(out, s->as.yield_stmt.value);
            }
            fputs(";\n", out);
            break;
    }
}

/* ---- top level ------------------------------------------------------------ */

static void emit_func_signature(FILE *out, const FuncDecl *f) {
    if (f->has_return_type) emit_type(out, &f->return_type); else fputs("void", out);
    fprintf(out, " %s(", f->name);
    if (f->param_count == 0) {
        fputs("void", out);
    } else {
        for (size_t p = 0; p < f->param_count; p++) {
            if (p) fputs(", ", out);
            emit_type(out, &f->params[p].type);
            fprintf(out, " %s", f->params[p].name);
        }
    }
    fputc(')', out);
}

int codegen_c_write(const Program *prog, FILE *out) {
    fputs("/* Generated by the LazyC -> C transpiler. Do not edit by hand. */\n", out);
    fputs("#include <stdio.h>\n", out);
    fputs("#include <stdlib.h>\n", out);
    fputs("#include <stdbool.h>\n", out);
    fputs("#include <string.h>\n", out);
    fputs("#include <math.h>\n\n", out);

    /* Runtime helpers for LazyC's builtins. print() dispatches per-argument
     * via _Generic to a real, singly-typed helper function (not an inlined
     * printf per branch) specifically so an unselected branch never gets
     * its own mismatched-format-string compile warning. */
    fputs("#if defined(__GNUC__) || defined(__clang__)\n"
          "#define __LAZYC_UNUSED __attribute__((unused))\n"
          "#else\n"
          "#define __LAZYC_UNUSED\n"
          "#endif\n", out);
    fputs("static __LAZYC_UNUSED void __lazyc_p_int(int v){printf(\"%d\", v);}\n", out);
    fputs("static __LAZYC_UNUSED void __lazyc_p_double(double v){printf(\"%g\", v);}\n", out);
    fputs("static __LAZYC_UNUSED void __lazyc_p_bool(bool v){printf(\"%s\", v ? \"true\" : \"false\");}\n", out);
    fputs("static __LAZYC_UNUSED void __lazyc_p_char(char v){printf(\"%c\", v);}\n", out);
    fputs("static __LAZYC_UNUSED void __lazyc_p_str(const char *v){printf(\"%s\", v);}\n", out);
    fputs("static __LAZYC_UNUSED void __lazyc_p_ptr(const void *v){printf(\"<ptr:%p>\", v);}\n", out);
    fputs("#define __lazyc_print1(x) _Generic((x), \\\n"
          "    int: __lazyc_p_int, double: __lazyc_p_double, bool: __lazyc_p_bool, \\\n"
          "    char: __lazyc_p_char, char*: __lazyc_p_str, const char*: __lazyc_p_str, \\\n"
          "    default: __lazyc_p_ptr)(x)\n", out);
    fputs("#define __lazyc_min(a,b) ((a) < (b) ? (a) : (b))\n", out);
    fputs("#define __lazyc_max(a,b) ((a) > (b) ? (a) : (b))\n", out);
    fputs("#define __lazyc_abs(x) _Generic((x), int: abs, double: fabs)(x)\n\n", out);

    /* Forward tag declarations for every struct, so pointer fields can
     * reference a struct defined later in the file (or itself). */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind == DECL_STRUCT)
            fprintf(out, "typedef struct %s %s;\n", prog->decls[i].as.struct_decl.name,
                    prog->decls[i].as.struct_decl.name);
    }
    fputc('\n', out);

    /* Struct bodies. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_STRUCT) continue;
        const StructDecl *sd = &prog->decls[i].as.struct_decl;
        fprintf(out, "struct %s {\n", sd->name);
        for (size_t f = 0; f < sd->field_count; f++) {
            fputs("    ", out);
            emit_type(out, &sd->fields[f].type);
            fprintf(out, " %s;\n", sd->fields[f].name);
        }
        fputs("};\n\n", out);
    }

    /* Function prototypes, so functions can call each other regardless
     * of declaration order (LazyC doesn't require forward declarations;
     * sema already checked every call resolves, so this just gives C
     * the same freedom). Extern functions are deliberately skipped here:
     * they're expected to already be declared by one of the standard
     * headers included above (stdio.h/stdlib.h/string.h/math.h) — e.g.
     * `extern work fopen(...) ptr;` calls the real fopen from stdio.h.
     * Emitting our own competing prototype would conflict with that
     * real declaration (different exact return type — void* vs FILE*,
     * say — even though C allows implicit void*-conversion at call
     * sites, redeclaration requires an *exact* type match). If a program
     * declares extern for something not covered by those headers, this
     * relies on the LazyC declaration matching a real symbol the linker
     * can find; the C compiler will report an "implicit declaration"
     * error if it can't see one, which is an honest signal to add
     * whatever real header actually declares it. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_FUNC || prog->decls[i].as.func.is_extern) continue;
        emit_func_signature(out, &prog->decls[i].as.func);
        fputs(";\n", out);
    }
    fputc('\n', out);

    /* Global variables. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_VAR) continue;
        const Stmt *v = prog->decls[i].as.var;
        emit_type(out, &v->as.var_decl.type);
        fprintf(out, " %s", v->as.var_decl.name);
        if (v->as.var_decl.init) {
            fputs(" = ", out);
            emit_expr(out, v->as.var_decl.init);
        }
        fputs(";\n", out);
    }
    fputc('\n', out);

    /* Function bodies. Extern functions have none — see above. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_FUNC || prog->decls[i].as.func.is_extern) continue;
        const FuncDecl *f = &prog->decls[i].as.func;
        emit_func_signature(out, f);
        fputs(" ", out);
        emit_block(out, f->body, 0);
        fputs("\n\n", out);
    }

    return 0;
}
