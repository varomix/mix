#include "arena.h"

static ArenaBlock *block_create(size_t capacity) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock));
    if (!block) {
        fprintf(stderr, "mix: out of memory\n");
        exit(1);
    }
    block->base = malloc(capacity);
    if (!block->base) {
        fprintf(stderr, "mix: out of memory\n");
        free(block);
        exit(1);
    }
    block->used = 0;
    block->capacity = capacity;
    block->next = NULL;
    return block;
}

Arena arena_create(size_t capacity) {
    Arena a;
    ArenaBlock *block = block_create(capacity);
    a.current = block;
    a.first = block;
    a.block_size = capacity;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~(size_t)7;

    // Try to fit in the current block
    if (a->current->used + size <= a->current->capacity) {
        void *ptr = a->current->base + a->current->used;
        a->current->used += size;
        return ptr;
    }

    // Current block is full — allocate a new one.
    // New block is at least double the default block size, or large enough for this request.
    size_t new_cap = a->block_size * 2;
    if (new_cap < size)
        new_cap = size;
    // Update default block size so future blocks keep growing
    a->block_size = new_cap;

    ArenaBlock *block = block_create(new_cap);
    // Append to the linked list
    a->current->next = block;
    a->current = block;

    void *ptr = block->base + block->used;
    block->used += size;
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
    ArenaBlock *block = a->first;
    while (block) {
        ArenaBlock *next = block->next;
        free(block->base);
        free(block);
        block = next;
    }
    a->current = NULL;
    a->first = NULL;
    a->block_size = 0;
}
