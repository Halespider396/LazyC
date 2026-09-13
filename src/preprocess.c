#define _POSIX_C_SOURCE 200809L
#include "preprocess.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny growable byte buffer -------------------------------------- */

static void sb_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap == 0 ? 4096 : *cap * 2;
        while (new_cap < *len + n + 1) new_cap *= 2;
        *buf = realloc(*buf, new_cap);
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
}

/* ---- path helpers ------------------------------------------------------ */

static char *dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return strdup(".");
    size_t n = (size_t)(slash - path);
    char *d = malloc(n + 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static char *join_path(const char *dir, const char *rel) {
    if (rel[0] == '/') return strdup(rel);
    size_t dn = strlen(dir), rn = strlen(rel);
    char *out = malloc(dn + 1 + rn + 1);
    memcpy(out, dir, dn);
    out[dn] = '/';
    memcpy(out + dn + 1, rel, rn);
    out[dn + 1 + rn] = '\0';
    return out;
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Strips the surrounding quote characters from a raw STRING_LIT token's
 * text. Paths don't generally need escape decoding, so this is
 * deliberately simpler than compile.c's decode_string_literal. */
static char *strip_quotes(const char *raw, size_t len) {
    size_t inner = len >= 2 ? len - 2 : 0;
    char *s = malloc(inner + 1);
    memcpy(s, raw + 1, inner);
    s[inner] = '\0';
    return s;
}

/* ---- borrow-cycle / already-included tracking --------------------------- */

typedef struct {
    char **included; size_t included_count, included_cap;
    char **in_progress; size_t in_progress_count, in_progress_cap;
} PPState;

static int contains(char **arr, size_t count, const char *s) {
    for (size_t i = 0; i < count; i++) if (strcmp(arr[i], s) == 0) return 1;
    return 0;
}
static void push(char ***arr, size_t *count, size_t *cap, const char *s) {
    if (*count >= *cap) { *cap = *cap == 0 ? 4 : *cap * 2; *arr = realloc(*arr, *cap * sizeof(char *)); }
    (*arr)[(*count)++] = strdup(s);
}

static int expand_into(PPState *st, const char *path, char **buf, size_t *len, size_t *cap) {
    if (contains(st->in_progress, st->in_progress_count, path)) {
        fprintf(stderr, "borrow error: cycle detected — '%s' borrows itself (directly or indirectly)\n", path);
        return 0;
    }
    if (contains(st->included, st->included_count, path)) return 1; /* already spliced in once */

    size_t content_len;
    char *content = read_whole_file(path, &content_len);
    if (!content) {
        fprintf(stderr, "borrow error: could not open '%s'\n", path);
        return 0;
    }

    push(&st->in_progress, &st->in_progress_count, &st->in_progress_cap, path);
    char *dir = dirname_of(path);

    Lexer lx;
    lexer_init(&lx, content, content_len);
    size_t flushed_to = 0;
    int ok = 1;

    for (;;) {
        Lexer before = lx;
        Token t = lexer_next(&lx);
        if (t.type == TOK_EOF) break;

        if (t.type == TOK_IDENT && t.length == 6 && memcmp(t.start, "borrow", 6) == 0) {
            Lexer probe = lx; /* lx is already past the 'borrow' token here */
            Token str_tok = lexer_next(&probe);
            Token semi_tok = lexer_next(&probe);
            if (str_tok.type == TOK_STRING_LIT && semi_tok.type == TOK_SEMI) {
                /* flush everything before 'borrow' verbatim */
                sb_append(buf, len, cap, content + flushed_to, (size_t)(t.start - content) - flushed_to);

                char *rel = strip_quotes(str_tok.start, str_tok.length);
                char *resolved = join_path(dir, rel);
                free(rel);

                if (!expand_into(st, resolved, buf, len, cap)) { ok = 0; free(resolved); break; }
                sb_append(buf, len, cap, "\n", 1);
                free(resolved);

                lx = probe; /* actually consume the string + semicolon tokens */
                flushed_to = (size_t)(semi_tok.start + semi_tok.length - content);
                continue;
            }
        }
        (void)before;
    }

    if (ok) sb_append(buf, len, cap, content + flushed_to, content_len - flushed_to);

    free(dir);
    free(content);

    /* move from in_progress to included */
    free(st->in_progress[--st->in_progress_count]);
    if (ok) push(&st->included, &st->included_count, &st->included_cap, path);

    return ok;
}

char *preprocess_file(const char *path, size_t *out_len) {
    PPState st = {0};
    char *buf = NULL; size_t len = 0, cap = 0;

    int ok = expand_into(&st, path, &buf, &len, &cap);

    for (size_t i = 0; i < st.included_count; i++) free(st.included[i]);
    free(st.included);
    for (size_t i = 0; i < st.in_progress_count; i++) free(st.in_progress[i]);
    free(st.in_progress);

    if (!ok) { free(buf); return NULL; }

    sb_append(&buf, &len, &cap, "\0", 1);
    len -= 1; /* don't count the terminator in the reported length */
    *out_len = len;
    return buf;
}
