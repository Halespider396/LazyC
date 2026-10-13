#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "sema.h"
#include "codegen_c.h"
#include "compile.h"
#include "vm.h"
#include "preprocess.h"

static int dump_tokens(const char *src, size_t len) {
    Lexer lx;
    lexer_init(&lx, src, len);
    int had_error = 0;

    for (;;) {
        Token t = lexer_next(&lx);
        char loc[32];
        snprintf(loc, sizeof(loc), "%d:%d", t.line, t.col);

        if (t.type == TOK_ERROR) {
            fprintf(stderr, "%-8s ERROR        '%.*s'  -- %s\n", loc,
                    (int)t.length, t.start, t.error_msg);
            had_error = 1;
        } else if (t.type == TOK_INT_LIT) {
            printf("%-8s %-12s '%.*s'  (int=%ld)\n", loc, token_type_name(t.type),
                   (int)t.length, t.start, t.int_val);
        } else if (t.type == TOK_FLOAT_LIT) {
            printf("%-8s %-12s '%.*s'  (float=%f)\n", loc, token_type_name(t.type),
                   (int)t.length, t.start, t.float_val);
        } else {
            printf("%-8s %-12s '%.*s'\n", loc, token_type_name(t.type),
                   (int)t.length, t.start);
        }
        if (t.type == TOK_EOF) break;
    }
    return had_error;
}

static int dump_ast(const char *src, size_t len) {
    Parser p;
    parser_init(&p, src, len);
    Program prog = parser_parse_program(&p);

    int had_error = p.had_error;
    if (!p.had_error) {
        had_error = sema_check_program(&prog);
        if (!had_error) ast_print_program(&prog);
    }

    ast_free_program(&prog);
    return had_error;
}

static int emit_c(const char *src, size_t len, const char *out_path) {
    Parser p;
    parser_init(&p, src, len);
    Program prog = parser_parse_program(&p);

    int had_error = p.had_error;
    if (!p.had_error) had_error = sema_check_program(&prog);

    if (!had_error) {
        FILE *out = fopen(out_path, "w");
        if (!out) { perror("fopen"); had_error = 1; }
        else {
            codegen_c_write(&prog, out);
            had_error = ferror(out) != 0;
            fclose(out);
        }
    }

    ast_free_program(&prog);
    return had_error;
}

/* `-o <binary>`: emit-c into a throwaway `<binary>.tmp.c`, then shell out to
 * gcc to turn that into a real native executable. Mirrors exactly what you'd
 * do by hand with `--emit-c` + `gcc`, just as one step. -lm is included
 * since `extern`-declared libc math functions (sqrt, etc.) are a documented
 * FFI use case. On a gcc failure, the intermediate .c file is left behind
 * (named in the error) so the C output can be inspected; on success it's
 * removed, leaving only the requested binary. */
static int compile_to_binary(const char *src, size_t len, const char *bin_out_path) {
    char tmp_c_path[4096];
    int n = snprintf(tmp_c_path, sizeof(tmp_c_path), "%s.tmp.c", bin_out_path);
    if (n < 0 || (size_t)n >= sizeof(tmp_c_path)) {
        fprintf(stderr, "error: output path too long\n");
        return 1;
    }

    if (emit_c(src, len, tmp_c_path)) return 1; /* emit_c already reported the error */

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "gcc";

    char cmd[8192];
    n = snprintf(cmd, sizeof(cmd), "%s -std=c11 -O2 \"%s\" -o \"%s\" -lm",
                 cc, tmp_c_path, bin_out_path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        fprintf(stderr, "error: command too long\n");
        return 1;
    }

    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "error: C compilation failed (see %s for the generated source)\n", tmp_c_path);
        return 1;
    }

    remove(tmp_c_path);
    return 0;
}

/* Returns -2 on compile/sema failure, -1 on a VM runtime error, or the
 * program's exit code (whatever `main` yielded) otherwise. */
static int run_program(const char *src, size_t len) {
    Parser p;
    parser_init(&p, src, len);
    Program prog = parser_parse_program(&p);

    int had_error = p.had_error;
    if (!p.had_error) had_error = sema_check_program(&prog);

    int exit_code = -2;
    if (!had_error) {
        CompiledProgram cp;
        if (compile_program(&prog, &cp) == 0) {
            exit_code = vm_run(&cp);
        }
        compiled_program_free(&cp);
    }

    ast_free_program(&prog);
    return exit_code;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--tokens|--emit-c <out.c>|--run|-o <out-binary>] <lazyc-source-file>\n", argv[0]);
        return 1;
    }

    int tokens_mode = 0, emit_c_mode = 0, run_mode = 0, compile_bin_mode = 0;
    const char *path = argv[1];
    const char *c_out_path = NULL;
    const char *bin_out_path = NULL;

    if (strcmp(argv[1], "--tokens") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s --tokens <lazyc-source-file>\n", argv[0]);
            return 1;
        }
        tokens_mode = 1;
        path = argv[2];
    } else if (strcmp(argv[1], "--emit-c") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: %s --emit-c <out.c> <lazyc-source-file>\n", argv[0]);
            return 1;
        }
        emit_c_mode = 1;
        c_out_path = argv[2];
        path = argv[3];
    } else if (strcmp(argv[1], "--run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s --run <lazyc-source-file>\n", argv[0]);
            return 1;
        }
        run_mode = 1;
        path = argv[2];
    } else if (strcmp(argv[1], "-o") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: %s -o <out-binary> <lazyc-source-file>\n", argv[0]);
            return 1;
        }
        compile_bin_mode = 1;
        bin_out_path = argv[2];
        path = argv[3];
    }

    size_t len;
    char *src = preprocess_file(path, &len);
    if (!src) return 1; /* preprocess_file already printed an error */

    int had_error;
    if (tokens_mode) had_error = dump_tokens(src, len);
    else if (emit_c_mode) had_error = emit_c(src, len, c_out_path);
    else if (compile_bin_mode) had_error = compile_to_binary(src, len, bin_out_path);
    else if (run_mode) {
        int code = run_program(src, len);
        free(src);
        if (code == -2) return 1;     /* compile/sema error, already reported */
        if (code == -1) return 1;     /* VM runtime error, already reported */
        return code;                   /* whatever main() yielded */
    }
    else had_error = dump_ast(src, len);

    free(src);
    return had_error ? 1 : 0;
}
