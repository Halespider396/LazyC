#ifndef LAZYC_COMPILE_H
#define LAZYC_COMPILE_H

#include "ast.h"
#include "bytecode.h"

typedef struct {
    char *name;
    int32_t entry_ip;
} FuncTableEntry;

typedef struct {
    Chunk code;
    FuncTableEntry *funcs;
    size_t func_count;
    int32_t global_slot_count;   /* total Value slots globals occupy at the base of memory */
} CompiledProgram;

/* Compiles a Program that has already passed sema_check_program() with no
 * errors. Returns 0 on success and fills `out`; returns 1 if the VM
 * backend can't represent something in the program (see limitations
 * below) and prints a message to stderr — `out` is unusable in that case,
 * but still safe to pass to compiled_program_free().
 *
 * Known v1 limitation: structs can only be used by pointer or as the
 * base of `.field`/`->field` access — passing, returning, or assigning a
 * whole struct *by value* isn't supported yet (the VM has no single
 * "struct value" it could push on the stack; every real program we've
 * exercised so far already uses the by-pointer style). Using a struct
 * this way is reported as a compile error here rather than silently
 * corrupting the VM's stack. */
int compile_program(const Program *prog, CompiledProgram *out);

void compiled_program_free(CompiledProgram *cp);

#endif
