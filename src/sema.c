#include "sema.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- ephemeral "SemType" values -----------------------------------------
 * These mirror ast.h's Type but are used only as transient results of type
 * inference during checking. `name` either points at a static string literal
 * (for primitives / sentinels below) or borrows a pointer already owned by
 * the AST (variable/field/param declarations) — either way sema.c never
 * frees a `name`, so there's nothing to manage here. */

#define T_ERROR  "<error>"
#define T_NULL   "<null>"
#define T_VOID   "void"
#define T_INT    "int"
#define T_FLOAT  "float"
#define T_BOOL   "bool"
#define T_CHAR   "char"
#define T_STRING "string"
#define T_PTR    "ptr"     /* opaque C pointer, e.g. FILE* from an extern decl — maps to void* */

static Type mk(const char *name, int ptr_depth, int is_array) {
    Type t;
    t.name = (char *)name;
    t.ptr_depth = ptr_depth;
    t.is_array = is_array;
    return t;
}
static Type mk_prim(const char *name) { return mk(name, 0, 0); }
static Type mk_error(void) { return mk_prim(T_ERROR); }

static int is_error(const Type *t) { return strcmp(t->name, T_ERROR) == 0; }
static int is_null_lit(const Type *t) { return strcmp(t->name, T_NULL) == 0; }
static int is_numeric_name(const char *n) { return strcmp(n, T_INT) == 0 || strcmp(n, T_FLOAT) == 0; }
static int is_plain(const Type *t) { return t->ptr_depth == 0 && !t->is_array; }
static int is_numeric(const Type *t) { return is_plain(t) && is_numeric_name(t->name); }
static int same_type(const Type *a, const Type *b) {
    return a->ptr_depth == b->ptr_depth && a->is_array == b->is_array && strcmp(a->name, b->name) == 0;
}

/* ---- symbol tables --------------------------------------------------- */

typedef struct { char *name; const Type *type; int used; int is_param; int line, col; } VarSym;

typedef struct Scope {
    VarSym *vars;
    size_t count, cap;
    struct Scope *parent;
} Scope;

typedef struct {
    char *name;
    const Type *return_type;   /* NULL if !has_return_type */
    int has_return_type;
    const Param *params;
    size_t param_count;
    int called;                /* set when some call site resolves to this function */
} FuncSig;

typedef struct {
    char *name;
    const FieldDecl *fields;
    size_t field_count;
} StructSig;

typedef struct {
    char *name;
    const Type *type;
} GlobalSig;

typedef struct {
    FuncSig *funcs; size_t func_count;
    StructSig *structs; size_t struct_count;
    GlobalSig *globals; size_t global_count;

    Scope *scope;               /* innermost local scope, or NULL at global level */
    const Type *ret_type;       /* current function's return type (if any) */
    int has_ret_type;
    int loop_depth;
    int had_error;
} Checker;

#define DA_PUSH(arr, count, cap, item) \
    do { \
        if ((count) >= (cap)) { \
            (cap) = (cap) == 0 ? 4 : (cap) * 2; \
            (arr) = realloc((arr), (cap) * sizeof(*(arr))); \
        } \
        (arr)[(count)++] = (item); \
    } while (0)

static void sema_error(Checker *c, int line, int col, const char *fmt, ...) {
    c->had_error = 1;
    fprintf(stderr, "%d:%d: type error: ", line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static int is_known_type_name(Checker *c, const char *name) {
    if (strcmp(name, T_INT) == 0 || strcmp(name, T_FLOAT) == 0 || strcmp(name, T_BOOL) == 0 ||
        strcmp(name, T_CHAR) == 0 || strcmp(name, T_STRING) == 0 || strcmp(name, T_VOID) == 0 ||
        strcmp(name, T_PTR) == 0)
        return 1;
    for (size_t i = 0; i < c->struct_count; i++)
        if (strcmp(c->structs[i].name, name) == 0) return 1;
    return 0;
}

static const StructSig *find_struct(Checker *c, const char *name) {
    for (size_t i = 0; i < c->struct_count; i++)
        if (strcmp(c->structs[i].name, name) == 0) return &c->structs[i];
    return NULL;
}

static FuncSig *find_func(Checker *c, const char *name) {
    for (size_t i = 0; i < c->func_count; i++)
        if (strcmp(c->funcs[i].name, name) == 0) return &c->funcs[i];
    return NULL;
}

static const Type *find_global(Checker *c, const char *name) {
    for (size_t i = 0; i < c->global_count; i++)
        if (strcmp(c->globals[i].name, name) == 0) return c->globals[i].type;
    return NULL;
}

/* Any top-level name (function, struct, or global) — used to reject
 * duplicate declarations and to catch a variable reference that
 * accidentally shadows a struct/function name. */
static int is_toplevel_name_taken(Checker *c, const char *name) {
    return find_func(c, name) || find_struct(c, name) || find_global(c, name);
}

static void scope_push(Checker *c) {
    Scope *s = calloc(1, sizeof(Scope));
    s->parent = c->scope;
    c->scope = s;
}
/* A leading underscore (`_x`) opts a name out of the unused-variable
 * warning — the same convention Go and Rust use for "yes, I know". */
static int is_intentionally_unused(const char *name) { return name[0] == '_'; }

static void scope_pop(Checker *c) {
    Scope *s = c->scope;
    for (size_t i = 0; i < s->count; i++) {
        VarSym *v = &s->vars[i];
        if (!v->used && !is_intentionally_unused(v->name))
            sema_error(c, v->line, v->col, v->is_param ? "unused parameter '%s'" : "unused variable '%s'", v->name);
    }
    c->scope = s->parent;
    free(s->vars);
    free(s);
}
/* Declares in the *innermost* scope only; returns 0 (and reports an error)
 * if that exact scope already has this name — shadowing an outer scope
 * is fine and not checked here. */
static int scope_declare(Checker *c, char *name, const Type *type, int is_param, int line, int col) {
    Scope *s = c->scope;
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->vars[i].name, name) == 0) return 0;
    VarSym v = { name, type, 0, is_param, line, col };
    DA_PUSH(s->vars, s->count, s->cap, v);
    return 1;
}
static const Type *scope_lookup(Checker *c, const char *name) {
    for (Scope *s = c->scope; s; s = s->parent)
        for (size_t i = 0; i < s->count; i++)
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].used = 1;
                return s->vars[i].type;
            }
    return find_global(c, name);
}

/* ---- assignability ------------------------------------------------------ */

static int assignable(const Type *target, const Type *value) {
    if (is_error(target) || is_error(value)) return 1; /* don't cascade */
    if (is_null_lit(value)) return target->ptr_depth > 0;
    if (same_type(target, value)) return 1;
    if (is_numeric(target) && is_numeric(value)) return 1; /* int <-> float */
    return 0;
}

static void type_to_str(const Type *t, char *buf, size_t bufsz) {
    char stars[16] = {0};
    int n = t->ptr_depth < 15 ? t->ptr_depth : 15;
    for (int i = 0; i < n; i++) stars[i] = '*';
    snprintf(buf, bufsz, "%s%s%s", stars, t->name, t->is_array ? "[]" : "");
}

/* ---- expression type inference ------------------------------------------ */

static Type infer_expr(Checker *c, Expr *e);

static Type infer_unary(Checker *c, Expr *e) {
    Type operand = infer_expr(c, e->as.unary.operand);
    switch (e->as.unary.op) {
        case TOK_AMP:
            return mk(operand.name, operand.ptr_depth + 1, 0);
        case TOK_STAR:
            if (is_error(&operand)) return mk_error();
            if (operand.ptr_depth == 0) {
                char buf[64]; type_to_str(&operand, buf, sizeof(buf));
                sema_error(c, e->line, e->col, "cannot dereference non-pointer type '%s'", buf);
                return mk_error();
            }
            return mk(operand.name, operand.ptr_depth - 1, operand.is_array);
        case TOK_MINUS:
        case TOK_BITNOT:
            if (is_error(&operand)) return mk_error();
            if (!is_numeric(&operand)) {
                char buf[64]; type_to_str(&operand, buf, sizeof(buf));
                sema_error(c, e->line, e->col, "operator requires a numeric operand, got '%s'", buf);
                return mk_error();
            }
            return operand;
        case TOK_NOT:
            return mk_prim(T_BOOL);
        case TOK_INC:
        case TOK_DEC:
            if (is_error(&operand)) return mk_error();
            if (!is_numeric(&operand) && operand.ptr_depth == 0) {
                char buf[64]; type_to_str(&operand, buf, sizeof(buf));
                sema_error(c, e->line, e->col, "'++'/'--' require a numeric or pointer operand, got '%s'", buf);
                return mk_error();
            }
            return operand;
        default:
            return mk_error();
    }
}

static int is_lvalue(const Expr *e) {
    if (e->kind == EXPR_IDENT || e->kind == EXPR_INDEX || e->kind == EXPR_MEMBER) return 1;
    if (e->kind == EXPR_UNARY && e->as.unary.op == TOK_STAR) return 1;
    return 0;
}

static Type infer_binary(Checker *c, Expr *e) {
    Type l = infer_expr(c, e->as.binary.left);
    Type r = infer_expr(c, e->as.binary.right);
    int op = e->as.binary.op;
    if (is_error(&l) || is_error(&r)) return mk_error();

    switch (op) {
        case TOK_PLUS: case TOK_MINUS:
            /* pointer +/- int is allowed; produces the pointer type */
            if (l.ptr_depth > 0 && is_plain(&r) && strcmp(r.name, T_INT) == 0) return l;
            if (op == TOK_PLUS && r.ptr_depth > 0 && is_plain(&l) && strcmp(l.name, T_INT) == 0) return r;
            __attribute__((fallthrough));
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
            if (!is_numeric(&l) || !is_numeric(&r)) {
                char lb[64], rb[64]; type_to_str(&l, lb, sizeof(lb)); type_to_str(&r, rb, sizeof(rb));
                sema_error(c, e->line, e->col, "arithmetic requires numeric operands, got '%s' and '%s'", lb, rb);
                return mk_error();
            }
            return (strcmp(l.name, T_FLOAT) == 0 || strcmp(r.name, T_FLOAT) == 0) ? mk_prim(T_FLOAT) : mk_prim(T_INT);

        case TOK_AMP: case TOK_BITOR: case TOK_BITXOR: case TOK_SHL: case TOK_SHR:
            if (!is_plain(&l) || !is_plain(&r) || strcmp(l.name, T_INT) != 0 || strcmp(r.name, T_INT) != 0) {
                char lb[64], rb[64]; type_to_str(&l, lb, sizeof(lb)); type_to_str(&r, rb, sizeof(rb));
                sema_error(c, e->line, e->col, "bitwise operator requires 'int' operands, got '%s' and '%s'", lb, rb);
                return mk_error();
            }
            return mk_prim(T_INT);

        case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE:
            if (!((is_numeric(&l) && is_numeric(&r)) || (l.ptr_depth > 0 && r.ptr_depth > 0))) {
                char lb[64], rb[64]; type_to_str(&l, lb, sizeof(lb)); type_to_str(&r, rb, sizeof(rb));
                sema_error(c, e->line, e->col, "comparison requires numeric or pointer operands, got '%s' and '%s'", lb, rb);
                return mk_error();
            }
            return mk_prim(T_BOOL);

        case TOK_EQ: case TOK_NEQ:
            if (!assignable(&l, &r) && !assignable(&r, &l)) {
                char lb[64], rb[64]; type_to_str(&l, lb, sizeof(lb)); type_to_str(&r, rb, sizeof(rb));
                sema_error(c, e->line, e->col, "cannot compare incompatible types '%s' and '%s'", lb, rb);
                return mk_error();
            }
            return mk_prim(T_BOOL);

        case TOK_AND: case TOK_OR:
            return mk_prim(T_BOOL);

        default:
            return mk_error();
    }
}

static int is_reserved_builtin_name(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "strlen") == 0 || strcmp(name, "streq") == 0 ||
           strcmp(name, "abs") == 0 || strcmp(name, "min") == 0 || strcmp(name, "max") == 0;
}

/* print(...) is variadic and accepts any number of arguments of any
 * printable type — everything except a bare struct, a raw array, or void
 * (e.g. the result of calling a void function). Always returns void, so
 * it can only be used as a standalone statement (using it anywhere a
 * value is required fails type-checking the same way any other void
 * expression would). */
static Type infer_print_call(Checker *c, Expr *e) {
    for (size_t i = 0; i < e->as.call.arg_count; i++) {
        Expr *arg = e->as.call.args[i];
        Type t = infer_expr(c, arg);
        if (is_error(&t)) continue;
        if (is_plain(&t) && find_struct(c, t.name)) {
            sema_error(c, arg->line, arg->col, "print() can't take struct '%s' directly — pass a field or a pointer", t.name);
        } else if (t.is_array) {
            sema_error(c, arg->line, arg->col, "print() can't take an array directly");
        } else if (is_plain(&t) && strcmp(t.name, T_VOID) == 0) {
            sema_error(c, arg->line, arg->col, "print() can't take a void value (result of calling a void function)");
        }
    }
    return mk_prim(T_VOID);
}

/* Fixed-arity builtins shared by both backends: strlen/streq operate on
 * `string`; abs/min/max are polymorphic over int/float (mirroring the
 * same numeric-coercion rule infer_binary uses for arithmetic). */
static Type infer_fixed_builtin_call(Checker *c, Expr *e, const char *name) {
    size_t want = (strcmp(name, "strlen") == 0 || strcmp(name, "abs") == 0) ? 1 : 2;
    if (e->as.call.arg_count != want) {
        sema_error(c, e->line, e->col, "'%s' expects %zu argument%s, got %zu",
                   name, want, want == 1 ? "" : "s", e->as.call.arg_count);
    }
    Type args[2] = { mk_error(), mk_error() };
    for (size_t i = 0; i < e->as.call.arg_count && i < 2; i++) args[i] = infer_expr(c, e->as.call.args[i]);
    for (size_t i = 2; i < e->as.call.arg_count; i++) infer_expr(c, e->as.call.args[i]); /* extras: still check internally */

    if (strcmp(name, "strlen") == 0 || strcmp(name, "streq") == 0) {
        for (size_t i = 0; i < want && i < e->as.call.arg_count; i++) {
            if (is_error(&args[i])) continue;
            if (!(is_plain(&args[i]) && strcmp(args[i].name, T_STRING) == 0)) {
                char b[64]; type_to_str(&args[i], b, sizeof(b));
                sema_error(c, e->as.call.args[i]->line, e->as.call.args[i]->col,
                           "'%s' expects a 'string' argument, got '%s'", name, b);
            }
        }
        return strcmp(name, "strlen") == 0 ? mk_prim(T_INT) : mk_prim(T_BOOL);
    }

    /* abs/min/max: numeric, int/float mixing allowed like ordinary arithmetic */
    for (size_t i = 0; i < want && i < e->as.call.arg_count; i++) {
        if (is_error(&args[i])) continue;
        if (!is_numeric(&args[i])) {
            char b[64]; type_to_str(&args[i], b, sizeof(b));
            sema_error(c, e->as.call.args[i]->line, e->as.call.args[i]->col,
                       "'%s' expects a numeric argument, got '%s'", name, b);
        }
    }
    int any_float = (want > 0 && is_numeric(&args[0]) && strcmp(args[0].name, T_FLOAT) == 0) ||
                     (want > 1 && is_numeric(&args[1]) && strcmp(args[1].name, T_FLOAT) == 0);
    return mk_prim(any_float ? T_FLOAT : T_INT);
}

static Type infer_call(Checker *c, Expr *e) {
    if (e->as.call.callee->kind != EXPR_IDENT) {
        sema_error(c, e->line, e->col, "expression is not callable");
        for (size_t i = 0; i < e->as.call.arg_count; i++) infer_expr(c, e->as.call.args[i]);
        return mk_error();
    }
    const char *name = e->as.call.callee->as.ident;
    if (strcmp(name, "print") == 0) return infer_print_call(c, e);
    if (strcmp(name, "strlen") == 0 || strcmp(name, "streq") == 0 ||
        strcmp(name, "abs") == 0 || strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
        return infer_fixed_builtin_call(c, e, name);

    FuncSig *fn = find_func(c, name);
    if (!fn) {
        sema_error(c, e->line, e->col, "call to undefined function '%s'", name);
        for (size_t i = 0; i < e->as.call.arg_count; i++) infer_expr(c, e->as.call.args[i]);
        return mk_error();
    }
    fn->called = 1;
    if (e->as.call.arg_count != fn->param_count) {
        sema_error(c, e->line, e->col, "'%s' expects %zu argument%s, got %zu",
                   name, fn->param_count, fn->param_count == 1 ? "" : "s", e->as.call.arg_count);
    }
    size_t n = e->as.call.arg_count < fn->param_count ? e->as.call.arg_count : fn->param_count;
    for (size_t i = 0; i < n; i++) {
        Type arg_t = infer_expr(c, e->as.call.args[i]);
        const Type *param_t = &fn->params[i].type;
        if (!assignable(param_t, &arg_t)) {
            char pb[64], ab[64]; type_to_str(param_t, pb, sizeof(pb)); type_to_str(&arg_t, ab, sizeof(ab));
            sema_error(c, e->as.call.args[i]->line, e->as.call.args[i]->col,
                       "argument %zu to '%s' expects '%s', got '%s'", i + 1, name, pb, ab);
        }
    }
    for (size_t i = n; i < e->as.call.arg_count; i++) infer_expr(c, e->as.call.args[i]); /* extras: still check internally */
    return fn->has_return_type ? *fn->return_type : mk_prim(T_VOID);
}

static Type infer_index(Checker *c, Expr *e) {
    Type arr = infer_expr(c, e->as.index.array);
    Type idx = infer_expr(c, e->as.index.index);
    if (!is_error(&idx) && !(is_plain(&idx) && strcmp(idx.name, T_INT) == 0)) {
        char ib[64]; type_to_str(&idx, ib, sizeof(ib));
        sema_error(c, e->as.index.index->line, e->as.index.index->col, "array index must be 'int', got '%s'", ib);
    }
    if (is_error(&arr)) return mk_error();
    if (arr.is_array) return mk(arr.name, arr.ptr_depth, 0);
    if (arr.ptr_depth > 0) return mk(arr.name, arr.ptr_depth - 1, 0);
    char ab[64]; type_to_str(&arr, ab, sizeof(ab));
    sema_error(c, e->line, e->col, "cannot index into non-array, non-pointer type '%s'", ab);
    return mk_error();
}

static Type infer_member(Checker *c, Expr *e) {
    Type obj = infer_expr(c, e->as.member.object);
    if (is_error(&obj)) return mk_error();

    int arrow = e->as.member.arrow;
    const char *struct_name = obj.name;
    int effective_ptr_depth = obj.ptr_depth;

    if (arrow) {
        if (effective_ptr_depth != 1) {
            char ob[64]; type_to_str(&obj, ob, sizeof(ob));
            sema_error(c, e->line, e->col, "'->' requires a pointer-to-struct, got '%s'", ob);
            return mk_error();
        }
    } else {
        if (effective_ptr_depth != 0) {
            char ob[64]; type_to_str(&obj, ob, sizeof(ob));
            sema_error(c, e->line, e->col, "use '->' instead of '.' on pointer type '%s'", ob);
            return mk_error();
        }
    }

    const StructSig *sd = find_struct(c, struct_name);
    if (!sd) {
        sema_error(c, e->line, e->col, "'%s' is not a struct type", struct_name);
        return mk_error();
    }
    for (size_t i = 0; i < sd->field_count; i++) {
        if (strcmp(sd->fields[i].name, e->as.member.field) == 0)
            return sd->fields[i].type;
    }
    sema_error(c, e->line, e->col, "struct '%s' has no member '%s'", struct_name, e->as.member.field);
    return mk_error();
}

static Type infer_assign(Checker *c, Expr *e) {
    Expr *target = e->as.assign.target;
    if (!is_lvalue(target)) {
        sema_error(c, target->line, target->col, "invalid assignment target");
        infer_expr(c, target);
        infer_expr(c, e->as.assign.value);
        return mk_error();
    }
    Type tt = infer_expr(c, target);
    Type vt = infer_expr(c, e->as.assign.value);
    if (is_error(&tt) || is_error(&vt)) return mk_error();

    if (e->as.assign.op == TOK_ASSIGN) {
        if (!assignable(&tt, &vt)) {
            char tb[64], vb[64]; type_to_str(&tt, tb, sizeof(tb)); type_to_str(&vt, vb, sizeof(vb));
            sema_error(c, e->line, e->col, "cannot assign '%s' to a variable of type '%s'", vb, tb);
            return mk_error();
        }
    } else {
        if (!is_numeric(&tt) || !is_numeric(&vt)) {
            char tb[64], vb[64]; type_to_str(&tt, tb, sizeof(tb)); type_to_str(&vt, vb, sizeof(vb));
            sema_error(c, e->line, e->col, "compound assignment requires numeric operands, got '%s' and '%s'", tb, vb);
            return mk_error();
        }
    }
    return tt;
}

static Type infer_expr(Checker *c, Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT:    return mk_prim(T_INT);
        case EXPR_FLOAT_LIT:  return mk_prim(T_FLOAT);
        case EXPR_STRING_LIT: return mk_prim(T_STRING);
        case EXPR_CHAR_LIT:   return mk_prim(T_CHAR);
        case EXPR_BOOL_LIT:   return mk_prim(T_BOOL);
        case EXPR_NULL_LIT:   return mk_prim(T_NULL);

        case EXPR_IDENT: {
            const Type *t = scope_lookup(c, e->as.ident);
            if (!t) {
                if (find_func(c, e->as.ident) || find_struct(c, e->as.ident))
                    sema_error(c, e->line, e->col, "'%s' is a function/type name, not a variable", e->as.ident);
                else
                    sema_error(c, e->line, e->col, "use of undeclared identifier '%s'", e->as.ident);
                return mk_error();
            }
            return *t;
        }

        case EXPR_UNARY:   return infer_unary(c, e);
        case EXPR_POSTFIX: {
            Type t = infer_expr(c, e->as.postfix.operand);
            if (!is_error(&t) && !is_numeric(&t) && t.ptr_depth == 0) {
                char tb[64]; type_to_str(&t, tb, sizeof(tb));
                sema_error(c, e->line, e->col, "'++'/'--' require a numeric or pointer operand, got '%s'", tb);
                return mk_error();
            }
            return t;
        }
        case EXPR_BINARY:  return infer_binary(c, e);
        case EXPR_ASSIGN:  return infer_assign(c, e);
        case EXPR_CALL:    return infer_call(c, e);
        case EXPR_INDEX:   return infer_index(c, e);
        case EXPR_MEMBER:  return infer_member(c, e);
    }
    return mk_error();
}

/* Conditions (if/while/for) accept anything "truthy": bool, int, or a
 * pointer (non-NULL check). Structs, arrays, void, and string are rejected. */
static void check_condition(Checker *c, Expr *cond) {
    Type t = infer_expr(c, cond);
    if (is_error(&t)) return;
    if (t.ptr_depth > 0) return;
    if (is_plain(&t) && (strcmp(t.name, T_BOOL) == 0 || strcmp(t.name, T_INT) == 0)) return;
    char tb[64]; type_to_str(&t, tb, sizeof(tb));
    sema_error(c, cond->line, cond->col, "condition must be 'bool', 'int', or a pointer, got '%s'", tb);
}

/* ---- control-flow analysis ------------------------------------------------
 *
 * stmt_diverges: true if control can never fall through past this statement
 * (it always yields, breaks, or continues, or is an if/else where both
 * branches diverge). Used to flag unreachable code within a block.
 *
 * guarantees_yield: true if this statement guarantees the *function*
 * yields a value before it could fall through — stricter than
 * stmt_diverges, since break/continue only escape a loop, they don't
 * make the enclosing function return. Used for the "missing yield on
 * some path" check on a non-void function's body.
 *
 * Known simplification: while/for loops are never treated as guaranteeing
 * (or, for stmt_diverges, as diverging) even when they're provably infinite
 * (e.g. `while (true) { ... }` with no break). Handling that precisely means
 * tracking which loop a break targets across nesting, which isn't worth the
 * complexity here — the practical effect is you may need a trailing
 * (possibly unreachable-looking) yield after such a loop, which most
 * compilers also ask for. */

static int stmt_diverges(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
        case STMT_YIELD:
        case STMT_BREAK:
        case STMT_CONTINUE:
            return 1;
        case STMT_IF:
            return s->as.if_stmt.else_branch != NULL &&
                   stmt_diverges(s->as.if_stmt.then_branch) &&
                   stmt_diverges(s->as.if_stmt.else_branch);
        case STMT_BLOCK:
            for (size_t i = 0; i < s->as.block.count; i++)
                if (stmt_diverges(s->as.block.stmts[i])) return 1;
            return 0;
        default:
            return 0;
    }
}

static int guarantees_yield(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
        case STMT_YIELD:
            return 1;
        case STMT_IF:
            return s->as.if_stmt.else_branch != NULL &&
                   guarantees_yield(s->as.if_stmt.then_branch) &&
                   guarantees_yield(s->as.if_stmt.else_branch);
        case STMT_BLOCK:
            for (size_t i = 0; i < s->as.block.count; i++)
                if (guarantees_yield(s->as.block.stmts[i])) return 1;
            return 0;
        default:
            return 0; /* while/for/break/continue/expr/vardecl: no guarantee */
    }
}

/* ---- statement checking -------------------------------------------------- */

static void check_stmt(Checker *c, Stmt *s);

static void check_block_contents(Checker *c, Stmt *block) {
    int unreachable = 0;
    for (size_t i = 0; i < block->as.block.count; i++) {
        Stmt *s = block->as.block.stmts[i];
        if (unreachable) {
            sema_error(c, s->line, s->col, "unreachable code");
            check_stmt(c, s); /* still type-check it, just don't re-flag reachability */
            continue;
        }
        check_stmt(c, s);
        if (stmt_diverges(s)) unreachable = 1;
    }
}

static void check_var_decl(Checker *c, Stmt *s) {
    Type *decl_t = &s->as.var_decl.type;
    if (!is_known_type_name(c, decl_t->name))
        sema_error(c, s->line, s->col, "unknown type '%s'", decl_t->name);

    if (s->as.var_decl.init) {
        Type init_t = infer_expr(c, s->as.var_decl.init);
        if (!is_error(&init_t) && !assignable(decl_t, &init_t)) {
            char db[64], ib[64]; type_to_str(decl_t, db, sizeof(db)); type_to_str(&init_t, ib, sizeof(ib));
            sema_error(c, s->as.var_decl.init->line, s->as.var_decl.init->col,
                       "cannot initialize '%s' with '%s'", db, ib);
        }
    }

    if (c->scope) {
        if (!scope_declare(c, s->as.var_decl.name, decl_t, 0, s->line, s->col))
            sema_error(c, s->line, s->col, "redeclaration of '%s' in this scope", s->as.var_decl.name);
    }
}

static void check_stmt(Checker *c, Stmt *s) {
    switch (s->kind) {
        case STMT_EXPR:
            infer_expr(c, s->as.expr_stmt.expr);
            break;
        case STMT_VAR_DECL:
            check_var_decl(c, s);
            break;
        case STMT_BLOCK:
            scope_push(c);
            check_block_contents(c, s);
            scope_pop(c);
            break;
        case STMT_IF:
            check_condition(c, s->as.if_stmt.cond);
            check_stmt(c, s->as.if_stmt.then_branch);
            if (s->as.if_stmt.else_branch) check_stmt(c, s->as.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            check_condition(c, s->as.while_stmt.cond);
            c->loop_depth++;
            check_stmt(c, s->as.while_stmt.body);
            c->loop_depth--;
            break;
        case STMT_FOR:
            scope_push(c);
            if (s->as.for_stmt.init) check_stmt(c, s->as.for_stmt.init);
            if (s->as.for_stmt.cond) check_condition(c, s->as.for_stmt.cond);
            if (s->as.for_stmt.post) infer_expr(c, s->as.for_stmt.post);
            c->loop_depth++;
            check_stmt(c, s->as.for_stmt.body);
            c->loop_depth--;
            scope_pop(c);
            break;
        case STMT_BREAK:
            if (c->loop_depth == 0) sema_error(c, s->line, s->col, "'break' used outside of a loop");
            break;
        case STMT_CONTINUE:
            if (c->loop_depth == 0) sema_error(c, s->line, s->col, "'continue' used outside of a loop");
            break;
        case STMT_YIELD:
            if (c->has_ret_type) {
                if (!s->as.yield_stmt.value) {
                    char rb[64]; type_to_str(c->ret_type, rb, sizeof(rb));
                    sema_error(c, s->line, s->col, "function must yield a value of type '%s'", rb);
                } else {
                    Type vt = infer_expr(c, s->as.yield_stmt.value);
                    if (!is_error(&vt) && !assignable(c->ret_type, &vt)) {
                        char rb[64], vb[64]; type_to_str(c->ret_type, rb, sizeof(rb)); type_to_str(&vt, vb, sizeof(vb));
                        sema_error(c, s->as.yield_stmt.value->line, s->as.yield_stmt.value->col,
                                   "yielded '%s' but function returns '%s'", vb, rb);
                    }
                }
            } else if (s->as.yield_stmt.value) {
                infer_expr(c, s->as.yield_stmt.value);
                sema_error(c, s->line, s->col, "function has no return type but 'yield' has a value");
            }
            break;
    }
}

/* ---- declaration-level registration & checking -------------------------- */

int sema_check_program(Program *prog) {
    Checker c = {0};
    size_t func_cap = 0, struct_cap = 0, global_cap = 0;

    /* Pass 1: register every top-level name so forward references
     * (a function calling one declared later, a struct pointing at
     * itself or at a struct declared afterward) all resolve. */
    for (size_t i = 0; i < prog->count; i++) {
        Decl *d = &prog->decls[i];
        const char *name = d->kind == DECL_FUNC ? d->as.func.name
                          : d->kind == DECL_STRUCT ? d->as.struct_decl.name
                          : d->as.var->as.var_decl.name;
        if (is_reserved_builtin_name(name))
            sema_error(&c, 0, 0, "'%s' is a reserved builtin name and can't be redeclared", name);
        if (is_toplevel_name_taken(&c, name))
            sema_error(&c, 0, 0, "redeclaration of '%s' at top level", name);

        switch (d->kind) {
            case DECL_FUNC: {
                FuncSig fs = {
                    d->as.func.name,
                    d->as.func.has_return_type ? &d->as.func.return_type : NULL,
                    d->as.func.has_return_type,
                    d->as.func.params,
                    d->as.func.param_count,
                    0
                };
                DA_PUSH(c.funcs, c.func_count, func_cap, fs);
                break;
            }
            case DECL_STRUCT: {
                StructSig ss = { d->as.struct_decl.name, d->as.struct_decl.fields, d->as.struct_decl.field_count };
                DA_PUSH(c.structs, c.struct_count, struct_cap, ss);
                break;
            }
            case DECL_VAR: {
                GlobalSig gs = { d->as.var->as.var_decl.name, &d->as.var->as.var_decl.type };
                DA_PUSH(c.globals, c.global_count, global_cap, gs);
                break;
            }
        }
    }

    /* Pass 2: check struct field types, function signatures, global
     * initializers, and every function body. */
    for (size_t i = 0; i < prog->count; i++) {
        Decl *d = &prog->decls[i];
        if (d->kind == DECL_STRUCT) {
            StructDecl *sd = &d->as.struct_decl;
            for (size_t f = 0; f < sd->field_count; f++)
                if (!is_known_type_name(&c, sd->fields[f].type.name))
                    sema_error(&c, 0, 0, "struct '%s' field '%s' has unknown type '%s'",
                               sd->name, sd->fields[f].name, sd->fields[f].type.name);
        } else if (d->kind == DECL_VAR) {
            check_var_decl(&c, d->as.var);
        } else { /* DECL_FUNC */
            FuncDecl *f = &d->as.func;
            if (f->has_return_type && !is_known_type_name(&c, f->return_type.name))
                sema_error(&c, 0, 0, "function '%s' has unknown return type '%s'", f->name, f->return_type.name);

            if (f->is_extern) {
                for (size_t p = 0; p < f->param_count; p++) {
                    if (!is_known_type_name(&c, f->params[p].type.name))
                        sema_error(&c, 0, 0, "function '%s' parameter '%s' has unknown type '%s'",
                                   f->name, f->params[p].name, f->params[p].type.name);
                }
                /* No body to check: no locals, no control-flow analysis,
                 * no missing-yield check — there's nothing here for LazyC
                 * to have gotten wrong. The real implementation lives
                 * outside this program entirely. */
                continue;
            }

            scope_push(&c);
            for (size_t p = 0; p < f->param_count; p++) {
                if (!is_known_type_name(&c, f->params[p].type.name))
                    sema_error(&c, 0, 0, "function '%s' parameter '%s' has unknown type '%s'",
                               f->name, f->params[p].name, f->params[p].type.name);
                if (!scope_declare(&c, f->params[p].name, &f->params[p].type, 1, f->body->line, f->body->col))
                    sema_error(&c, 0, 0, "function '%s' has duplicate parameter '%s'", f->name, f->params[p].name);
            }
            c.ret_type = f->has_return_type ? &f->return_type : NULL;
            c.has_ret_type = f->has_return_type;
            c.loop_depth = 0;
            check_block_contents(&c, f->body);
            if (f->has_return_type && !guarantees_yield(f->body)) {
                sema_error(&c, f->body->line, f->body->col,
                           "function '%s' does not yield a value on all control paths", f->name);
            }
            scope_pop(&c);
        }
    }

    /* Unused-function scan. 'main' is the implicit entry point and extern
     * declarations are exempt too — declaring an FFI signature the rest
     * of the program doesn't happen to call yet isn't dead code, it's
     * telling the compiler about something that exists outside this
     * program. Known simplification: a function that only ever calls
     * itself (dead recursion, never invoked from anywhere reachable)
     * still counts as "called" here — proper reachability analysis from
     * the entry point is more machinery than this warrants right now. */
    for (size_t i = 0; i < c.func_count; i++) {
        int is_ext = 0;
        for (size_t d = 0; d < prog->count; d++)
            if (prog->decls[d].kind == DECL_FUNC && strcmp(prog->decls[d].as.func.name, c.funcs[i].name) == 0) {
                is_ext = prog->decls[d].as.func.is_extern;
                break;
            }
        if (!c.funcs[i].called && !is_ext && strcmp(c.funcs[i].name, "main") != 0)
            sema_error(&c, 0, 0, "function '%s' is never called", c.funcs[i].name);
    }

    int had_error = c.had_error;
    while (c.scope) scope_pop(&c);
    free(c.funcs);
    free(c.structs);
    free(c.globals);
    return had_error;
}
