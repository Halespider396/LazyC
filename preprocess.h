#ifndef LAZYC_PREPROCESS_H
#define LAZYC_PREPROCESS_H

#include <stddef.h>

/* Expands every `borrow "path";` directive in the file at `path`,
 * recursively, and returns one combined source buffer (caller frees with
 * free()). A relative borrow path is resolved relative to the directory
 * of the file doing the borrowing (like C's #include "..."), not the
 * current working directory.
 *
 * Each file is only ever spliced in once, even if reached via multiple
 * borrow paths (like an automatic #pragma once) — so a diamond-shaped
 * borrow graph (A and B both borrow C) doesn't produce duplicate-
 * definition errors the way raw C #include would without guards.
 * Files are matched by their exact path text, not by resolving symlinks
 * or relative-path differences — borrowing the same file via two
 * differently-spelled paths will include it twice.
 *
 * Known limitation: line/column numbers in parser and sema error
 * messages are counted in the *combined* output, not remapped back to
 * each original file (the way a real C preprocessor's `# line "file"`
 * markers do). For a single borrowed file this is usually still easy to
 * follow; for several nested borrows it can get confusing. Fixing that
 * properly means threading a filename through every diagnostic in the
 * pipeline, which is a bigger change than this first version of borrow
 * is worth.
 *
 * On error (missing file, I/O failure, or a borrow cycle) prints a
 * message to stderr and returns NULL. */
char *preprocess_file(const char *path, size_t *out_len);

#endif
