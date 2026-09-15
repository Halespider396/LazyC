#ifndef LAZYC_VM_H
#define LAZYC_VM_H

#include "compile.h"
#include <stdint.h>

typedef enum {
  VAL_INT,
  VAL_FLOAT,
  VAL_BOOL,
  VAL_CHAR,
  VAL_STRING,
  VAL_PTR,
  VAL_NULL
} ValueType;

typedef struct {
  ValueType type;
  union {
    int64_t i;
    double f;
    int b;
    const char *s; /* points into the chunk's string pool; not owned here */
    int64_t addr;  /* VAL_PTR: index into the VM's flat memory array */
  } as;
} Value;

/* Runs a compiled program to completion.
 * Returns the int64 value the program's `main` yielded (truncated the same
 * way a C `int main()` return value becomes a process exit code), or 0 if
 * main yielded no value / doesn't exist / yielded a non-int.
 * Prints a message to stderr and returns -1 on a runtime error (e.g. null
 * dereference, division by zero). */
int vm_run(const CompiledProgram *prog);

#endif
