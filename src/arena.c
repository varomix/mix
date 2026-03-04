#include "arena.h"

Arena arena_create(size_t capacity) {
    Arena a;
    a.base = malloc(capacity);
    if (!a.base) {
        fprintf(stderr, "mix: out of memory\n");
        exit(1);
    }
    a.used = 0;
    a.capacity = capacity;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~(size_t)7;
    if (a->used + size > a->capacity) {
        // Double capacity
        size_t new_cap = a->capacity * 2;
        while (a->used + size > new_cap)
            new_cap *= 2;
        char *new_base = realloc(a->base, new_cap);
        if (!new_base) {
            fprintf(stderr, "mix: out of memory\n");
            exit(1);
        }
        a->base = new_base;
        a->capacity = new_cap;
    }
    void *ptr = a->base + a->used;
    a->used += size;
    return ptr;
}

char *arena_strdup(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *copy = arena_alloc(a, len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *copy = arena_alloc(a, n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

void arena_destroy(Arena *a) {
    free(a->base);
    a->base = NULL;
    a->used = 0;
    a->capacity = 0;
}
