#ifndef LAZYC_CODEGEN_C_H
#define LAZYC_CODEGEN_C_H

#include <stdio.h>
#include "ast.h"

/* Emits equivalent C source for a checked Program to `out`.
 * Assumes the program already passed sema_check_program() with no
 * errors — codegen does not re-validate types, arg counts, etc.
 *
 * Returns 0 on success. Currently the only failure mode is a write
 * error on `out`, which this doesn't specially detect — callers should
 * check ferror(out) after, if they care. */
int codegen_c_write(const Program *prog, FILE *out);

#endif
