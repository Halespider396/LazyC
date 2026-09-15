#include "vm.h"
#include "bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Memory model
 *
 * `memory` is one flat array of Values, backed by a growable buffer
 * (`memory_cap`) that only ever grows for capacity reasons. Slots
 * [0, global_slot_count) are the program's globals, permanently.
 * Everything from global_slot_count upward is "frame space": each call
 * gets a fresh region starting at `high_water` (the top of everything
 * currently in use), addressed as memory[fp + slot]. On return,
 * `high_water` is reset back to the returning frame's own `fp` — since a
 * function's local slot offsets are fixed at compile time relative to its
 * own fp, this reclaims exactly that frame's slots (and any nested calls'
 * frames, which have already reclaimed themselves by the time we get
 * here). So `memory` behaves like a real stack: it grows during deep call
 * chains and shrinks back on return, rather than only ever growing.
 * There's still no stack *overflow detection* — only whatever the
 * process's actual memory limit eventually enforces — but steady-state
 * recursion (e.g. a loop of calls) no longer leaks.
 *
 * A separate `stack` is the VM's expression/operand stack — the thing
 * bytecode instructions actually push/pop, distinct from `memory`.
 * ========================================================================= */

typedef struct {
    const CompiledProgram *prog;

    Value *memory; size_t memory_cap;
    size_t high_water;   /* slots [0, high_water) have been touched */
    size_t fp;           /* current frame's base slot index */

    Value *stack; size_t sp, stack_cap;

    struct { int32_t ret_ip; size_t saved_fp; } *calls;
    size_t call_count, call_cap;

    size_t ip;
    int had_error;
} VM;

#define GROW(arr, count, cap, type) \
    do { if ((count) >= (cap)) { (cap) = (cap) == 0 ? 64 : (cap) * 2; (arr) = realloc((arr), (cap) * sizeof(type)); } } while (0)

static void rt_error(VM *vm, const char *msg) {
    vm->had_error = 1;
    fprintf(stderr, "runtime error at ip %zu: %s\n", vm->ip, msg);
}

static void push(VM *vm, Value v) {
    GROW(vm->stack, vm->sp, vm->stack_cap, Value);
    vm->stack[vm->sp++] = v;
}
static Value pop(VM *vm) {
    if (vm->sp == 0) { rt_error(vm, "operand stack underflow"); Value z = {VAL_NULL, {0}}; return z; }
    return vm->stack[--vm->sp];
}
static Value peek(VM *vm) {
    if (vm->sp == 0) { rt_error(vm, "operand stack underflow"); Value z = {VAL_NULL, {0}}; return z; }
    return vm->stack[vm->sp - 1];
}

static void ensure_memory(VM *vm, size_t index) {
    if (index < vm->memory_cap) return;
    size_t new_cap = vm->memory_cap == 0 ? 256 : vm->memory_cap * 2;
    while (new_cap <= index) new_cap *= 2;
    size_t old_cap = vm->memory_cap;
    vm->memory = realloc(vm->memory, new_cap * sizeof(Value));
    memset(vm->memory + old_cap, 0, (new_cap - old_cap) * sizeof(Value)); /* zeroed = VAL_INT 0, safe */
    vm->memory_cap = new_cap;
}
static Value mem_get(VM *vm, size_t index) {
    ensure_memory(vm, index);
    if (index + 1 > vm->high_water) vm->high_water = index + 1;
    return vm->memory[index];
}
static void mem_set(VM *vm, size_t index, Value v) {
    ensure_memory(vm, index);
    if (index + 1 > vm->high_water) vm->high_water = index + 1;
    vm->memory[index] = v;
}

static int is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL: return v.as.b != 0;
        case VAL_INT: return v.as.i != 0;
        case VAL_CHAR: return v.as.i != 0;
        case VAL_FLOAT: return v.as.f != 0.0;
        case VAL_PTR: return 1;
        case VAL_STRING: return 1;
        case VAL_NULL: return 0;
    }
    return 0;
}

static double as_double(Value v) { return v.type == VAL_FLOAT ? v.as.f : (double)v.as.i; }
static int is_float_op(Value a, Value b) { return a.type == VAL_FLOAT || b.type == VAL_FLOAT; }

static Value vint(int64_t i) { Value v; v.type = VAL_INT; v.as.i = i; return v; }
static Value vfloat(double f) { Value v; v.type = VAL_FLOAT; v.as.f = f; return v; }
static Value vbool(int b) { Value v; v.type = VAL_BOOL; v.as.b = b; return v; }
static Value vptr(int64_t addr) { Value v; v.type = VAL_PTR; v.as.addr = addr; return v; }

static int require_ptr(VM *vm, Value v, int64_t *addr_out) {
    if (v.type != VAL_PTR) { rt_error(vm, "null or non-pointer dereference"); return 0; }
    *addr_out = v.as.addr;
    return 1;
}

static int32_t read_i32(VM *vm) { int32_t v; memcpy(&v, vm->prog->code.code + vm->ip, sizeof(v)); vm->ip += sizeof(v); return v; }
static int64_t read_i64(VM *vm) { int64_t v; memcpy(&v, vm->prog->code.code + vm->ip, sizeof(v)); vm->ip += sizeof(v); return v; }
static double read_f64(VM *vm) { double v; memcpy(&v, vm->prog->code.code + vm->ip, sizeof(v)); vm->ip += sizeof(v); return v; }

int vm_run(const CompiledProgram *prog) {
    VM vm = {0};
    vm.prog = prog;
    vm.fp = (size_t)prog->global_slot_count;
    vm.high_water = (size_t)prog->global_slot_count;
    ensure_memory(&vm, vm.high_water == 0 ? 0 : vm.high_water - 1);

    int running = 1;
    Value result = vint(0);

    while (running && !vm.had_error) {
        if (vm.ip >= prog->code.count) { rt_error(&vm, "instruction pointer ran off the end of the program"); break; }
        OpCode op = (OpCode)prog->code.code[vm.ip++];

        switch (op) {
            case OP_CONST_INT: push(&vm, vint(read_i64(&vm))); break;
            case OP_CONST_FLOAT: push(&vm, vfloat(read_f64(&vm))); break;
            case OP_CONST_STRING: {
                int32_t idx = read_i32(&vm);
                Value v; v.type = VAL_STRING; v.as.s = prog->code.strings[idx];
                push(&vm, v);
                break;
            }
            case OP_CONST_TRUE: push(&vm, vbool(1)); break;
            case OP_CONST_FALSE: push(&vm, vbool(0)); break;
            case OP_CONST_NULL: { Value v; v.type = VAL_NULL; v.as.i = 0; push(&vm, v); break; }
            case OP_CONST_CHAR: { Value v; v.type = VAL_CHAR; v.as.i = read_i64(&vm); push(&vm, v); break; }

            case OP_LOAD_LOCAL: { int32_t slot = read_i32(&vm); push(&vm, mem_get(&vm, vm.fp + (size_t)slot)); break; }
            case OP_STORE_LOCAL: { int32_t slot = read_i32(&vm); Value v = pop(&vm); mem_set(&vm, vm.fp + (size_t)slot, v); break; }
            case OP_LOAD_GLOBAL: { int32_t slot = read_i32(&vm); push(&vm, mem_get(&vm, (size_t)slot)); break; }
            case OP_STORE_GLOBAL: { int32_t slot = read_i32(&vm); Value v = pop(&vm); mem_set(&vm, (size_t)slot, v); break; }
            case OP_ADDR_LOCAL: { int32_t slot = read_i32(&vm); push(&vm, vptr((int64_t)(vm.fp + (size_t)slot))); break; }
            case OP_ADDR_GLOBAL: { int32_t slot = read_i32(&vm); push(&vm, vptr((int64_t)slot)); break; }

            case OP_FIELD_LOAD: {
                int32_t off = read_i32(&vm);
                int64_t addr; Value base = pop(&vm);
                if (!require_ptr(&vm, base, &addr)) break;
                push(&vm, mem_get(&vm, (size_t)(addr + off)));
                break;
            }
            case OP_FIELD_STORE: {
                int32_t off = read_i32(&vm);
                int64_t addr; Value base = pop(&vm); Value v = pop(&vm);
                if (!require_ptr(&vm, base, &addr)) break;
                mem_set(&vm, (size_t)(addr + off), v);
                break;
            }
            case OP_FIELD_ADDR: {
                int32_t off = read_i32(&vm);
                int64_t addr; Value base = pop(&vm);
                if (!require_ptr(&vm, base, &addr)) break;
                push(&vm, vptr(addr + off));
                break;
            }
            case OP_DEREF_LOAD: {
                int64_t addr; Value p = pop(&vm);
                if (!require_ptr(&vm, p, &addr)) break;
                push(&vm, mem_get(&vm, (size_t)addr));
                break;
            }
            case OP_DEREF_STORE: {
                int64_t addr; Value p = pop(&vm); Value v = pop(&vm);
                if (!require_ptr(&vm, p, &addr)) break;
                mem_set(&vm, (size_t)addr, v);
                break;
            }
            case OP_INDEX_LOAD: {
                int64_t addr; Value p = pop(&vm); Value idx = pop(&vm);
                if (!require_ptr(&vm, p, &addr)) break;
                push(&vm, mem_get(&vm, (size_t)(addr + idx.as.i)));
                break;
            }
            case OP_INDEX_STORE: {
                int64_t addr; Value p = pop(&vm); Value idx = pop(&vm); Value v = pop(&vm);
                if (!require_ptr(&vm, p, &addr)) break;
                mem_set(&vm, (size_t)(addr + idx.as.i), v);
                break;
            }
            case OP_INDEX_ADDR: {
                int64_t addr; Value p = pop(&vm); Value idx = pop(&vm);
                if (!require_ptr(&vm, p, &addr)) break;
                push(&vm, vptr(addr + idx.as.i));
                break;
            }

            case OP_ADD: { Value b = pop(&vm), a = pop(&vm); push(&vm, is_float_op(a,b) ? vfloat(as_double(a)+as_double(b)) : vint(a.as.i+b.as.i)); break; }
            case OP_SUB: { Value b = pop(&vm), a = pop(&vm); push(&vm, is_float_op(a,b) ? vfloat(as_double(a)-as_double(b)) : vint(a.as.i-b.as.i)); break; }
            case OP_MUL: { Value b = pop(&vm), a = pop(&vm); push(&vm, is_float_op(a,b) ? vfloat(as_double(a)*as_double(b)) : vint(a.as.i*b.as.i)); break; }
            case OP_DIV: {
                Value b = pop(&vm), a = pop(&vm);
                if (is_float_op(a, b)) { push(&vm, vfloat(as_double(a) / as_double(b))); break; }
                if (b.as.i == 0) { rt_error(&vm, "division by zero"); break; }
                push(&vm, vint(a.as.i / b.as.i));
                break;
            }
            case OP_MOD: {
                Value b = pop(&vm), a = pop(&vm);
                if (b.as.i == 0) { rt_error(&vm, "modulo by zero"); break; }
                push(&vm, vint(a.as.i % b.as.i));
                break;
            }
            case OP_BIT_AND: { Value b = pop(&vm), a = pop(&vm); push(&vm, vint(a.as.i & b.as.i)); break; }
            case OP_BIT_OR:  { Value b = pop(&vm), a = pop(&vm); push(&vm, vint(a.as.i | b.as.i)); break; }
            case OP_BIT_XOR: { Value b = pop(&vm), a = pop(&vm); push(&vm, vint(a.as.i ^ b.as.i)); break; }
            case OP_SHL:     { Value b = pop(&vm), a = pop(&vm); push(&vm, vint(a.as.i << b.as.i)); break; }
            case OP_SHR:     { Value b = pop(&vm), a = pop(&vm); push(&vm, vint(a.as.i >> b.as.i)); break; }

            case OP_LT: { Value b = pop(&vm), a = pop(&vm); push(&vm, vbool(is_float_op(a,b) ? as_double(a)<as_double(b) : a.as.i<b.as.i)); break; }
            case OP_GT: { Value b = pop(&vm), a = pop(&vm); push(&vm, vbool(is_float_op(a,b) ? as_double(a)>as_double(b) : a.as.i>b.as.i)); break; }
            case OP_LE: { Value b = pop(&vm), a = pop(&vm); push(&vm, vbool(is_float_op(a,b) ? as_double(a)<=as_double(b) : a.as.i<=b.as.i)); break; }
            case OP_GE: { Value b = pop(&vm), a = pop(&vm); push(&vm, vbool(is_float_op(a,b) ? as_double(a)>=as_double(b) : a.as.i>=b.as.i)); break; }
            case OP_EQ: {
                Value b = pop(&vm), a = pop(&vm);
                int r;
                if (a.type == VAL_NULL || b.type == VAL_NULL) r = (a.type == VAL_NULL) && (b.type == VAL_NULL);
                else if (a.type == VAL_STRING && b.type == VAL_STRING) r = strcmp(a.as.s, b.as.s) == 0;
                else if (is_float_op(a, b)) r = as_double(a) == as_double(b);
                else if (a.type == VAL_PTR || b.type == VAL_PTR) r = a.as.addr == b.as.addr;
                else r = a.as.i == b.as.i;
                push(&vm, vbool(r));
                break;
            }
            case OP_NEQ: {
                Value b = pop(&vm), a = pop(&vm);
                int r;
                if (a.type == VAL_NULL || b.type == VAL_NULL) r = !(a.type == VAL_NULL && b.type == VAL_NULL);
                else if (a.type == VAL_STRING && b.type == VAL_STRING) r = strcmp(a.as.s, b.as.s) != 0;
                else if (is_float_op(a, b)) r = as_double(a) != as_double(b);
                else if (a.type == VAL_PTR || b.type == VAL_PTR) r = a.as.addr != b.as.addr;
                else r = a.as.i != b.as.i;
                push(&vm, vbool(r));
                break;
            }

            case OP_NEG: { Value a = pop(&vm); push(&vm, a.type == VAL_FLOAT ? vfloat(-a.as.f) : vint(-a.as.i)); break; }
            case OP_NOT: { Value a = pop(&vm); push(&vm, vbool(!is_truthy(a))); break; }
            case OP_BIT_NOT: { Value a = pop(&vm); push(&vm, vint(~a.as.i)); break; }

            case OP_PTR_ADD_INT: {
                Value b = pop(&vm), a = pop(&vm);
                Value ptrv = a.type == VAL_PTR ? a : b;
                Value intv = a.type == VAL_PTR ? b : a;
                push(&vm, vptr(ptrv.as.addr + intv.as.i));
                break;
            }

            case OP_JUMP: { int32_t target = read_i32(&vm); vm.ip = (size_t)target; break; }
            case OP_JUMP_IF_FALSE: { int32_t target = read_i32(&vm); Value cond = pop(&vm); if (!is_truthy(cond)) vm.ip = (size_t)target; break; }
            case OP_JUMP_IF_TRUE:  { int32_t target = read_i32(&vm); Value cond = pop(&vm); if (is_truthy(cond)) vm.ip = (size_t)target; break; }

            case OP_CALL: {
                int32_t func_index = read_i32(&vm);
                int32_t arg_count = read_i32(&vm);
                if (func_index < 0 || (size_t)func_index >= prog->func_count) { rt_error(&vm, "call to invalid function index"); break; }
                int32_t entry_ip = prog->funcs[func_index].entry_ip;
                if (entry_ip < 0) { rt_error(&vm, "call to a function with no compiled body"); break; }

                size_t new_fp = vm.high_water;
                /* place args into the new frame's slots [0, arg_count), highest-popped-first */
                for (int32_t i = arg_count - 1; i >= 0; i--) {
                    Value v = pop(&vm);
                    mem_set(&vm, new_fp + (size_t)i, v);
                }

                GROW(vm.calls, vm.call_count, vm.call_cap, *vm.calls);
                vm.calls[vm.call_count].ret_ip = (int32_t)vm.ip;
                vm.calls[vm.call_count].saved_fp = vm.fp;
                vm.call_count++;

                vm.fp = new_fp;
                vm.ip = (size_t)entry_ip;
                break;
            }

            case OP_RETURN:
            case OP_RETURN_VOID: {
                Value ret = vint(0);
                int has_ret = (op == OP_RETURN);
                if (has_ret) ret = pop(&vm);

                if (vm.call_count == 0) {
                    /* returning from the prologue's call to main */
                    result = ret;
                    running = 0;
                    break;
                }
                vm.call_count--;
                vm.high_water = vm.fp; /* reclaim this frame's slots (and any nested frames') */
                vm.ip = (size_t)vm.calls[vm.call_count].ret_ip;
                vm.fp = vm.calls[vm.call_count].saved_fp;
                if (has_ret) {
                    push(&vm, ret);
                } else {
                    /* Every compiled call site expects exactly one value
                     * to come back (see compile.c's "always leaves one
                     * value" contract for compile_expr_value) — including
                     * calls to void functions used as statements. Without
                     * this, a void call left the operand stack one short
                     * and the caller's next OP_POP would underflow. */
                    Value placeholder; placeholder.type = VAL_NULL; placeholder.as.i = 0;
                    push(&vm, placeholder);
                }
                break;
            }

            case OP_POP: pop(&vm); break;
            case OP_DUP: { Value v = peek(&vm); push(&vm, v); break; }

            case OP_PRINT: {
                int32_t argc = read_i32(&vm);
                Value *args = malloc((size_t)argc * sizeof(Value));
                for (int32_t i = argc - 1; i >= 0; i--) args[i] = pop(&vm);
                for (int32_t i = 0; i < argc; i++) {
                    if (i > 0) fputc(' ', stdout);
                    switch (args[i].type) {
                        case VAL_INT: printf("%lld", (long long)args[i].as.i); break;
                        case VAL_FLOAT: printf("%g", args[i].as.f); break;
                        case VAL_BOOL: fputs(args[i].as.b ? "true" : "false", stdout); break;
                        case VAL_CHAR: fputc((int)(unsigned char)args[i].as.i, stdout); break;
                        case VAL_STRING: fputs(args[i].as.s, stdout); break;
                        case VAL_PTR: printf("<ptr:%lld>", (long long)args[i].as.addr); break;
                        case VAL_NULL: fputs("null", stdout); break;
                    }
                }
                fputc('\n', stdout);
                free(args);
                break;
            }
            case OP_STRLEN: { Value s = pop(&vm); push(&vm, vint((int64_t)strlen(s.as.s))); break; }
            case OP_STREQ: { Value b = pop(&vm), a = pop(&vm); push(&vm, vbool(strcmp(a.as.s, b.as.s) == 0)); break; }
            case OP_ABS: {
                Value a = pop(&vm);
                push(&vm, a.type == VAL_FLOAT ? vfloat(a.as.f < 0 ? -a.as.f : a.as.f)
                                               : vint(a.as.i < 0 ? -a.as.i : a.as.i));
                break;
            }
            case OP_MIN: {
                Value b = pop(&vm), a = pop(&vm);
                if (is_float_op(a, b)) push(&vm, vfloat(as_double(a) < as_double(b) ? as_double(a) : as_double(b)));
                else push(&vm, vint(a.as.i < b.as.i ? a.as.i : b.as.i));
                break;
            }
            case OP_MAX: {
                Value b = pop(&vm), a = pop(&vm);
                if (is_float_op(a, b)) push(&vm, vfloat(as_double(a) > as_double(b) ? as_double(a) : as_double(b)));
                else push(&vm, vint(a.as.i > b.as.i ? a.as.i : b.as.i));
                break;
            }

            case OP_HALT:
                if (vm.sp > 0) result = vm.stack[vm.sp - 1];
                running = 0;
                break;
        }
    }

    free(vm.memory);
    free(vm.stack);
    free(vm.calls);

    if (vm.had_error) return -1;
    if (result.type == VAL_FLOAT) return (int)result.as.f;
    if (result.type == VAL_BOOL) return result.as.b;
    return (int)result.as.i;
}
