#ifndef LAZYC_BYTECODE_H
#define LAZYC_BYTECODE_H

#include <stddef.h>
#include <stdint.h>

/* One flat instruction stream for the whole program (all function bodies
 * concatenated). Calls jump by function *index* (resolved to an entry
 * offset at the start of VM execution, once every function has been
 * compiled), so call sites never care what order functions were compiled
 * in.
 *
 * Encoding: 1 byte opcode, followed by fixed-size operand bytes specific
 * to that opcode (documented per case below). Multi-byte operands are
 * native-endian int32_t/int64_t/double — this bytecode never leaves the
 * process, so portability across machines doesn't matter. */
typedef enum {
    OP_CONST_INT,     /* i64 value                 -> push VAL_INT   */
    OP_CONST_FLOAT,   /* f64 value                 -> push VAL_FLOAT */
    OP_CONST_STRING,  /* i32 string-pool index     -> push VAL_STRING */
    OP_CONST_CHAR,    /* i64 char code             -> push VAL_CHAR  */
    OP_CONST_TRUE,    /*                           -> push VAL_BOOL(1) */
    OP_CONST_FALSE,   /*                           -> push VAL_BOOL(0) */
    OP_CONST_NULL,    /*                           -> push VAL_NULL  */

    OP_LOAD_LOCAL,    /* i32 slot   -> push memory[fp + slot]        */
    OP_STORE_LOCAL,   /* i32 slot   -> pop v; memory[fp + slot] = v  */
    OP_LOAD_GLOBAL,   /* i32 slot   -> push memory[slot]             */
    OP_STORE_GLOBAL,  /* i32 slot   -> pop v; memory[slot] = v       */
    OP_ADDR_LOCAL,    /* i32 slot   -> push VAL_PTR(fp + slot)       */
    OP_ADDR_GLOBAL,   /* i32 slot   -> push VAL_PTR(slot)            */

    OP_FIELD_LOAD,    /* i32 offset -> pop addr; push memory[addr+off] */
    OP_FIELD_STORE,   /* i32 offset -> pop addr; pop v; memory[addr+off]=v */
    OP_FIELD_ADDR,    /* i32 offset -> pop addr; push VAL_PTR(addr+off) */
    OP_DEREF_LOAD,    /*            -> pop ptr; push memory[ptr.addr]  */
    OP_DEREF_STORE,   /*            -> pop ptr; pop v; memory[ptr.addr]=v */
    OP_INDEX_LOAD,    /*            -> pop idx, ptr; push memory[ptr.addr+idx] */
    OP_INDEX_STORE,   /*            -> pop idx; pop ptr; pop v; memory[ptr.addr+idx]=v */
    OP_INDEX_ADDR,    /*            -> pop idx, ptr; push VAL_PTR(ptr.addr+idx) */

    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_BIT_AND, OP_BIT_OR, OP_BIT_XOR, OP_SHL, OP_SHR,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NEQ,
    OP_NEG, OP_NOT, OP_BIT_NOT,
    OP_PTR_ADD_INT,   /*            -> pop int, ptr (either stack order — the VM
                                       * picks whichever operand is the pointer at
                                       * runtime); push VAL_PTR(ptr.addr + int)     */

    OP_JUMP,          /* i32 target (absolute ip)                     */
    OP_JUMP_IF_FALSE, /* i32 target -> pop cond; jump there if falsy  */
    OP_JUMP_IF_TRUE,  /* i32 target -> pop cond; jump there if truthy */

    OP_CALL,          /* i32 func_index, i32 arg_count                */
    OP_RETURN,        /*            -> pop v; return v to caller      */
    OP_RETURN_VOID,   /*            -> return, no value                */

    OP_POP,           /* discard top of stack (expr-statement result)  */
    OP_DUP,           /* duplicate top of stack                        */

    OP_PRINT,         /* i32 arg_count -> pop that many values (in call
                        * order), print them space-separated with a
                        * trailing newline; pushes nothing extra itself —
                        * the compiler follows this with a CONST_NULL so
                        * `print(...)` still behaves like any other
                        * value-producing expression                    */
    OP_STRLEN,        /*            -> pop VAL_STRING; push VAL_INT length */
    OP_STREQ,         /*            -> pop b, a (VAL_STRING); push VAL_BOOL */
    OP_ABS,           /*            -> pop v; push |v| (same numeric type) */
    OP_MIN,           /*            -> pop b, a; push the smaller           */
    OP_MAX,           /*            -> pop b, a; push the larger            */

    OP_HALT,
} OpCode;

typedef struct {
    uint8_t *code;
    size_t count, cap;

    char **strings;       /* string constant pool (owns each string) */
    size_t string_count, string_cap;
} Chunk;

void chunk_init(Chunk *c);
void chunk_free(Chunk *c);

/* Raw emitters used by the compiler. */
size_t chunk_emit_op(Chunk *c, OpCode op);          /* returns byte offset of this instruction */
void chunk_emit_i32(Chunk *c, int32_t v);
void chunk_emit_i64(Chunk *c, int64_t v);
void chunk_emit_f64(Chunk *c, double v);
void chunk_patch_i32(Chunk *c, size_t offset, int32_t v); /* for backpatched jump targets */
int32_t chunk_add_string(Chunk *c, const char *str);      /* interns (copies) str, returns pool index */

#endif
