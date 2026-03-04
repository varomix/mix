#ifndef ARENA_H
#define ARENA_H

#include "mix.h"

#define ARENA_DEFAULT_CAP (1024 * 1024) // 1 MB

struct Arena {
    char *base;
    size_t used;
    size_t capacity;
};

Arena arena_create(size_t capacity);
void *arena_alloc(Arena *a, size_t size);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
void arena_destroy(Arena *a);

#endif // ARENA_H
