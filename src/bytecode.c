#define _POSIX_C_SOURCE 200809L
#include "bytecode.h"
#include <stdlib.h>
#include <string.h>

void chunk_init(Chunk *c) {
    memset(c, 0, sizeof(*c));
}

void chunk_free(Chunk *c) {
    free(c->code);
    for (size_t i = 0; i < c->string_count; i++) free(c->strings[i]);
    free(c->strings);
    memset(c, 0, sizeof(*c));
}

static void ensure_cap(Chunk *c, size_t more) {
    if (c->count + more <= c->cap) return;
    size_t new_cap = c->cap == 0 ? 256 : c->cap * 2;
    while (new_cap < c->count + more) new_cap *= 2;
    c->code = realloc(c->code, new_cap);
    c->cap = new_cap;
}

size_t chunk_emit_op(Chunk *c, OpCode op) {
    ensure_cap(c, 1);
    size_t at = c->count;
    c->code[c->count++] = (uint8_t)op;
    return at;
}

void chunk_emit_i32(Chunk *c, int32_t v) {
    ensure_cap(c, sizeof(v));
    memcpy(c->code + c->count, &v, sizeof(v));
    c->count += sizeof(v);
}

void chunk_emit_i64(Chunk *c, int64_t v) {
    ensure_cap(c, sizeof(v));
    memcpy(c->code + c->count, &v, sizeof(v));
    c->count += sizeof(v);
}

void chunk_emit_f64(Chunk *c, double v) {
    ensure_cap(c, sizeof(v));
    memcpy(c->code + c->count, &v, sizeof(v));
    c->count += sizeof(v);
}

void chunk_patch_i32(Chunk *c, size_t offset, int32_t v) {
    memcpy(c->code + offset, &v, sizeof(v));
}

int32_t chunk_add_string(Chunk *c, const char *str) {
    if (c->string_count >= c->string_cap) {
        c->string_cap = c->string_cap == 0 ? 8 : c->string_cap * 2;
        c->strings = realloc(c->strings, c->string_cap * sizeof(char *));
    }
    c->strings[c->string_count] = strdup(str);
    return (int32_t)c->string_count++;
}
