#ifndef ARENA_H
#define ARENA_H

#include "mix.h"

#define ARENA_DEFAULT_CAP (1024 * 1024) // 1 MB

typedef struct ArenaBlock {
    char *base;
    size_t used;
    size_t capacity;
    struct ArenaBlock *next;
} ArenaBlock;

struct Arena {
    ArenaBlock *current;
    ArenaBlock *first;
    size_t block_size; // default capacity for new blocks
};

Arena arena_create(size_t capacity);
void *arena_alloc(Arena *a, size_t size);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
void arena_destroy(Arena *a);

#endif // ARENA_H
