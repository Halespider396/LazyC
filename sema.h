#ifndef LAZYC_SEMA_H
#define LAZYC_SEMA_H

#include "ast.h"

/* Walks a parsed Program and reports semantic errors (undefined names,
 * duplicate declarations, type mismatches, invalid break/continue,
 * bad member access, etc.) to stderr in "line:col: message" form.
 *
 * Returns 1 if any semantic error was found, 0 if the program checks out.
 * Safe to call even on a program that had parse errors, but you should
 * generally only run this when the parser reported no errors — a partial
 * AST from a failed parse will likely produce a flood of unrelated
 * semantic errors. */
int sema_check_program(Program *prog);

#endif
