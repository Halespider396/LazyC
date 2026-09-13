#include "compile.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================================
 * Struct layouts — every struct's fields are laid out as consecutive Value
 * "slots" (the VM's unit of storage; see vm.h). A field's slot size is 1 for
 * anything scalar/pointer/array-marked, or recursively the nested struct's
 * total slot count for a by-value struct field.
 * ========================================================================= */

typedef struct { char *name; Type type; int32_t offset; int32_t size; } FieldLayout;
typedef struct { char *name; FieldLayout *fields; size_t field_count, field_cap; int32_t total_slots; int computing; } StructLayout;
typedef struct { char *name; const Type *type; int32_t slot; } GlobalVar;
typedef struct { char *name; const Type *type; int32_t slot; } NameSlot;

typedef struct LocalScope {
    NameSlot *entries; size_t count, cap;
    struct LocalScope *parent;
} LocalScope;

typedef struct LoopCtx {
    size_t *break_ips; size_t break_count, break_cap;
    int32_t continue_target;              /* -1 if not yet known (for-loop post-expr) */
    size_t *continue_patch_ips; size_t continue_patch_count, continue_patch_cap;
    struct LoopCtx *parent;
} LoopCtx;

typedef struct {
    char *name;
    int32_t index;
    int has_return_type;
    Type return_type;
    int is_extern;
} FuncSigEntry;

typedef struct {
    CompiledProgram *out;
    const Program *ast;

    StructLayout *structs; size_t struct_count, struct_cap;
    GlobalVar *globals; size_t global_count, global_cap;
    FuncSigEntry *funcsig; size_t funcsig_count, funcsig_cap;

    LocalScope *scope;
    LoopCtx *loop;
    int32_t next_slot;

    int had_error;
} Compiler;

#define DA_PUSH(arr, count, cap, item) \
    do { \
        if ((count) >= (cap)) { \
            (cap) = (cap) == 0 ? 4 : (cap) * 2; \
            (arr) = realloc((arr), (cap) * sizeof(*(arr))); \
        } \
        (arr)[(count)++] = (item); \
    } while (0)

static void cerror(Compiler *c, const char *fmt, ...) {
    c->had_error = 1;
    fprintf(stderr, "codegen error: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static Type prim(const char *name) { Type t; t.name = (char *)name; t.ptr_depth = 0; t.is_array = 0; return t; }

/* The parser stores string-literal tokens verbatim, quotes and all
 * (that's exactly right for the C transpiler, since LazyC's string syntax
 * IS C's). The VM's string pool needs the actual runtime content instead
 * — quotes stripped, escapes decoded — since that's what print() etc.
 * should output. */
static char *decode_string_literal(const char *raw) {
    size_t len = strlen(raw);
    char *buf = malloc(len + 1);
    size_t bi = 0;
    size_t end = len > 0 ? len - 1 : 0; /* index of the closing quote */
    for (size_t i = 1; i < end; i++) {
        char ch = raw[i];
        if (ch == '\\' && i + 1 < end) {
            char next = raw[++i];
            switch (next) {
                case 'n': buf[bi++] = '\n'; break;
                case 't': buf[bi++] = '\t'; break;
                case 'r': buf[bi++] = '\r'; break;
                case '0': buf[bi++] = '\0'; break;
                case '\\': buf[bi++] = '\\'; break;
                case '"': buf[bi++] = '"'; break;
                case '\'': buf[bi++] = '\''; break;
                default: buf[bi++] = next; break; /* unrecognized escape: keep the literal char */
            }
        } else {
            buf[bi++] = ch;
        }
    }
    buf[bi] = '\0';
    return buf;
}

static const StructLayout *find_layout(const Compiler *c, const char *name) {
    for (size_t i = 0; i < c->struct_count; i++)
        if (strcmp(c->structs[i].name, name) == 0) return &c->structs[i];
    return NULL;
}
static int is_bare_struct_type(const Compiler *c, const Type *t) {
    return t->ptr_depth == 0 && !t->is_array && find_layout(c, t->name) != NULL;
}
static const FieldLayout *find_field(const StructLayout *sl, const char *name) {
    for (size_t i = 0; i < sl->field_count; i++)
        if (strcmp(sl->fields[i].name, name) == 0) return &sl->fields[i];
    return NULL;
}

static int32_t slot_size_of(Compiler *c, const Type *t) {
    if (t->ptr_depth > 0 || t->is_array) return 1;
    StructLayout *sl = NULL;
    for (size_t i = 0; i < c->struct_count; i++)
        if (strcmp(c->structs[i].name, t->name) == 0) { sl = &c->structs[i]; break; }
    if (!sl) return 1; /* a primitive */
    if (sl->computing) return 1; /* self-referential by-value struct: degrade gracefully, see header */
    return sl->total_slots;
}

static void build_struct_layouts(Compiler *c) {
    for (size_t i = 0; i < c->ast->count; i++) {
        if (c->ast->decls[i].kind != DECL_STRUCT) continue;
        StructLayout sl = {0};
        sl.name = c->ast->decls[i].as.struct_decl.name;
        DA_PUSH(c->structs, c->struct_count, c->struct_cap, sl);
    }
    for (size_t i = 0; i < c->ast->count; i++) {
        if (c->ast->decls[i].kind != DECL_STRUCT) continue;
        const StructDecl *sd = &c->ast->decls[i].as.struct_decl;
        StructLayout *sl = NULL;
        for (size_t k = 0; k < c->struct_count; k++)
            if (strcmp(c->structs[k].name, sd->name) == 0) { sl = &c->structs[k]; break; }
        sl->computing = 1;
        int32_t offset = 0;
        for (size_t f = 0; f < sd->field_count; f++) {
            int32_t size = slot_size_of(c, &sd->fields[f].type);
            FieldLayout fl = { sd->fields[f].name, sd->fields[f].type, offset, size };
            DA_PUSH(sl->fields, sl->field_count, sl->field_cap, fl);
            offset += size;
        }
        sl->total_slots = offset;
        sl->computing = 0;
    }
}

/* ---- scopes --------------------------------------------------------------- */

static void scope_push(Compiler *c) {
    LocalScope *s = calloc(1, sizeof(LocalScope));
    s->parent = c->scope;
    c->scope = s;
}
static void scope_pop(Compiler *c) {
    LocalScope *s = c->scope;
    c->scope = s->parent;
    free(s->entries);
    free(s);
}
static void scope_declare(Compiler *c, const char *name, const Type *type, int32_t slot) {
    NameSlot n = { (char *)name, type, slot };
    DA_PUSH(c->scope->entries, c->scope->count, c->scope->cap, n);
}
static int resolve_name(Compiler *c, const char *name, int32_t *slot, const Type **type, int *is_global) {
    for (LocalScope *s = c->scope; s; s = s->parent) {
        for (size_t i = 0; i < s->count; i++) {
            if (strcmp(s->entries[i].name, name) == 0) {
                *slot = s->entries[i].slot; *type = s->entries[i].type; *is_global = 0;
                return 1;
            }
        }
    }
    for (size_t i = 0; i < c->global_count; i++) {
        if (strcmp(c->globals[i].name, name) == 0) {
            *slot = c->globals[i].slot; *type = c->globals[i].type; *is_global = 1;
            return 1;
        }
    }
    return 0;
}
static int32_t declare_local(Compiler *c, const char *name, const Type *type) {
    int32_t slot = c->next_slot;
    c->next_slot += slot_size_of(c, type);
    scope_declare(c, name, type, slot);
    return slot;
}

/* ---- minimal type-shape inference ------------------------------------------
 * Just enough to pick the right instructions (which struct a `.`/`->`
 * targets, whether an identifier is a bare struct, whether a `+`/`-`
 * operand is a pointer). sema.c already guarantees the program is well
 * typed, so this never needs to *report* anything — only recover shape. */

static Type infer_shape(Compiler *c, const Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT: return prim("int");
        case EXPR_FLOAT_LIT: return prim("float");
        case EXPR_STRING_LIT: return prim("string");
        case EXPR_CHAR_LIT: return prim("char");
        case EXPR_BOOL_LIT: return prim("bool");
        case EXPR_NULL_LIT: return prim("<null>");
        case EXPR_IDENT: {
            int32_t slot; const Type *t; int is_global;
            if (resolve_name(c, e->as.ident, &slot, &t, &is_global)) return *t;
            return prim("<error>");
        }
        case EXPR_UNARY: {
            Type inner = infer_shape(c, e->as.unary.operand);
            if (e->as.unary.op == TOK_AMP) { inner.ptr_depth++; return inner; }
            if (e->as.unary.op == TOK_STAR && inner.ptr_depth > 0) { inner.ptr_depth--; return inner; }
            return inner;
        }
        case EXPR_POSTFIX: return infer_shape(c, e->as.postfix.operand);
        case EXPR_BINARY: {
            switch (e->as.binary.op) {
                case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE:
                case TOK_EQ: case TOK_NEQ: case TOK_AND: case TOK_OR:
                    return prim("bool");
                default: return infer_shape(c, e->as.binary.left);
            }
        }
        case EXPR_ASSIGN: return infer_shape(c, e->as.assign.target);
        case EXPR_CALL:
            if (e->as.call.callee->kind == EXPR_IDENT) {
                const char *name = e->as.call.callee->as.ident;
                if (strcmp(name, "strlen") == 0) return prim("int");
                if (strcmp(name, "streq") == 0) return prim("bool");
                if (strcmp(name, "abs") == 0 || strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
                    if (e->as.call.arg_count > 0) {
                        Type t0 = infer_shape(c, e->as.call.args[0]);
                        if (t0.ptr_depth == 0 && strcmp(t0.name, "float") == 0) return prim("float");
                    }
                    return prim("int");
                }
                for (size_t i = 0; i < c->funcsig_count; i++)
                    if (strcmp(c->funcsig[i].name, name) == 0)
                        return c->funcsig[i].has_return_type ? c->funcsig[i].return_type : prim("void");
            }
            return prim("<error>");
        case EXPR_INDEX: {
            Type arr = infer_shape(c, e->as.index.array);
            if (arr.is_array) arr.is_array = 0;
            else if (arr.ptr_depth > 0) arr.ptr_depth--;
            return arr;
        }
        case EXPR_MEMBER: {
            Type obj = infer_shape(c, e->as.member.object);
            const StructLayout *sl = find_layout(c, obj.name);
            const FieldLayout *fl = sl ? find_field(sl, e->as.member.field) : NULL;
            return fl ? fl->type : prim("<error>");
        }
    }
    return prim("<error>");
}

/* ============================================================================
 * Expression compilation.
 *
 * compile_expr_value(e): emits code leaving exactly one Value on the VM
 * stack — e's value. Every call site that uses this as a *statement*
 * (STMT_EXPR, a for-loop's post-expr) must follow it with OP_POP to keep
 * the stack balanced, since this always pushes exactly one value.
 *
 * compile_lvalue_addr(e): emits code leaving the *address* (a VAL_PTR) of
 * the storage e refers to — used for `&e`, and as the base of a
 * `.field`/`->field` access when the base isn't already a pointer.
 * ========================================================================= */

static void compile_expr_value(Compiler *c, const Expr *e);
static void compile_lvalue_addr(Compiler *c, const Expr *e);
static void compile_stmt(Compiler *c, const Stmt *s);

static void field_base_and_offset(Compiler *c, const Expr *object, int arrow, const char *field, int32_t *out_offset) {
    Type obj_t = infer_shape(c, object);
    const StructLayout *sl = find_layout(c, obj_t.name);
    const FieldLayout *fl = sl ? find_field(sl, field) : NULL;
    *out_offset = fl ? fl->offset : 0;
    if (arrow) compile_expr_value(c, object);   /* object is already a pointer */
    else compile_lvalue_addr(c, object);        /* need the address of the struct itself */
}

static void compile_lvalue_addr(Compiler *c, const Expr *e) {
    switch (e->kind) {
        case EXPR_IDENT: {
            int32_t slot; const Type *t; int is_global;
            if (!resolve_name(c, e->as.ident, &slot, &t, &is_global)) {
                cerror(c, "internal: unresolved identifier '%s'", e->as.ident);
                chunk_emit_op(&c->out->code, OP_CONST_NULL);
                return;
            }
            chunk_emit_op(&c->out->code, is_global ? OP_ADDR_GLOBAL : OP_ADDR_LOCAL);
            chunk_emit_i32(&c->out->code, slot);
            break;
        }
        case EXPR_UNARY: /* &(*p) — the address is just p's own value */
            if (e->as.unary.op == TOK_STAR) { compile_expr_value(c, e->as.unary.operand); return; }
            cerror(c, "internal: invalid lvalue (unary)");
            break;
        case EXPR_INDEX:
            compile_expr_value(c, e->as.index.array);
            compile_expr_value(c, e->as.index.index);
            chunk_emit_op(&c->out->code, OP_INDEX_ADDR);
            break;
        case EXPR_MEMBER: {
            int32_t offset;
            field_base_and_offset(c, e->as.member.object, e->as.member.arrow, e->as.member.field, &offset);
            chunk_emit_op(&c->out->code, OP_FIELD_ADDR);
            chunk_emit_i32(&c->out->code, offset);
            break;
        }
        default:
            cerror(c, "internal: expression is not a valid assignment target");
            break;
    }
}

/* Assumes the value to store is already on top of the stack (with a
 * second copy of it just beneath, left behind as the expression's
 * result). Emits whatever address computation the target needs, then
 * the matching *_STORE op, which pops the address parts and the value
 * copy — net effect: pops exactly one item (the copy) from the stack.
 *
 * Known limitation: for EXPR_INDEX/EXPR_MEMBER targets used as compound-
 * assignment or ++/-- operands, the base/index sub-expressions are
 * compiled twice (once to read the current value, once here to compute
 * the store address). Harmless for side-effect-free subexpressions
 * (the overwhelming common case — a plain variable index), but would
 * double a side effect in something like `arr[f()] += 1`. */
static void compile_store(Compiler *c, const Expr *target) {
    switch (target->kind) {
        case EXPR_IDENT: {
            int32_t slot; const Type *t; int is_global;
            if (!resolve_name(c, target->as.ident, &slot, &t, &is_global)) {
                cerror(c, "internal: unresolved identifier '%s'", target->as.ident);
                break;
            }
            chunk_emit_op(&c->out->code, is_global ? OP_STORE_GLOBAL : OP_STORE_LOCAL);
            chunk_emit_i32(&c->out->code, slot);
            break;
        }
        case EXPR_UNARY: /* *p = ... */
            compile_expr_value(c, target->as.unary.operand);
            chunk_emit_op(&c->out->code, OP_DEREF_STORE);
            break;
        case EXPR_INDEX:
            compile_expr_value(c, target->as.index.array);
            compile_expr_value(c, target->as.index.index);
            chunk_emit_op(&c->out->code, OP_INDEX_STORE);
            break;
        case EXPR_MEMBER: {
            int32_t offset;
            field_base_and_offset(c, target->as.member.object, target->as.member.arrow, target->as.member.field, &offset);
            chunk_emit_op(&c->out->code, OP_FIELD_STORE);
            chunk_emit_i32(&c->out->code, offset);
            break;
        }
        default:
            cerror(c, "internal: expression is not a valid assignment target");
            break;
    }
}

static void compile_print_call(Compiler *c, const Expr *e) {
    for (size_t i = 0; i < e->as.call.arg_count; i++) compile_expr_value(c, e->as.call.args[i]);
    chunk_emit_op(&c->out->code, OP_PRINT);
    chunk_emit_i32(&c->out->code, (int32_t)e->as.call.arg_count);
    chunk_emit_op(&c->out->code, OP_CONST_NULL); /* print() is void; keep the "always leaves one value" contract */
}

static int compile_builtin_call(Compiler *c, const Expr *e, const char *name) {
    if (strcmp(name, "print") == 0) { compile_print_call(c, e); return 1; }
    if (strcmp(name, "strlen") == 0) {
        compile_expr_value(c, e->as.call.args[0]);
        chunk_emit_op(&c->out->code, OP_STRLEN);
        return 1;
    }
    if (strcmp(name, "streq") == 0) {
        compile_expr_value(c, e->as.call.args[0]);
        compile_expr_value(c, e->as.call.args[1]);
        chunk_emit_op(&c->out->code, OP_STREQ);
        return 1;
    }
    if (strcmp(name, "abs") == 0) {
        compile_expr_value(c, e->as.call.args[0]);
        chunk_emit_op(&c->out->code, OP_ABS);
        return 1;
    }
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        compile_expr_value(c, e->as.call.args[0]);
        compile_expr_value(c, e->as.call.args[1]);
        chunk_emit_op(&c->out->code, strcmp(name, "min") == 0 ? OP_MIN : OP_MAX);
        return 1;
    }
    return 0;
}

static void compile_call(Compiler *c, const Expr *e) {
    const char *name = e->as.call.callee->as.ident;
    if (compile_builtin_call(c, e, name)) return;

    int32_t func_index = -1; int has_ret = 0; Type ret_type = prim("void"); int is_extern = 0;
    for (size_t i = 0; i < c->funcsig_count; i++) {
        if (strcmp(c->funcsig[i].name, name) == 0) {
            func_index = c->funcsig[i].index; has_ret = c->funcsig[i].has_return_type;
            ret_type = c->funcsig[i].return_type; is_extern = c->funcsig[i].is_extern;
            break;
        }
    }
    if (func_index < 0) { cerror(c, "internal: call to unknown function '%s'", name); return; }
    if (is_extern) {
        cerror(c, "cannot call extern function '%s' from the VM backend (--run) — extern/FFI calls are only "
                  "supported when compiling to C via --emit-c, since that's what actually links against the "
                  "real implementation", name);
        chunk_emit_op(&c->out->code, OP_CONST_NULL); /* keep the "always leaves one value" contract */
        return;
    }
    if (has_ret && is_bare_struct_type(c, &ret_type)) {
        cerror(c, "function '%s' returns struct '%s' by value, which the VM backend doesn't support yet "
                  "— return a pointer (*%s) instead", name, ret_type.name, ret_type.name);
    }
    for (size_t i = 0; i < e->as.call.arg_count; i++) compile_expr_value(c, e->as.call.args[i]);
    chunk_emit_op(&c->out->code, OP_CALL);
    chunk_emit_i32(&c->out->code, func_index);
    chunk_emit_i32(&c->out->code, (int32_t)e->as.call.arg_count);
}

static void compile_incdec(Compiler *c, const Expr *operand, int is_inc, int is_prefix) {
    if (is_prefix) {
        compile_expr_value(c, operand);
        chunk_emit_op(&c->out->code, OP_CONST_INT); chunk_emit_i64(&c->out->code, 1);
        chunk_emit_op(&c->out->code, is_inc ? OP_ADD : OP_SUB);
        chunk_emit_op(&c->out->code, OP_DUP);
        compile_store(c, operand); /* pops the copy, stores; leaves [new] */
    } else {
        compile_expr_value(c, operand);          /* [old] */
        chunk_emit_op(&c->out->code, OP_DUP);     /* [old, old] */
        chunk_emit_op(&c->out->code, OP_CONST_INT); chunk_emit_i64(&c->out->code, 1);
        chunk_emit_op(&c->out->code, is_inc ? OP_ADD : OP_SUB); /* [old, new] */
        chunk_emit_op(&c->out->code, OP_DUP);     /* [old, new, new] */
        compile_store(c, operand);                /* pops copy, stores; [old, new] */
        chunk_emit_op(&c->out->code, OP_POP);      /* [old] */
    }
}

static void compile_expr_value(Compiler *c, const Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT:
            chunk_emit_op(&c->out->code, OP_CONST_INT);
            chunk_emit_i64(&c->out->code, e->as.int_val);
            return;
        case EXPR_FLOAT_LIT:
            chunk_emit_op(&c->out->code, OP_CONST_FLOAT);
            chunk_emit_f64(&c->out->code, e->as.float_val);
            return;
        case EXPR_STRING_LIT: {
            char *decoded = decode_string_literal(e->as.str_lit.value);
            int32_t idx = chunk_add_string(&c->out->code, decoded);
            free(decoded);
            chunk_emit_op(&c->out->code, OP_CONST_STRING);
            chunk_emit_i32(&c->out->code, idx);
            return;
        }
        case EXPR_CHAR_LIT:
            chunk_emit_op(&c->out->code, OP_CONST_CHAR);
            chunk_emit_i64(&c->out->code, (int64_t)(unsigned char)e->as.char_val);
            return;
        case EXPR_BOOL_LIT:
            chunk_emit_op(&c->out->code, e->as.bool_val ? OP_CONST_TRUE : OP_CONST_FALSE);
            return;
        case EXPR_NULL_LIT:
            chunk_emit_op(&c->out->code, OP_CONST_NULL);
            return;

        case EXPR_IDENT: {
            int32_t slot; const Type *t; int is_global;
            if (!resolve_name(c, e->as.ident, &slot, &t, &is_global)) {
                cerror(c, "internal: unresolved identifier '%s'", e->as.ident);
                chunk_emit_op(&c->out->code, OP_CONST_NULL);
                return;
            }
            if (is_bare_struct_type(c, t)) {
                cerror(c, "cannot use struct variable '%s' as a plain value — the VM backend doesn't support "
                          "copying structs by value yet; access its fields, or use a pointer", e->as.ident);
                chunk_emit_op(&c->out->code, OP_CONST_NULL);
                return;
            }
            chunk_emit_op(&c->out->code, is_global ? OP_LOAD_GLOBAL : OP_LOAD_LOCAL);
            chunk_emit_i32(&c->out->code, slot);
            return;
        }

        case EXPR_UNARY: {
            int op = e->as.unary.op;
            if (op == TOK_AMP) { compile_lvalue_addr(c, e->as.unary.operand); return; }
            if (op == TOK_STAR) {
                compile_expr_value(c, e->as.unary.operand);
                chunk_emit_op(&c->out->code, OP_DEREF_LOAD);
                return;
            }
            if (op == TOK_INC || op == TOK_DEC) { compile_incdec(c, e->as.unary.operand, op == TOK_INC, 1); return; }
            compile_expr_value(c, e->as.unary.operand);
            if (op == TOK_MINUS) chunk_emit_op(&c->out->code, OP_NEG);
            else if (op == TOK_NOT) chunk_emit_op(&c->out->code, OP_NOT);
            else if (op == TOK_BITNOT) chunk_emit_op(&c->out->code, OP_BIT_NOT);
            return;
        }

        case EXPR_POSTFIX:
            compile_incdec(c, e->as.postfix.operand, e->as.postfix.op == TOK_INC, 0);
            return;

        case EXPR_BINARY: {
            int op = e->as.binary.op;
            if (op == TOK_AND) {
                compile_expr_value(c, e->as.binary.left);
                size_t jf = chunk_emit_op(&c->out->code, OP_JUMP_IF_FALSE);
                chunk_emit_i32(&c->out->code, 0);
                compile_expr_value(c, e->as.binary.right);
                size_t je = chunk_emit_op(&c->out->code, OP_JUMP);
                chunk_emit_i32(&c->out->code, 0);
                chunk_patch_i32(&c->out->code, jf + 1, (int32_t)c->out->code.count);
                chunk_emit_op(&c->out->code, OP_CONST_FALSE);
                chunk_patch_i32(&c->out->code, je + 1, (int32_t)c->out->code.count);
                return;
            }
            if (op == TOK_OR) {
                compile_expr_value(c, e->as.binary.left);
                size_t jt = chunk_emit_op(&c->out->code, OP_JUMP_IF_TRUE);
                chunk_emit_i32(&c->out->code, 0);
                compile_expr_value(c, e->as.binary.right);
                size_t je = chunk_emit_op(&c->out->code, OP_JUMP);
                chunk_emit_i32(&c->out->code, 0);
                chunk_patch_i32(&c->out->code, jt + 1, (int32_t)c->out->code.count);
                chunk_emit_op(&c->out->code, OP_CONST_TRUE);
                chunk_patch_i32(&c->out->code, je + 1, (int32_t)c->out->code.count);
                return;
            }

            Type lt = infer_shape(c, e->as.binary.left);
            compile_expr_value(c, e->as.binary.left);
            compile_expr_value(c, e->as.binary.right);
            if (op == TOK_MINUS && lt.ptr_depth > 0) chunk_emit_op(&c->out->code, OP_NEG); /* negate rhs int first */

            switch (op) {
                case TOK_PLUS: chunk_emit_op(&c->out->code, lt.ptr_depth > 0 ? OP_PTR_ADD_INT : OP_ADD); break;
                case TOK_MINUS: chunk_emit_op(&c->out->code, lt.ptr_depth > 0 ? OP_PTR_ADD_INT : OP_SUB); break;
                case TOK_STAR: chunk_emit_op(&c->out->code, OP_MUL); break;
                case TOK_SLASH: chunk_emit_op(&c->out->code, OP_DIV); break;
                case TOK_PERCENT: chunk_emit_op(&c->out->code, OP_MOD); break;
                case TOK_AMP: chunk_emit_op(&c->out->code, OP_BIT_AND); break;
                case TOK_BITOR: chunk_emit_op(&c->out->code, OP_BIT_OR); break;
                case TOK_BITXOR: chunk_emit_op(&c->out->code, OP_BIT_XOR); break;
                case TOK_SHL: chunk_emit_op(&c->out->code, OP_SHL); break;
                case TOK_SHR: chunk_emit_op(&c->out->code, OP_SHR); break;
                case TOK_LT: chunk_emit_op(&c->out->code, OP_LT); break;
                case TOK_GT: chunk_emit_op(&c->out->code, OP_GT); break;
                case TOK_LE: chunk_emit_op(&c->out->code, OP_LE); break;
                case TOK_GE: chunk_emit_op(&c->out->code, OP_GE); break;
                case TOK_EQ: chunk_emit_op(&c->out->code, OP_EQ); break;
                case TOK_NEQ: chunk_emit_op(&c->out->code, OP_NEQ); break;
                default: cerror(c, "internal: unhandled binary operator"); break;
            }
            return;
        }

        case EXPR_ASSIGN: {
            if (e->as.assign.op == TOK_ASSIGN) {
                compile_expr_value(c, e->as.assign.value);
                chunk_emit_op(&c->out->code, OP_DUP);
                compile_store(c, e->as.assign.target);
            } else {
                compile_expr_value(c, e->as.assign.target);
                compile_expr_value(c, e->as.assign.value);
                switch (e->as.assign.op) {
                    case TOK_PLUS_EQ: chunk_emit_op(&c->out->code, OP_ADD); break;
                    case TOK_MINUS_EQ: chunk_emit_op(&c->out->code, OP_SUB); break;
                    case TOK_STAR_EQ: chunk_emit_op(&c->out->code, OP_MUL); break;
                    case TOK_SLASH_EQ: chunk_emit_op(&c->out->code, OP_DIV); break;
                    default: break;
                }
                chunk_emit_op(&c->out->code, OP_DUP);
                compile_store(c, e->as.assign.target);
            }
            return;
        }

        case EXPR_CALL:
            compile_call(c, e);
            return;

        case EXPR_INDEX:
            compile_expr_value(c, e->as.index.array);
            compile_expr_value(c, e->as.index.index);
            chunk_emit_op(&c->out->code, OP_INDEX_LOAD);
            return;

        case EXPR_MEMBER: {
            int32_t offset;
            field_base_and_offset(c, e->as.member.object, e->as.member.arrow, e->as.member.field, &offset);
            chunk_emit_op(&c->out->code, OP_FIELD_LOAD);
            chunk_emit_i32(&c->out->code, offset);
            return;
        }
    }
}

/* ============================================================================
 * Statement compilation.
 * ========================================================================= */

static void loop_push(Compiler *c, int32_t continue_target) {
    LoopCtx *lc = calloc(1, sizeof(LoopCtx));
    lc->continue_target = continue_target;
    lc->parent = c->loop;
    c->loop = lc;
}
static void loop_patch_deferred_continues(Compiler *c, int32_t target) {
    LoopCtx *lc = c->loop;
    for (size_t i = 0; i < lc->continue_patch_count; i++)
        chunk_patch_i32(&c->out->code, lc->continue_patch_ips[i] + 1, target);
    lc->continue_patch_count = 0;
}
static void loop_pop_and_patch_breaks(Compiler *c, int32_t after_ip) {
    LoopCtx *lc = c->loop;
    for (size_t i = 0; i < lc->break_count; i++)
        chunk_patch_i32(&c->out->code, lc->break_ips[i] + 1, after_ip);
    c->loop = lc->parent;
    free(lc->break_ips);
    free(lc->continue_patch_ips);
    free(lc);
}

static void compile_block(Compiler *c, const Stmt *block) {
    scope_push(c);
    for (size_t i = 0; i < block->as.block.count; i++)
        compile_stmt(c, block->as.block.stmts[i]);
    scope_pop(c);
}

static void compile_stmt(Compiler *c, const Stmt *s) {
    switch (s->kind) {
        case STMT_EXPR:
            compile_expr_value(c, s->as.expr_stmt.expr);
            chunk_emit_op(&c->out->code, OP_POP);
            break;

        case STMT_VAR_DECL: {
            int32_t slot = declare_local(c, s->as.var_decl.name, &s->as.var_decl.type);
            if (s->as.var_decl.init) {
                compile_expr_value(c, s->as.var_decl.init);
                chunk_emit_op(&c->out->code, OP_STORE_LOCAL);
                chunk_emit_i32(&c->out->code, slot);
            }
            break;
        }

        case STMT_BLOCK:
            compile_block(c, s);
            break;

        case STMT_IF: {
            compile_expr_value(c, s->as.if_stmt.cond);
            size_t jf = chunk_emit_op(&c->out->code, OP_JUMP_IF_FALSE);
            chunk_emit_i32(&c->out->code, 0);
            compile_stmt(c, s->as.if_stmt.then_branch);
            if (s->as.if_stmt.else_branch) {
                size_t je = chunk_emit_op(&c->out->code, OP_JUMP);
                chunk_emit_i32(&c->out->code, 0);
                chunk_patch_i32(&c->out->code, jf + 1, (int32_t)c->out->code.count);
                compile_stmt(c, s->as.if_stmt.else_branch);
                chunk_patch_i32(&c->out->code, je + 1, (int32_t)c->out->code.count);
            } else {
                chunk_patch_i32(&c->out->code, jf + 1, (int32_t)c->out->code.count);
            }
            break;
        }

        case STMT_WHILE: {
            int32_t cond_ip = (int32_t)c->out->code.count;
            loop_push(c, cond_ip);
            compile_expr_value(c, s->as.while_stmt.cond);
            size_t jf = chunk_emit_op(&c->out->code, OP_JUMP_IF_FALSE);
            chunk_emit_i32(&c->out->code, 0);
            compile_stmt(c, s->as.while_stmt.body);
            size_t jb = chunk_emit_op(&c->out->code, OP_JUMP);
            chunk_emit_i32(&c->out->code, cond_ip);
            (void)jb;
            int32_t after_ip = (int32_t)c->out->code.count;
            chunk_patch_i32(&c->out->code, jf + 1, after_ip);
            loop_pop_and_patch_breaks(c, after_ip);
            break;
        }

        case STMT_FOR: {
            scope_push(c);
            if (s->as.for_stmt.init) compile_stmt(c, s->as.for_stmt.init);

            int32_t cond_ip = (int32_t)c->out->code.count;
            loop_push(c, -1); /* continue target = post-expr, not known yet */

            size_t jf = 0; int has_cond = s->as.for_stmt.cond != NULL;
            if (has_cond) {
                compile_expr_value(c, s->as.for_stmt.cond);
                jf = chunk_emit_op(&c->out->code, OP_JUMP_IF_FALSE);
                chunk_emit_i32(&c->out->code, 0);
            }

            compile_stmt(c, s->as.for_stmt.body);

            int32_t post_ip = (int32_t)c->out->code.count;
            loop_patch_deferred_continues(c, post_ip);
            if (s->as.for_stmt.post) {
                compile_expr_value(c, s->as.for_stmt.post);
                chunk_emit_op(&c->out->code, OP_POP);
            }
            chunk_emit_op(&c->out->code, OP_JUMP);
            chunk_emit_i32(&c->out->code, cond_ip);

            int32_t after_ip = (int32_t)c->out->code.count;
            if (has_cond) chunk_patch_i32(&c->out->code, jf + 1, after_ip);
            loop_pop_and_patch_breaks(c, after_ip);
            scope_pop(c);
            break;
        }

        case STMT_BREAK: {
            size_t at = chunk_emit_op(&c->out->code, OP_JUMP);
            chunk_emit_i32(&c->out->code, 0);
            if (c->loop) DA_PUSH(c->loop->break_ips, c->loop->break_count, c->loop->break_cap, at);
            break;
        }
        case STMT_CONTINUE: {
            if (!c->loop) break;
            if (c->loop->continue_target >= 0) {
                chunk_emit_op(&c->out->code, OP_JUMP);
                chunk_emit_i32(&c->out->code, c->loop->continue_target);
            } else {
                size_t at = chunk_emit_op(&c->out->code, OP_JUMP);
                chunk_emit_i32(&c->out->code, 0);
                DA_PUSH(c->loop->continue_patch_ips, c->loop->continue_patch_count, c->loop->continue_patch_cap, at);
            }
            break;
        }

        case STMT_YIELD:
            if (s->as.yield_stmt.value) {
                compile_expr_value(c, s->as.yield_stmt.value);
                chunk_emit_op(&c->out->code, OP_RETURN);
            } else {
                chunk_emit_op(&c->out->code, OP_RETURN_VOID);
            }
            break;
    }
}

/* ============================================================================
 * Top level.
 * ========================================================================= */

int compile_program(const Program *prog, CompiledProgram *out) {
    memset(out, 0, sizeof(*out));
    chunk_init(&out->code);

    Compiler c = {0};
    c.out = out;
    c.ast = prog;

    build_struct_layouts(&c);

    /* Globals get the first block of memory slots. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_VAR) continue;
        const Stmt *v = prog->decls[i].as.var;
        GlobalVar g = { v->as.var_decl.name, &v->as.var_decl.type, c.next_slot };
        c.next_slot += slot_size_of(&c, &v->as.var_decl.type);
        DA_PUSH(c.globals, c.global_count, c.global_cap, g);
    }
    out->global_slot_count = c.next_slot;

    /* Function signature table (pass 1) — lets any call resolve to any
     * function regardless of declaration order. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_FUNC) continue;
        const FuncDecl *f = &prog->decls[i].as.func;
        FuncSigEntry fs = { f->name, (int32_t)c.funcsig_count, f->has_return_type,
                             f->has_return_type ? f->return_type : prim("void"), f->is_extern };
        DA_PUSH(c.funcsig, c.funcsig_count, c.funcsig_cap, fs);
    }
    out->funcs = calloc(c.funcsig_count, sizeof(FuncTableEntry));
    out->func_count = c.funcsig_count;
    for (size_t i = 0; i < c.funcsig_count; i++) {
        out->funcs[i].name = c.funcsig[i].name;
        out->funcs[i].entry_ip = -1;
    }

    int32_t main_index = -1;
    for (size_t i = 0; i < c.funcsig_count; i++)
        if (strcmp(c.funcsig[i].name, "main") == 0) { main_index = c.funcsig[i].index; break; }
    if (main_index < 0) {
        cerror(&c, "no 'main' function found — the VM needs one as the program entry point");
    }

    /* Entry prologue (ip 0): initialize globals in declaration order, call
     * main, then halt. Written before any function body so ip 0 is always
     * "start of program", independent of where functions end up. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_VAR) continue;
        const Stmt *v = prog->decls[i].as.var;
        if (!v->as.var_decl.init) continue;
        compile_expr_value(&c, v->as.var_decl.init);
        int32_t slot = 0;
        for (size_t g = 0; g < c.global_count; g++)
            if (strcmp(c.globals[g].name, v->as.var_decl.name) == 0) { slot = c.globals[g].slot; break; }
        chunk_emit_op(&out->code, OP_STORE_GLOBAL);
        chunk_emit_i32(&out->code, slot);
    }
    if (main_index >= 0) {
        chunk_emit_op(&out->code, OP_CALL);
        chunk_emit_i32(&out->code, main_index);
        chunk_emit_i32(&out->code, 0);
    }
    chunk_emit_op(&out->code, OP_HALT);

    /* Function bodies (pass 2). Extern functions have no body to compile —
     * their entry_ip is left at -1 (its calloc'd default), and compile_call
     * refuses to emit a call to one with a clear error rather than letting
     * the VM later hit an unset entry point. */
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->decls[i].kind != DECL_FUNC) continue;
        const FuncDecl *f = &prog->decls[i].as.func;
        if (f->is_extern) continue;
        int32_t idx = -1;
        for (size_t k = 0; k < c.funcsig_count; k++)
            if (strcmp(c.funcsig[k].name, f->name) == 0) { idx = c.funcsig[k].index; break; }

        out->funcs[idx].entry_ip = (int32_t)out->code.count;

        scope_push(&c);
        c.next_slot = 0;
        for (size_t p = 0; p < f->param_count; p++)
            declare_local(&c, f->params[p].name, &f->params[p].type);
        compile_block(&c, f->body);
        chunk_emit_op(&out->code, OP_RETURN_VOID); /* safety net if control falls off the end */
        scope_pop(&c);
    }

    while (c.scope) scope_pop(&c);
    for (size_t i = 0; i < c.struct_count; i++) free(c.structs[i].fields);
    free(c.structs);
    free(c.globals);
    free(c.funcsig);

    return c.had_error ? 1 : 0;
}

void compiled_program_free(CompiledProgram *cp) {
    chunk_free(&cp->code);
    free(cp->funcs);
    memset(cp, 0, sizeof(*cp));
}
