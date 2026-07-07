#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

// MIX runtime — linked with every MIX program

// Print functions with newline
void mix_print_int(int64_t val) {
    printf("%" PRId64 "\n", val);
}

static void print_float_val(double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        strcat(buf, ".0");
    }
    printf("%s", buf);
}

void mix_print_float(double val) {
    print_float_val(val);
    printf("\n");
}

void mix_print_str(const char *val) {
    printf("%s\n", val ? val : "");
}

void mix_print_bool(int val) {
    printf("%s\n", val ? "true" : "false");
}

// Print functions WITHOUT newline (for string interpolation)
void mix_write_int(int64_t val) {
    printf("%" PRId64, val);
}

void mix_write_float(double val) {
    print_float_val(val);
}

void mix_write_str(const char *val) {
    printf("%s", val);
}

void mix_write_bool(int val) {
    printf("%s", val ? "true" : "false");
}

void mix_write_newline(void) {
    printf("\n");
}

// Panic — abort with message
void mix_panic(const char *msg) {
    fprintf(stderr, "panic: %s\n", msg);
    exit(1);
}

// Assert — abort if condition is false
void mix_assert(int32_t cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "assertion failed: %s\n", msg ? msg : "");
        exit(1);
    }
}

// Memory
void *mix_alloc(int64_t size) {
    void *ptr = malloc((size_t)size);
    if (!ptr) {
        mix_panic("out of memory");
    }
    return ptr;
}

void mix_free(void *ptr) {
    free(ptr);
}

// Allocate zeroed bytes (for SDL_Event buffers, etc.)
void *mix_bytes(int64_t n) {
    void *ptr = calloc(1, (size_t)n);
    if (!ptr) mix_panic("out of memory");
    return ptr;
}

// Read a uint32 from a pointer (for reading SDL_Event.type)
uint32_t mix_peek_u32(const void *ptr) {
    return *(const uint32_t *)ptr;
}

// Read a uint32 at byte offset, widened to int64. Backs the `peek_u32`
// builtin — returning int64 lets it participate in normal int arithmetic
// without backend-side w/l-mismatch issues.
int64_t mix_peek_u32_at(const void *ptr, int64_t offset) {
    uint32_t v;
    memcpy(&v, (const char *)ptr + offset, sizeof(uint32_t));
    return (int64_t)v;
}

// Read a single byte at offset, widened to int64.
int64_t mix_peek_byte(const void *ptr, int64_t offset) {
    return (int64_t)((const uint8_t *)ptr)[offset];
}

// Read a 32-bit float at byte offset, widened to double.
double mix_peek_f32(const void *ptr, int64_t offset) {
    float v;
    memcpy(&v, (const char *)ptr + offset, sizeof(float));
    return (double)v;
}

// memcpy exposed to MIX as a builtin.
void mix_memcpy(void *dst, const void *src, int64_t n) {
    memcpy(dst, src, (size_t)n);
}

// Pack 2 structs contiguously. Returns malloc'd buffer with both copied.
void *mix_pack2(void *a, void *b, int64_t elem_size) {
    void *buf = calloc(2, elem_size);
    if (!buf) mix_panic("out of memory");
    memcpy(buf, a, elem_size);
    memcpy((char *)buf + elem_size, b, elem_size);
    return buf;
}

// Pack 3 structs contiguously.
void *mix_pack3(void *a, void *b, void *c, int64_t elem_size) {
    void *buf = calloc(3, elem_size);
    if (!buf) mix_panic("out of memory");
    memcpy(buf, a, elem_size);
    memcpy((char *)buf + elem_size, b, elem_size);
    memcpy((char *)buf + 2 * elem_size, c, elem_size);
    return buf;
}

// Write a float32 to a pointer at byte offset (for OpenGL vertex buffers)
void mix_poke_f32(void *ptr, int64_t offset, double val) {
    float f = (float)val;
    *(float *)((char *)ptr + offset) = f;
}

// Write a uint32 to a pointer at byte offset (for C struct fields)
void mix_poke_u32(void *ptr, int64_t offset, int64_t val) {
    *(uint32_t *)((char *)ptr + offset) = (uint32_t)val;
}

// Write a pointer to a pointer at byte offset (for C struct pointer fields)
void mix_poke_ptr(void *ptr, int64_t offset, void *val) {
    *(void **)((char *)ptr + offset) = val;
}

// Read a pointer from a pointer at byte offset
int64_t mix_peek_ptr(const void *ptr, int64_t offset) {
    return *(const int64_t *)((char *)ptr + offset);
}

typedef struct MixZoneHandle MixZoneHandle;

static MixZoneHandle *mix_current_zone(void);
void *zone_alloc(void *zone_ptr, int64_t size);
void *mix_list_new_in(void *zone_ptr);
void *mix_list_new_shape_in(void *zone_ptr, int64_t elem_size);
void *mix_map_new_in(void *zone_ptr);
void *mix_set_new_in(void *zone_ptr);
static MixZoneHandle *zone_handle_require_alive(void *zone_ptr);
static int64_t zone_handle_generation(void *zone_ptr);
static void zone_handle_check_generation(const MixZoneHandle *zone,
                                         int64_t generation,
                                         const char *kind);
static void *mix_realloc_copy_for_zone(MixZoneHandle *zone, void *old_ptr,
                                       size_t old_size, size_t new_size);

static void *mix_alloc_zero_for_zone(MixZoneHandle *zone, size_t size) {
    if (zone) return zone_alloc(zone, (int64_t)size);
    size_t alloc_size = size > 0 ? size : 1u;
    void *ptr = calloc(1, alloc_size);
    if (!ptr) mix_panic("out of memory");
    return ptr;
}

static void *mix_alloc_zero_current(size_t size) {
    return mix_alloc_zero_for_zone(mix_current_zone(), size);
}

static void *mix_realloc_copy_current(void *old_ptr, size_t old_size,
                                      size_t new_size) {
    return mix_realloc_copy_for_zone(mix_current_zone(), old_ptr, old_size,
                                     new_size);
}

static void *mix_realloc_copy_for_zone(MixZoneHandle *zone, void *old_ptr,
                                       size_t old_size, size_t new_size) {
    void *new_ptr = mix_alloc_zero_for_zone(zone, new_size);
    if (old_ptr && old_size > 0) {
        size_t copy_size = old_size < new_size ? old_size : new_size;
        memcpy(new_ptr, old_ptr, copy_size);
        if (!zone) free(old_ptr);
    }
    return new_ptr;
}

static char *mix_strdup_for_zone(MixZoneHandle *zone, const char *src) {
    size_t len = strlen(src) + 1;
    char *dst = mix_alloc_zero_for_zone(zone, len);
    memcpy(dst, src, len);
    return dst;
}

static char *mix_strdup_current(const char *src) {
    return mix_strdup_for_zone(mix_current_zone(), src ? src : "");
}

static char *mix_adopt_c_string(char *src) {
    if (!src) return mix_strdup_current("");
    char *dst = mix_strdup_current(src);
    free(src);
    return dst;
}

// ---- Lists ----
// Primitive lists store 8-byte values. Shape lists store inline element bytes,
// so `List[Sprite]` is compact and supports true element borrows.

typedef struct {
    int64_t len;
    int64_t cap;
    int64_t elem_size;
    uint8_t *data;
    int is_inline;
    MixZoneHandle *zone;
    int64_t generation;
} MixList;

static void mix_list_oob(const MixList *list, int64_t index) {
    fprintf(stderr, "panic: list index %" PRId64 " out of bounds (len %" PRId64 ")\n",
            index, list ? list->len : 0);
    exit(1);
}

static void mix_list_require_alive(const MixList *list) {
    if (!list) mix_panic("list is null");
    zone_handle_check_generation(list->zone, list->generation, "list");
}

static void mix_list_require_scalar(const MixList *list, const char *op) {
    mix_list_require_alive(list);
    if (list && list->is_inline) {
        fprintf(stderr, "panic: %s requires a scalar list\n", op);
        exit(1);
    }
}

static void mix_list_init(MixList *list, int64_t elem_size, int is_inline,
                          MixZoneHandle *zone) {
    if (!list) mix_panic("list is null");
    if (elem_size <= 0) mix_panic("list elem_size must be positive");
    if (zone) zone = zone_handle_require_alive(zone);
    list->len = 0;
    list->cap = 8;
    list->elem_size = elem_size;
    list->is_inline = is_inline;
    list->zone = zone;
    list->generation = zone ? zone_handle_generation(zone) : 0;
    list->data = mix_alloc_zero_for_zone(zone, (size_t)(elem_size * list->cap));
}

static void mix_list_ensure_cap(MixList *list, int64_t extra) {
    mix_list_require_alive(list);
    if (list->len + extra <= list->cap) return;
    int64_t old_cap = list->cap;
    while (list->len + extra > list->cap) {
        list->cap *= 2;
    }
    list->data = mix_realloc_copy_for_zone(
        list->zone, list->data,
        (size_t)(list->elem_size * old_cap),
        (size_t)(list->elem_size * list->cap));
}

static uint8_t *mix_list_elem_ptr_mut(MixList *list, int64_t index) {
    mix_list_require_alive(list);
    if (index < 0 || index >= list->len) mix_list_oob(list, index);
    return list->data + (index * list->elem_size);
}

static const uint8_t *mix_list_elem_ptr_const(const MixList *list, int64_t index) {
    mix_list_require_alive(list);
    if (index < 0 || index >= list->len) mix_list_oob(list, index);
    return list->data + (index * list->elem_size);
}

void *mix_list_new(void) {
    return mix_list_new_in(mix_current_zone());
}

void *mix_list_new_shape(int64_t elem_size) {
    return mix_list_new_shape_in(mix_current_zone(), elem_size);
}

void *mix_list_new_in(void *zone_ptr) {
    MixZoneHandle *zone = zone_ptr;
    MixList *list = calloc(1, sizeof(MixList));
    if (!list) mix_panic("out of memory");
    mix_list_init(list, 8, 0, zone);
    return list;
}

void *mix_list_new_shape_in(void *zone_ptr, int64_t elem_size) {
    MixZoneHandle *zone = zone_ptr;
    MixList *list = calloc(1, sizeof(MixList));
    if (!list) mix_panic("out of memory");
    mix_list_init(list, elem_size, 1, zone);
    return list;
}

int64_t mix_list_len(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_alive(list);
    return list->len;
}

void mix_list_push(void *list_ptr, int64_t val) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_push");
    mix_list_ensure_cap(list, 1);
    memcpy(list->data + (list->len * 8), &val, sizeof(int64_t));
    list->len++;
}

void mix_list_push_bytes(void *list_ptr, const void *src) {
    MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (!list || !list->is_inline) mix_panic("mix_list_push_bytes requires an inline list");
    mix_list_ensure_cap(list, 1);
    memcpy(list->data + (list->len * list->elem_size), src, (size_t)list->elem_size);
    list->len++;
}

int64_t mix_list_get(const void *list_ptr, int64_t index) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_get");
    int64_t val = 0;
    memcpy(&val, mix_list_elem_ptr_const(list, index), sizeof(int64_t));
    return val;
}

void *mix_list_ptr(void *list_ptr, int64_t index) {
    MixList *list = list_ptr;
    return mix_list_elem_ptr_mut(list, index);
}

void mix_list_set(void *list_ptr, int64_t index, int64_t val) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_set");
    memcpy(mix_list_elem_ptr_mut(list, index), &val, sizeof(int64_t));
}

void mix_list_set_bytes(void *list_ptr, int64_t index, const void *src) {
    MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (!list || !list->is_inline) mix_panic("mix_list_set_bytes requires an inline list");
    memcpy(mix_list_elem_ptr_mut(list, index), src, (size_t)list->elem_size);
}

void mix_list_pop_bytes(void *list_ptr, void *out) {
    MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (!list || !list->is_inline) mix_panic("mix_list_pop_bytes requires an inline list");
    if (list->len <= 0) mix_panic("pop from empty list");
    list->len--;
    memcpy(out, list->data + (list->len * list->elem_size), (size_t)list->elem_size);
}

void mix_list_insert_bytes(void *list_ptr, int64_t idx, const void *src) {
    MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (!list || !list->is_inline) mix_panic("mix_list_insert_bytes requires an inline list");
    if (idx < 0 || idx > list->len) {
        fprintf(stderr, "panic: list insert index %" PRId64 " out of bounds (len %" PRId64 ")\n",
                idx, list->len);
        exit(1);
    }
    mix_list_ensure_cap(list, 1);
    size_t tail = (size_t)((list->len - idx) * list->elem_size);
    memmove(list->data + ((idx + 1) * list->elem_size),
            list->data + (idx * list->elem_size),
            tail);
    memcpy(list->data + (idx * list->elem_size), src, (size_t)list->elem_size);
    list->len++;
}

int64_t mix_list_max_int64(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (list->len == 0) mix_panic("max of empty list");
    int64_t max = INT64_MIN;
    for (int64_t i = 0; i < list->len; i++) {
        int64_t val;
        memcpy(&val, mix_list_elem_ptr_const(list, i), sizeof(int64_t));
        if (val > max) max = val;
    }
    return max;
}

double mix_list_max_double(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (list->len == 0) mix_panic("max of empty list");
    double max = -1.0 / 0.0;
    for (int64_t i = 0; i < list->len; i++) {
        double val;
        memcpy(&val, mix_list_elem_ptr_const(list, i), sizeof(double));
        if (val > max) max = val;
    }
    return max;
}

int64_t mix_list_min_int64(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (list->len == 0) mix_panic("min of empty list");
    int64_t min = INT64_MAX;
    for (int64_t i = 0; i < list->len; i++) {
        int64_t val;
        memcpy(&val, mix_list_elem_ptr_const(list, i), sizeof(int64_t));
        if (val < min) min = val;
    }
    return min;
}

double mix_list_min_double(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (list->len == 0) mix_panic("min of empty list");
    double min = 1.0 / 0.0;
    for (int64_t i = 0; i < list->len; i++) {
        double val;
        memcpy(&val, mix_list_elem_ptr_const(list, i), sizeof(double));
        if (val < min) min = val;
    }
    return min;
}

void *mix_list_slice(const void *list_ptr, int64_t start, int64_t end, int32_t inclusive) {
    const MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (start < 0) start = list->len + start;
    if (end < 0) end = list->len + end;
    if (start < 0) start = 0;
    if (end > list->len) end = list->len;
    if (inclusive && end < list->len) end++;

    MixList *result = list->is_inline
        ? mix_list_new_shape_in(list->zone, list->elem_size)
        : mix_list_new_in(list->zone);
    for (int64_t i = start; i < end; i++) {
        if (list->is_inline) {
            mix_list_push_bytes(result, list->data + (i * list->elem_size));
        } else {
            mix_list_push(result, mix_list_get(list, i));
        }
    }
    return result;
}

void mix_print_list_int(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_print_list_int");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%" PRId64, mix_list_get(list, i));
    }
    printf("]\n");
}

void mix_print_list_str(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_print_list_str");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("\"%s\"", (const char *)(intptr_t)mix_list_get(list, i));
    }
    printf("]\n");
}

void mix_print_list_float(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_print_list_float");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        double val;
        int64_t bits = mix_list_get(list, i);
        memcpy(&val, &bits, sizeof(double));
        printf("%g", val);
    }
    printf("]\n");
}

void mix_print_list_bool(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_print_list_bool");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%s", mix_list_get(list, i) ? "true" : "false");
    }
    printf("]\n");
}

int64_t mix_list_pop(void *list_ptr) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_pop");
    if (list->len <= 0) mix_panic("pop from empty list");
    int64_t val = 0;
    list->len--;
    memcpy(&val, list->data + (list->len * 8), sizeof(int64_t));
    return val;
}

void mix_list_remove(void *list_ptr, int64_t idx) {
    MixList *list = list_ptr;
    mix_list_require_alive(list);
    if (idx < 0 || idx >= list->len) {
        fprintf(stderr, "panic: list index %" PRId64 " out of bounds (len %" PRId64 ")\n", idx, list->len);
        exit(1);
    }
    size_t tail = (size_t)((list->len - idx - 1) * list->elem_size);
    if (tail > 0) {
        memmove(list->data + (idx * list->elem_size),
                list->data + ((idx + 1) * list->elem_size),
                tail);
    }
    list->len--;
}

void mix_list_insert(void *list_ptr, int64_t idx, int64_t val) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_insert");
    if (idx < 0 || idx > list->len) {
        fprintf(stderr, "panic: list insert index %" PRId64 " out of bounds (len %" PRId64 ")\n", idx, list->len);
        exit(1);
    }
    mix_list_ensure_cap(list, 1);
    size_t tail = (size_t)((list->len - idx) * 8);
    memmove(list->data + ((idx + 1) * 8),
            list->data + (idx * 8),
            tail);
    memcpy(list->data + (idx * 8), &val, sizeof(int64_t));
    list->len++;
}

static int cmp_int64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

void mix_list_sort(void *list_ptr) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_sort");
    qsort(list->data, (size_t)list->len, sizeof(int64_t), cmp_int64);
}

static int cmp_str(const void *a, const void *b) {
    const char *sa = (const char *)*(const int64_t *)a;
    const char *sb = (const char *)*(const int64_t *)b;
    return strcmp(sa, sb);
}

void mix_list_sort_str(void *list_ptr) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_sort_str");
    qsort(list->data, (size_t)list->len, sizeof(int64_t), cmp_str);
}

static int cmp_float(const void *a, const void *b) {
    double da, db;
    memcpy(&da, a, sizeof(double));
    memcpy(&db, b, sizeof(double));
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void mix_list_sort_float(void *list_ptr) {
    MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_sort_float");
    qsort(list->data, (size_t)list->len, sizeof(int64_t), cmp_float);
}

void *mix_list_to_f32(void *list_ptr) {
    MixList *list = (MixList *)list_ptr;
    mix_list_require_scalar(list, "mix_list_to_f32");
    if (!list || list->len == 0) return NULL;
    float *buf = mix_alloc_zero_for_zone(list->zone, (size_t)list->len * sizeof(float));
    for (int64_t i = 0; i < list->len; i++) {
        double d;
        int64_t bits = mix_list_get(list, i);
        memcpy(&d, &bits, sizeof(double));
        buf[i] = (float)d;
    }
    return buf;
}

void mix_list_reverse(void *list_ptr) {
    MixList *list = list_ptr;
    if (!list) return;
    mix_list_require_alive(list);
    uint8_t *tmp = malloc((size_t)list->elem_size);
    if (!tmp) mix_panic("out of memory");
    for (int64_t i = 0, j = list->len - 1; i < j; i++, j--) {
        uint8_t *a = list->data + (i * list->elem_size);
        uint8_t *b = list->data + (j * list->elem_size);
        memcpy(tmp, a, (size_t)list->elem_size);
        memcpy(a, b, (size_t)list->elem_size);
        memcpy(b, tmp, (size_t)list->elem_size);
    }
    free(tmp);
}

int32_t mix_list_contains(const void *list_ptr, int64_t val) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_contains");
    for (int64_t i = 0; i < list->len; i++) {
        if (mix_list_get(list, i) == val) return 1;
    }
    return 0;
}

int64_t mix_list_index_of(const void *list_ptr, int64_t val) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_list_index_of");
    for (int64_t i = 0; i < list->len; i++) {
        if (mix_list_get(list, i) == val) return i;
    }
    return -1;
}

// ---- Explicit Zone Handles ----

typedef struct MixZoneHandle MixZoneHandle;

typedef struct {
    MixZoneHandle *zone;
    int64_t generation;
    void *payload;
} MixBoxHandle;

struct MixZoneHandle {
    char *name;
    void **allocs;
    int64_t alloc_count;
    int64_t alloc_cap;
    int64_t alloc_bytes;
    int64_t high_water;
    int64_t reset_count;
    int64_t generation;
    int destroyed;
};

static const char *zone_handle_name(const MixZoneHandle *zone) {
    return (zone && zone->name) ? zone->name : "<unnamed>";
}

static MixZoneHandle *zone_handle_require(void *zone_ptr) {
    MixZoneHandle *zone = zone_ptr;
    if (!zone) mix_panic("zone is null");
    return zone;
}

static MixZoneHandle *zone_handle_require_alive(void *zone_ptr) {
    MixZoneHandle *zone = zone_handle_require(zone_ptr);
    if (zone->destroyed) {
        mix_panic("zone was destroyed");
    }
    return zone;
}

static int64_t zone_handle_generation(void *zone_ptr) {
    return zone_handle_require_alive(zone_ptr)->generation;
}

static void zone_handle_check_generation(const MixZoneHandle *zone,
                                         int64_t generation,
                                         const char *kind) {
    if (!zone) return;
    if (zone->destroyed) {
        fprintf(stderr, "panic: %s points into destroyed zone '%s'\n",
                kind, zone_handle_name(zone));
        exit(1);
    }
    if (generation != zone->generation) {
        fprintf(stderr, "panic: %s points into reset zone '%s'\n",
                kind, zone_handle_name(zone));
        exit(1);
    }
}

static void zone_handle_track_alloc(MixZoneHandle *zone, void *ptr) {
    if (zone->alloc_count >= zone->alloc_cap) {
        int64_t new_cap = zone->alloc_cap ? zone->alloc_cap * 2 : 16;
        void **new_allocs = realloc(zone->allocs, sizeof(void *) * (size_t)new_cap);
        if (!new_allocs) mix_panic("out of memory");
        zone->allocs = new_allocs;
        zone->alloc_cap = new_cap;
    }
    zone->allocs[zone->alloc_count++] = ptr;
}

static void zone_handle_clear(MixZoneHandle *zone, int count_reset) {
    for (int64_t i = 0; i < zone->alloc_count; i++) {
        free(zone->allocs[i]);
    }
    zone->alloc_count = 0;
    zone->alloc_bytes = 0;
    if (count_reset) {
        zone->reset_count++;
        zone->generation++;
    }
}

void *zone_create(const char *name, int64_t capacity_hint) {
    (void)capacity_hint;
    MixZoneHandle *zone = calloc(1, sizeof(MixZoneHandle));
    if (!zone) mix_panic("out of memory");
    zone->alloc_cap = 16;
    zone->allocs = calloc((size_t)zone->alloc_cap, sizeof(void *));
    if (!zone->allocs) mix_panic("out of memory");
    if (name) {
        size_t name_len = strlen(name) + 1;
        zone->name = malloc(name_len);
        if (!zone->name) mix_panic("out of memory");
        memcpy(zone->name, name, name_len);
    }
    zone->generation = 1;
    return zone;
}

void zone_destroy(void *zone_ptr) {
    if (!zone_ptr) return;
    MixZoneHandle *zone = zone_handle_require_alive(zone_ptr);
    zone_handle_clear(zone, 0);
    zone->generation++;
    zone->destroyed = 1;
    free(zone->allocs);
    zone->allocs = NULL;
    zone->alloc_cap = 0;
}

void zone_reset(void *zone_ptr) {
    MixZoneHandle *zone = zone_handle_require_alive(zone_ptr);
    zone_handle_clear(zone, 1);
}

void *zone_alloc(void *zone_ptr, int64_t size) {
    MixZoneHandle *zone = zone_handle_require_alive(zone_ptr);
    if (size < 0) mix_panic("zone_alloc size must be non-negative");
    size_t alloc_size = size > 0 ? (size_t)size : 1u;
    void *ptr = calloc(1, alloc_size);
    if (!ptr) mix_panic("out of memory");
    zone_handle_track_alloc(zone, ptr);
    zone->alloc_bytes += size;
    if (zone->alloc_bytes > zone->high_water) {
        zone->high_water = zone->alloc_bytes;
    }
    return ptr;
}

void *mix_box_clone(void *zone_ptr, const void *src, int64_t size) {
    MixZoneHandle *zone = zone_handle_require_alive(zone_ptr);
    void *dst = zone_alloc(zone, size);
    size_t copy_size = size > 0 ? (size_t)size : 0u;
    if (copy_size > 0) {
        memcpy(dst, src, copy_size);
    }

    MixBoxHandle *box = malloc(sizeof(MixBoxHandle));
    if (!box) mix_panic("out of memory");
    box->zone = zone;
    box->generation = zone->generation;
    box->payload = dst;
    return box;
}

void *mix_box_check(void *box_ptr) {
    if (!box_ptr) return NULL;

    MixBoxHandle *box = box_ptr;
    MixZoneHandle *zone = zone_handle_require(box->zone);
    zone_handle_check_generation(zone, box->generation, "box");
    if (!box->payload) {
        mix_panic("invalid Box payload");
    }
    return box->payload;
}

int64_t _mix_zone_alloc_bytes(void *zone_ptr) {
    return zone_handle_require_alive(zone_ptr)->alloc_bytes;
}

int64_t _mix_zone_high_water(void *zone_ptr) {
    return zone_handle_require_alive(zone_ptr)->high_water;
}

int64_t _mix_zone_reset_count(void *zone_ptr) {
    return zone_handle_require_alive(zone_ptr)->reset_count;
}

// ---- Zones ----
// Anonymous scoped zone stack used by `zone ...` statements.

#define MAX_ZONE_DEPTH 32

// Thread-local zone state so `go` tasks don't race with the main thread.
// Each depth slot is stable storage, so stale Box/List/Map handles can still
// diagnose reset/escape bugs after a zone scope exits.
static _Thread_local MixZoneHandle zone_stack[MAX_ZONE_DEPTH];
static _Thread_local int zone_depth = 0;

static MixZoneHandle *mix_current_zone(void) {
    if (zone_depth <= 0) return NULL;
    return &zone_stack[zone_depth - 1];
}

void mix_zone_enter(void) {
    if (zone_depth >= MAX_ZONE_DEPTH) mix_panic("zone nesting too deep");
    MixZoneHandle *zone = &zone_stack[zone_depth++];
    zone->destroyed = 0;
    if (zone->generation == 0) zone->generation = 1;
    zone->alloc_count = 0;
    zone->alloc_bytes = 0;
    zone->high_water = 0;
    zone->reset_count = 0;
    zone->name = "<zone>";
}

void mix_zone_exit(void) {
    if (zone_depth <= 0) mix_panic("zone exit without enter");
    MixZoneHandle *zone = &zone_stack[--zone_depth];
    zone_handle_clear(zone, 0);
    zone->generation++;
    zone->destroyed = 1;
}

// Zone-aware allocation: if inside a zone, track for cleanup
void *mix_zone_alloc(int64_t size) {
    if (size < 0) mix_panic("mix_zone_alloc size must be non-negative");
    return mix_alloc_zero_current((size_t)size);
}

// ---- Optionals ----
// Layout: { int64_t has_value; int64_t value; }
// has_value: 0 = none, 1 = some

// ---- Results ----
// Layout: { int64_t is_ok; int64_t value; }
// is_ok: 1 = ok (value is the success value), 0 = error (value is error string ptr)

void *mix_result_ok(int64_t value) {
    int64_t *res = mix_alloc_zero_current(2 * sizeof(int64_t));
    if (!res) mix_panic("out of memory");
    res[0] = 1; // is_ok
    res[1] = value;
    return res;
}

void *mix_result_err(int64_t err_value) {
    int64_t *res = mix_alloc_zero_current(2 * sizeof(int64_t));
    if (!res) mix_panic("out of memory");
    res[0] = 0; // is_err
    res[1] = err_value;
    return res;
}

int64_t mix_result_is_ok(const void *res_ptr) {
    const int64_t *res = res_ptr;
    return res[0];
}

int64_t mix_result_unwrap(const void *res_ptr) {
    const int64_t *res = res_ptr;
    if (!res[0]) {
        // Error — the value is a string pointer
        const char *msg = (const char *)res[1];
        if (msg) {
            fprintf(stderr, "panic: unwrap of error result: %s\n", msg);
        } else {
            fprintf(stderr, "panic: unwrap of error result\n");
        }
        exit(1);
    }
    return res[1];
}

int64_t mix_result_unwrap_err(const void *res_ptr) {
    const int64_t *res = res_ptr;
    if (res[0]) {
        mix_panic("unwrap_err of ok result");
    }
    return res[1];
}

void *mix_optional_some(int64_t value) {
    int64_t *opt = mix_alloc_zero_current(2 * sizeof(int64_t));
    if (!opt) mix_panic("out of memory");
    opt[0] = 1; // has_value
    opt[1] = value;
    return opt;
}

void *mix_optional_none(void) {
    int64_t *opt = mix_alloc_zero_current(2 * sizeof(int64_t));
    if (!opt) mix_panic("out of memory");
    opt[0] = 0; // no value
    opt[1] = 0;
    return opt;
}

int64_t mix_optional_has(const void *opt_ptr) {
    const int64_t *opt = opt_ptr;
    return opt[0];
}

int64_t mix_optional_get(const void *opt_ptr) {
    const int64_t *opt = opt_ptr;
    if (!opt[0]) mix_panic("unwrap of none optional");
    return opt[1];
}

// ---- String methods ----

int64_t mix_str_len(const char *s) {
    if (!s) return 0;
    return (int64_t)strlen(s);
}

char *mix_str_upper(const char *s) {
    if (!s) s = "";
    int64_t len = strlen(s);
    char *result = mix_alloc_zero_current((size_t)len + 1);
    if (!result) mix_panic("out of memory");
    for (int64_t i = 0; i < len; i++) result[i] = toupper((unsigned char)s[i]);
    result[len] = '\0';
    return result;
}

char *mix_str_lower(const char *s) {
    if (!s) s = "";
    int64_t len = strlen(s);
    char *result = mix_alloc_zero_current((size_t)len + 1);
    if (!result) mix_panic("out of memory");
    for (int64_t i = 0; i < len; i++) result[i] = tolower((unsigned char)s[i]);
    result[len] = '\0';
    return result;
}

char *mix_str_trim(const char *s) {
    if (!s) s = "";
    const char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (*start == '\0') {
        char *result = mix_alloc_zero_current(1);
        result[0] = '\0';
        return result;
    }
    const char *end = s + strlen(s) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    int64_t len = end - start + 1;
    char *result = mix_alloc_zero_current((size_t)len + 1);
    if (!result) mix_panic("out of memory");
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

void *mix_str_split(const char *s, const char *delim) {
    MixList *list = mix_list_new();
    if (!s) return list;
    if (!delim) delim = "";
    int64_t dlen = strlen(delim);
    if (dlen == 0) {
        // Split into individual characters
        const char *p = s;
        while (*p) {
            char *ch = mix_alloc_zero_current(2);
            ch[0] = *p;
            ch[1] = '\0';
            mix_list_push(list, (int64_t)ch);
            p++;
        }
        return list;
    }
    const char *p = s;
    while (*p) {
        const char *found = strstr(p, delim);
        if (!found) {
            char *part = mix_strdup_current(p);
            strcpy(part, p);
            mix_list_push(list, (int64_t)part);
            break;
        }
        int64_t len = found - p;
        char *part = mix_alloc_zero_current((size_t)len + 1);
        memcpy(part, p, len);
        part[len] = '\0';
        mix_list_push(list, (int64_t)part);
        p = found + dlen;
    }
    return list;
}

int32_t mix_str_contains(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    return strstr(s, needle) != NULL ? 1 : 0;
}

int32_t mix_str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    return strncmp(s, prefix, strlen(prefix)) == 0 ? 1 : 0;
}

char *mix_str_replace(const char *s, const char *old_str, const char *new_str) {
    if (!s) s = "";
    if (!old_str) old_str = "";
    if (!new_str) new_str = "";
    int64_t slen = strlen(s);
    int64_t olen = strlen(old_str);
    int64_t nlen = strlen(new_str);
    if (olen == 0) {
        char *result = mix_alloc_zero_current((size_t)slen + 1);
        strcpy(result, s);
        return result;
    }
    // Count occurrences
    int count = 0;
    const char *p = s;
    while ((p = strstr(p, old_str))) { count++; p += olen; }
    int64_t rlen = slen + count * (nlen - olen);
    char *result = mix_alloc_zero_current((size_t)rlen + 1);
    if (!result) mix_panic("out of memory");
    char *dst = result;
    p = s;
    while (*p) {
        const char *found = strstr(p, old_str);
        if (!found) { strcpy(dst, p); break; }
        memcpy(dst, p, found - p);
        dst += found - p;
        memcpy(dst, new_str, nlen);
        dst += nlen;
        p = found + olen;
    }
    result[rlen] = '\0';
    return result;
}

char *mix_str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    int64_t alen = strlen(a);
    int64_t blen = strlen(b);
    char *result = mix_alloc_zero_current((size_t)(alen + blen) + 1);
    if (!result) mix_panic("out of memory");
    memcpy(result, a, alen);
    memcpy(result + alen, b, blen);
    result[alen + blen] = '\0';
    return result;
}

int32_t mix_str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    int64_t slen = strlen(s);
    int64_t sflen = strlen(suffix);
    if (sflen > slen) return 0;
    return strcmp(s + slen - sflen, suffix) == 0 ? 1 : 0;
}

char *mix_str_char_at(const char *s, int64_t idx) {
    if (!s) mix_panic("char_at on null string");
    int64_t len = strlen(s);
    if (idx < 0 || idx >= len) {
        mix_panic("string index out of bounds");
    }
    char *result = mix_alloc_zero_current(2);
    if (!result) mix_panic("out of memory");
    result[0] = s[idx];
    result[1] = '\0';
    return result;
}

char *mix_str_join(const void *list_ptr, const char *sep) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_str_join");
    if (list->len == 0) {
        char *result = mix_alloc_zero_current(1);
        result[0] = '\0';
        return result;
    }
    int64_t seplen = strlen(sep);
    // Calculate total length
    int64_t total = 0;
    for (int64_t i = 0; i < list->len; i++) {
        total += strlen((const char *)(intptr_t)mix_list_get(list, i));
        if (i < list->len - 1) total += seplen;
    }
    char *result = mix_alloc_zero_current((size_t)total + 1);
    if (!result) mix_panic("out of memory");
    char *dst = result;
    for (int64_t i = 0; i < list->len; i++) {
        const char *s = (const char *)(intptr_t)mix_list_get(list, i);
        int64_t slen = strlen(s);
        memcpy(dst, s, slen);
        dst += slen;
        if (i < list->len - 1) {
            memcpy(dst, sep, seplen);
            dst += seplen;
        }
    }
    *dst = '\0';
    return result;
}

char *mix_str_reverse(const char *s) {
    if (!s) s = "";
    int64_t len = strlen(s);
    char *result = mix_alloc_zero_current((size_t)len + 1);
    if (!result) mix_panic("out of memory");
    for (int64_t i = 0; i < len; i++) {
        result[i] = s[len - 1 - i];
    }
    result[len] = '\0';
    return result;
}

static int cmp_char(const void *a, const void *b) {
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

char *mix_str_sort(const char *s) {
    if (!s) s = "";
    int64_t len = strlen(s);
    char *result = mix_alloc_zero_current((size_t)len + 1);
    if (!result) mix_panic("out of memory");
    memcpy(result, s, len + 1);
    qsort(result, len, 1, cmp_char);
    return result;
}

int64_t mix_str_count(const char *s, const char *sub) {
    if (!s || !sub) return 0;
    int64_t count = 0;
    int64_t sublen = strlen(sub);
    if (sublen == 0) return 0;
    const char *p = s;
    while ((p = strstr(p, sub)) != NULL) {
        count++;
        p += sublen;
    }
    return count;
}

char *mix_str_slice(const char *s, int64_t start, int64_t end) {
    if (!s) s = "";
    int64_t len = strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) {
        char *r = mix_alloc_zero_current(1);
        r[0] = '\0';
        return r;
    }
    int64_t slen = end - start;
    char *r = mix_alloc_zero_current((size_t)slen + 1);
    if (!r) mix_panic("out of memory");
    memcpy(r, s + start, slen);
    r[slen] = '\0';
    return r;
}

char *mix_str_repeat(const char *s, int64_t n) {
    if (!s) s = "";
    if (n <= 0) {
        char *r = mix_alloc_zero_current(1);
        r[0] = '\0';
        return r;
    }
    int64_t slen = strlen(s);
    if (slen > 0 && n > INT64_MAX / slen) {
        mix_panic("string repeat: size overflow");
    }
    int64_t total = slen * n;
    char *r = mix_alloc_zero_current((size_t)total + 1);
    if (!r) mix_panic("out of memory");
    for (int64_t i = 0; i < n; i++) {
        memcpy(r + i * slen, s, slen);
    }
    r[total] = '\0';
    return r;
}

int64_t mix_str_index_of(const char *s, const char *sub) {
    if (!s || !sub) return -1;
    const char *found = strstr(s, sub);
    if (!found) return -1;
    return (int64_t)(found - s);
}

char *mix_to_string_int(int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    return mix_strdup_current(buf);
}

char *mix_to_string_float(double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        strcat(buf, ".0");
    }
    return mix_strdup_current(buf);
}

// String-building variants of the write_* functions, used when a value
// appears inside a string-interpolation expression so we need to splice
// it into the resulting string instead of writing to stdout.
char *mix_to_string_bool(int val) {
    return mix_strdup_current(val ? "true" : "false");
}

char *mix_to_string_list_int(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_to_string_list_int");
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) fprintf(f, ", ");
        fprintf(f, "%" PRId64, mix_list_get(list, i));
    }
    fprintf(f, "]");
    fclose(f);
    return mix_adopt_c_string(buf);
}

char *mix_to_string_list_str(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_to_string_list_str");
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) fprintf(f, ", ");
        fprintf(f, "\"%s\"", (const char *)(intptr_t)mix_list_get(list, i));
    }
    fprintf(f, "]");
    fclose(f);
    return mix_adopt_c_string(buf);
}

char *mix_to_string_list_float(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_to_string_list_float");
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) fprintf(f, ", ");
        // List slots store the int64 bit pattern of a double; cast back.
        double d;
        int64_t bits = mix_list_get(list, i);
        memcpy(&d, &bits, sizeof(d));
        fprintf(f, "%g", d);
    }
    fprintf(f, "]");
    fclose(f);
    return mix_adopt_c_string(buf);
}

char *mix_to_string_list_bool(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_to_string_list_bool");
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) fprintf(f, ", ");
        fprintf(f, "%s", mix_list_get(list, i) ? "true" : "false");
    }
    fprintf(f, "]");
    fclose(f);
    return mix_adopt_c_string(buf);
}

int64_t mix_to_int(double val) {
    return (int64_t)val;
}

double mix_to_float(int64_t val) {
    return (double)val;
}

int64_t mix_parse_int(const char *s) {
    return strtoll(s, NULL, 10);
}

double mix_parse_float(const char *s) {
    return strtod(s, NULL);
}

// ---- File I/O ----

int64_t mix_file_open(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    return (int64_t)f;  // 0 on failure
}

char *mix_file_read(int64_t handle) {
    FILE *f = (FILE *)handle;
    if (!f) return "";
    size_t cap = 4096, len = 0;
    char *buf = mix_alloc_zero_current(cap);
    if (!buf) mix_panic("out of memory");
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = mix_realloc_copy_current(buf, cap / 2, cap);
            if (!buf) mix_panic("out of memory");
        }
    }
    buf[len] = '\0';
    return buf;
}

void mix_file_write(int64_t handle, const char *data) {
    FILE *f = (FILE *)handle;
    if (!f) return;
    fputs(data, f);
}

void mix_file_close(int64_t handle) {
    FILE *f = (FILE *)handle;
    if (f) fclose(f);
}

char *mix_file_read_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = mix_alloc_zero_current((size_t)size + 1);
    if (!buf) { fclose(f); mix_panic("out of memory"); }
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

int32_t mix_file_write_all(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(content, f);
    fclose(f);
    return 1;
}

// ---- Math ----

double mix_math_sqrt(double x)  { return sqrt(x); }
double mix_math_abs(double x)   { return fabs(x); }
double mix_math_pow(double x, double y) { return pow(x, y); }
double mix_math_sin(double x)   { return sin(x); }
double mix_math_cos(double x)   { return cos(x); }
double mix_math_tan(double x)   { return tan(x); }
double mix_math_log(double x)   { return log(x); }
double mix_math_floor(double x) { return floor(x); }
double mix_math_ceil(double x)  { return ceil(x); }
double mix_math_round(double x) { return round(x); }
double mix_math_min(double x, double y) { return x < y ? x : y; }
double mix_math_max(double x, double y) { return x > y ? x : y; }

// ---- Maps ----
// Hash map with open addressing (djb2 hash, 0.75 load factor)

typedef struct {
    char *key;          // NULL = empty slot
    int64_t value;
    uint8_t occupied;
} MixMapEntry;

typedef struct {
    MixMapEntry *entries;
    int64_t len;
    int64_t cap;
    MixZoneHandle *zone;
    int64_t generation;
} MixMap;

static uint64_t hash_str(const char *s) {
    uint64_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static void mix_map_require_alive(const MixMap *map) {
    if (!map) mix_panic("map is null");
    zone_handle_check_generation(map->zone, map->generation, "map");
}

static void mix_map_init(MixMap *map, MixZoneHandle *zone) {
    if (!map) mix_panic("map is null");
    if (zone) zone = zone_handle_require_alive(zone);
    map->len = 0;
    map->cap = 16;
    map->zone = zone;
    map->generation = zone ? zone_handle_generation(zone) : 0;
    map->entries = mix_alloc_zero_for_zone(zone, (size_t)map->cap * sizeof(MixMapEntry));
    if (!map->entries) mix_panic("out of memory");
}

static void mix_map_place_entry(MixMap *map, char *key, int64_t value) {
    uint64_t h = hash_str(key) % (uint64_t)map->cap;
    while (map->entries[h].occupied) {
        h = (h + 1) % (uint64_t)map->cap;
    }
    map->entries[h].key = key;
    map->entries[h].value = value;
    map->entries[h].occupied = 1;
    map->len++;
}

void *mix_map_new(void) {
    return mix_map_new_in(mix_current_zone());
}

void *mix_map_new_in(void *zone_ptr) {
    MixZoneHandle *zone = zone_ptr;
    MixMap *map = calloc(1, sizeof(MixMap));
    if (!map) mix_panic("out of memory");
    mix_map_init(map, zone);
    return map;
}

int64_t mix_map_len(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    return map->len;
}

static void map_grow(MixMap *map) {
    mix_map_require_alive(map);
    int64_t new_cap = map->cap * 2;
    MixMapEntry *old_entries = map->entries;
    int64_t old_cap = map->cap;
    MixMapEntry *new_entries = mix_alloc_zero_for_zone(
        map->zone, (size_t)new_cap * sizeof(MixMapEntry));
    if (!new_entries) mix_panic("out of memory");
    map->entries = new_entries;
    map->cap = new_cap;
    map->len = 0;
    for (int64_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied) {
            mix_map_place_entry(map, old_entries[i].key, old_entries[i].value);
        }
    }
    if (!map->zone) free(old_entries);
}

void mix_map_set(void *map_ptr, const char *key, int64_t val) {
    MixMap *map = map_ptr;
    mix_map_require_alive(map);
    // Check load factor
    if (map->len * 4 >= map->cap * 3) {
        map_grow(map);
    }
    uint64_t h = hash_str(key) % map->cap;
    while (map->entries[h].occupied) {
        if (strcmp(map->entries[h].key, key) == 0) {
            // Update existing
            map->entries[h].value = val;
            return;
        }
        h = (h + 1) % map->cap;
    }
    // Insert new
    mix_map_place_entry(map, mix_strdup_for_zone(map->zone, key), val);
}

int64_t mix_map_get(const void *map_ptr, const char *key) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    uint64_t h = hash_str(key) % map->cap;
    for (int64_t i = 0; i < map->cap; i++) {
        int64_t idx = (h + i) % map->cap;
        if (!map->entries[idx].occupied) {
            fprintf(stderr, "panic: map key '%s' not found\n", key);
            exit(1);
        }
        if (strcmp(map->entries[idx].key, key) == 0) {
            return map->entries[idx].value;
        }
    }
    fprintf(stderr, "panic: map key '%s' not found\n", key);
    exit(1);
}

int32_t mix_map_has(const void *map_ptr, const char *key) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    uint64_t h = hash_str(key) % map->cap;
    for (int64_t i = 0; i < map->cap; i++) {
        int64_t idx = (h + i) % map->cap;
        if (!map->entries[idx].occupied) return 0;
        if (strcmp(map->entries[idx].key, key) == 0) return 1;
    }
    return 0;
}

void mix_map_remove(void *map_ptr, const char *key) {
    MixMap *map = map_ptr;
    mix_map_require_alive(map);
    uint64_t h = hash_str(key) % map->cap;
    for (int64_t i = 0; i < map->cap; i++) {
        int64_t idx = (h + i) % map->cap;
        if (!map->entries[idx].occupied) return;
        if (strcmp(map->entries[idx].key, key) == 0) {
            if (!map->zone) free(map->entries[idx].key);
            map->entries[idx].key = NULL;
            map->entries[idx].occupied = 0;
            map->len--;
            // Rehash subsequent entries (linear probing)
            int64_t j = (idx + 1) % map->cap;
            while (map->entries[j].occupied) {
                MixMapEntry e = map->entries[j];
                map->entries[j].occupied = 0;
                map->entries[j].key = NULL;
                map->len--;
                mix_map_place_entry(map, e.key, e.value);
                j = (j + 1) % map->cap;
            }
            return;
        }
    }
}

void *mix_map_keys(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    MixList *list = mix_list_new_in(map->zone);
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            mix_list_push(list, (int64_t)map->entries[i].key);
        }
    }
    return list;
}

void *mix_map_values(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    MixList *list = mix_list_new_in(map->zone);
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            mix_list_push(list, map->entries[i].value);
        }
    }
    return list;
}

void mix_print_map(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    printf("{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("\"%s\": %" PRId64, map->entries[i].key, map->entries[i].value);
            first = 0;
        }
    }
    printf("}\n");
}

char *mix_to_string_map(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) fprintf(f, ", ");
            fprintf(f, "\"%s\": %" PRId64, map->entries[i].key, map->entries[i].value);
            first = 0;
        }
    }
    fprintf(f, "}");
    fclose(f);
    return mix_adopt_c_string(buf);
}

char *mix_to_string_map_str(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) fprintf(f, ", ");
            fprintf(f, "\"%s\": \"%s\"", map->entries[i].key, (const char *)map->entries[i].value);
            first = 0;
        }
    }
    fprintf(f, "}");
    fclose(f);
    return mix_adopt_c_string(buf);
}

char *mix_to_string_set(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "set{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) fprintf(f, ", ");
            fprintf(f, "\"%s\"", map->entries[i].key);
            first = 0;
        }
    }
    fprintf(f, "}");
    fclose(f);
    return mix_adopt_c_string(buf);
}

char *mix_to_string_set_int(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    fprintf(f, "set{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) fprintf(f, ", ");
            fprintf(f, "%" PRId64, map->entries[i].value);
            first = 0;
        }
    }
    fprintf(f, "}");
    fclose(f);
    return mix_adopt_c_string(buf);
}

void mix_print_map_str(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    printf("{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("\"%s\": \"%s\"", map->entries[i].key, (const char *)map->entries[i].value);
            first = 0;
        }
    }
    printf("}\n");
}

// ---- Sets (backed by MixMap: keys = elements, values = ignored) ----

void *mix_set_new(void) {
    return mix_set_new_in(mix_current_zone());
}

void *mix_set_new_in(void *zone_ptr) {
    return mix_map_new_in(zone_ptr);
}

int64_t mix_set_len(const void *set_ptr) {
    return mix_map_len(set_ptr);
}

void mix_set_add(void *set_ptr, const char *elem) {
    mix_map_set(set_ptr, elem, 1);
}

void mix_set_remove(void *set_ptr, const char *elem) {
    mix_map_remove(set_ptr, elem);
}

int32_t mix_set_has(const void *set_ptr, const char *elem) {
    return mix_map_has(set_ptr, elem);
}

void *mix_set_values(const void *set_ptr) {
    return mix_map_keys(set_ptr);
}

void *mix_set_union(const void *a_ptr, const void *b_ptr) {
    const MixMap *a = a_ptr;
    const MixMap *b = b_ptr;
    mix_map_require_alive(a);
    mix_map_require_alive(b);
    void *result = mix_set_new_in(a->zone);
    for (int64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].occupied) {
            mix_map_set(result, a->entries[i].key, 1);
        }
    }
    for (int64_t i = 0; i < b->cap; i++) {
        if (b->entries[i].occupied) {
            mix_map_set(result, b->entries[i].key, 1);
        }
    }
    return result;
}

void *mix_set_intersect(const void *a_ptr, const void *b_ptr) {
    const MixMap *a = a_ptr;
    mix_map_require_alive(a);
    void *result = mix_set_new_in(a->zone);
    for (int64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].occupied && mix_map_has(b_ptr, a->entries[i].key)) {
            mix_map_set(result, a->entries[i].key, 1);
        }
    }
    return result;
}

void *mix_set_diff(const void *a_ptr, const void *b_ptr) {
    const MixMap *a = a_ptr;
    mix_map_require_alive(a);
    void *result = mix_set_new_in(a->zone);
    for (int64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].occupied && !mix_map_has(b_ptr, a->entries[i].key)) {
            mix_map_set(result, a->entries[i].key, 1);
        }
    }
    return result;
}

void mix_print_set(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    printf("set{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("\"%s\"", map->entries[i].key);
            first = 0;
        }
    }
    printf("}\n");
}

void *mix_set_from_list(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_set_from_list");
    void *set = mix_set_new_in(list->zone);
    for (int64_t i = 0; i < list->len; i++) {
        mix_set_add(set, (const char *)(intptr_t)mix_list_get(list, i));
    }
    return set;
}

// Int-element set operations (convert int <-> string internally)
void mix_set_add_int(void *set_ptr, int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    mix_map_set(set_ptr, buf, val);
}

void mix_set_remove_int(void *set_ptr, int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    mix_map_remove(set_ptr, buf);
}

int32_t mix_set_has_int(const void *set_ptr, int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    return mix_map_has(set_ptr, buf);
}

void *mix_set_values_int(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    MixList *list = mix_list_new_in(map->zone);
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            mix_list_push(list, map->entries[i].value);
        }
    }
    return list;
}

void mix_print_set_int(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    printf("set{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("%" PRId64, map->entries[i].value);
            first = 0;
        }
    }
    printf("}\n");
}

// Write functions for collections (no newline — for string interpolation)
void mix_write_list_int(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_write_list_int");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%"PRId64, mix_list_get(list, i));
    }
    printf("]");
}

void mix_write_list_str(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_write_list_str");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("\"%s\"", (const char *)(intptr_t)mix_list_get(list, i));
    }
    printf("]");
}

void mix_write_list_float(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_write_list_float");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        double val;
        int64_t bits = mix_list_get(list, i);
        memcpy(&val, &bits, sizeof(double));
        printf("%g", val);
    }
    printf("]");
}

void mix_write_list_bool(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_write_list_bool");
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%s", mix_list_get(list, i) ? "true" : "false");
    }
    printf("]");
}

void mix_write_map(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    printf("{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("\"%s\": %" PRId64, map->entries[i].key, map->entries[i].value);
            first = 0;
        }
    }
    printf("}");
}

void mix_write_map_str(const void *map_ptr) {
    const MixMap *map = map_ptr;
    mix_map_require_alive(map);
    printf("{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("\"%s\": \"%s\"", map->entries[i].key, (const char *)map->entries[i].value);
            first = 0;
        }
    }
    printf("}");
}

void mix_write_set(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    printf("set{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("\"%s\"", map->entries[i].key);
            first = 0;
        }
    }
    printf("}");
}

void mix_write_set_int(const void *set_ptr) {
    const MixMap *map = set_ptr;
    mix_map_require_alive(map);
    printf("set{");
    int first = 1;
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            if (!first) printf(", ");
            printf("%" PRId64, map->entries[i].value);
            first = 0;
        }
    }
    printf("}");
}

void *mix_set_from_list_int(const void *list_ptr) {
    const MixList *list = list_ptr;
    mix_list_require_scalar(list, "mix_set_from_list_int");
    void *set = mix_set_new_in(list->zone);
    for (int64_t i = 0; i < list->len; i++) {
        mix_set_add_int(set, mix_list_get(list, i));
    }
    return set;
}

// ---- Shared (mutex-wrapped values) ----

typedef struct {
    pthread_mutex_t mutex;
    int64_t value;
} MixShared;

void *mix_shared_new(int64_t init_val) {
    MixShared *s = malloc(sizeof(MixShared));
    if (!s) mix_panic("out of memory");
    pthread_mutex_init(&s->mutex, NULL);
    s->value = init_val;
    return s;
}

int64_t mix_shared_read(void *shared_ptr) {
    MixShared *s = shared_ptr;
    pthread_mutex_lock(&s->mutex);
    int64_t val = s->value;
    pthread_mutex_unlock(&s->mutex);
    return val;
}

void mix_shared_update(void *shared_ptr, void *fn_ptr) {
    MixShared *s = shared_ptr;
    int64_t (*fn)(int64_t) = fn_ptr;
    pthread_mutex_lock(&s->mutex);
    s->value = fn(s->value);
    pthread_mutex_unlock(&s->mutex);
}

// ---- Tasks (go/wait) ----

typedef int64_t (*MixFn0)(void);
typedef int64_t (*MixFn1)(int64_t);
typedef int64_t (*MixFn2)(int64_t, int64_t);
typedef int64_t (*MixFn3)(int64_t, int64_t, int64_t);
typedef int64_t (*MixFn4)(int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*MixFn5)(int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*MixFn6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*MixFn7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*MixFn8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

typedef struct {
    pthread_t thread;
    void *fn_ptr;
    int64_t *args;
    int64_t arg_count;
    int64_t result;
} MixTask;

static void *task_trampoline(void *arg) {
    MixTask *task = arg;
    int64_t *a = task->args;
    switch (task->arg_count) {
        case 0: task->result = ((MixFn0)task->fn_ptr)(); break;
        case 1: task->result = ((MixFn1)task->fn_ptr)(a[0]); break;
        case 2: task->result = ((MixFn2)task->fn_ptr)(a[0], a[1]); break;
        case 3: task->result = ((MixFn3)task->fn_ptr)(a[0], a[1], a[2]); break;
        case 4: task->result = ((MixFn4)task->fn_ptr)(a[0], a[1], a[2], a[3]); break;
        case 5: task->result = ((MixFn5)task->fn_ptr)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: task->result = ((MixFn6)task->fn_ptr)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: task->result = ((MixFn7)task->fn_ptr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: task->result = ((MixFn8)task->fn_ptr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        default: mix_panic("too many arguments for go task"); break;
    }
    return NULL;
}

void *mix_task_spawn(void *fn_ptr, void *packed_args, int64_t count) {
    MixTask *task = malloc(sizeof(MixTask));
    if (!task) mix_panic("out of memory");
    task->fn_ptr = fn_ptr;
    task->args = packed_args;
    task->arg_count = count;
    task->result = 0;
    int err = pthread_create(&task->thread, NULL, task_trampoline, task);
    if (err != 0) {
        fprintf(stderr, "panic: failed to spawn task: %s\n", strerror(err));
        free(task);
        exit(1);
    }
    return task;
}

int64_t mix_task_wait(void *task_ptr) {
    MixTask *task = task_ptr;
    pthread_join(task->thread, NULL);
    int64_t result = task->result;
    if (task->args) free(task->args);
    free(task);
    return result;
}

// ---- OS Builtins ----

// argc/argv capture for args()
static int mix_argc = 0;
static char **mix_argv = NULL;

void mix_set_args(int32_t argc, char **argv) {
    mix_argc = argc;
    mix_argv = argv;
}

void *mix_args(void) {
    MixList *list = (MixList *)mix_list_new();
    for (int i = 0; i < mix_argc; i++) {
        mix_list_push(list, (int64_t)mix_argv[i]);
    }
    return list;
}

#ifndef __wasi__
int64_t mix_shell(const char *cmd) {
    return (int64_t)system(cmd);
}

char *mix_shell_output(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        char *empty = mix_alloc_zero_current(1);
        empty[0] = '\0';
        return empty;
    }
    size_t cap = 1024;
    size_t len = 0;
    char *buf = mix_alloc_zero_current(cap);
    if (!buf) mix_panic("out of memory");
    while (1) {
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        if (n == 0) break;
        len += n;
        if (len + 1 >= cap) {
            size_t old_cap = cap;
            cap *= 2;
            buf = mix_realloc_copy_current(buf, old_cap, cap);
            if (!buf) mix_panic("out of memory");
        }
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}
#else
int64_t mix_shell(const char *cmd) { (void)cmd; return -1; }
char *mix_shell_output(const char *cmd) { (void)cmd; char *empty = mix_alloc_zero_current(1); empty[0] = '\0'; return empty; }
#endif

int64_t mix_file_exists(const char *path) {
    return access(path, F_OK) == 0 ? 1 : 0;
}

void *mix_list_dir(const char *path) {
    MixList *list = (MixList *)mix_list_new();
    DIR *dir = opendir(path);
    if (!dir) return list;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char *name = mix_strdup_current(entry->d_name);
        strcpy(name, entry->d_name);
        mix_list_push(list, (int64_t)name);
    }
    closedir(dir);
    return list;
}

char *mix_env(const char *name) {
    char *val = getenv(name);
    if (val) {
        return mix_strdup_current(val);
    }
    char *empty = mix_alloc_zero_current(1);
    empty[0] = '\0';
    return empty;
}

void mix_exit(int64_t code) {
    exit((int)code);
}

char *mix_getcwd(void) {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) {
        return mix_strdup_current(buf);
    }
    char *dot = mix_alloc_zero_current(2);
    dot[0] = '.';
    dot[1] = '\0';
    return dot;
}

// Recursive mkdir (like mkdir -p)
static int mkpath(const char *path, mode_t mode) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

int64_t mix_mkdir(const char *path) {
    return mkpath(path, 0755) == 0 ? 1 : 0;
}

// ---- Project build ----

void mix_project_build(void *self) {
    char *name          = *(char **)((char *)self + 0);
    char *entry         = *(char **)((char *)self + 8);
    char *output        = *(char **)((char *)self + 16);
    MixList *libs       = *(MixList **)((char *)self + 24);
    MixList *lib_paths  = *(MixList **)((char *)self + 32);
    MixList *inc_paths  = *(MixList **)((char *)self + 40);
    MixList *flags_list = *(MixList **)((char *)self + 48);
    int32_t debug       = *(int32_t *)((char *)self + 56);

    // Derive output from entry if empty
    char derived[256];
    if (!output || output[0] == '\0') {
        // Strip path and .mix extension from entry
        const char *slash = strrchr(entry, '/');
        const char *base = slash ? slash + 1 : entry;
        strncpy(derived, base, sizeof(derived) - 1);
        derived[sizeof(derived) - 1] = '\0';
        char *dot = strrchr(derived, '.');
        if (dot && strcmp(dot, ".mix") == 0) *dot = '\0';
        output = derived;
    }

    // Ensure output directory exists
    {
        char outdir[4096];
        strncpy(outdir, output, sizeof(outdir) - 1);
        outdir[sizeof(outdir) - 1] = '\0';
        char *last_slash = strrchr(outdir, '/');
        if (last_slash) {
            *last_slash = '\0';
            mkpath(outdir, 0755);
        }
    }

    // Find the mix compiler
    const char *mix_bin = getenv("MIX_COMPILER");
    if (!mix_bin || mix_bin[0] == '\0') mix_bin = "mix";

    // Build command
    char cmd[4096];
    int off = snprintf(cmd, sizeof(cmd), "%s build %s -o %s", mix_bin, entry, output);

    // Append -l flags from libs list
    if (libs) {
        for (int64_t i = 0; i < libs->len; i++) {
            off += snprintf(cmd + off, sizeof(cmd) - off, " -l%s",
                            (char *)(intptr_t)mix_list_get(libs, i));
        }
    }

    // Append -L flags from lib_paths list
    if (lib_paths) {
        for (int64_t i = 0; i < lib_paths->len; i++) {
            off += snprintf(cmd + off, sizeof(cmd) - off, " -L%s",
                            (char *)(intptr_t)mix_list_get(lib_paths, i));
        }
    }

    // Append --debug if debug != 0
    if (debug) {
        off += snprintf(cmd + off, sizeof(cmd) - off, " --debug");
    }

    // Append extra flags
    if (flags_list) {
        for (int64_t i = 0; i < flags_list->len; i++) {
            off += snprintf(cmd + off, sizeof(cmd) - off, " %s",
                            (char *)(intptr_t)mix_list_get(flags_list, i));
        }
    }

    // Forward include_paths as CPPFLAGS for use c header resolution
    if (inc_paths && inc_paths->len > 0) {
        char cppflags[4096] = "";
        int cpoff = 0;
        const char *existing = getenv("CPPFLAGS");
        if (existing && existing[0]) {
            cpoff = snprintf(cppflags, sizeof(cppflags), "%s", existing);
        }
        for (int64_t i = 0; i < inc_paths->len; i++) {
            cpoff += snprintf(cppflags + cpoff, sizeof(cppflags) - cpoff,
                              " -I%s", (char *)(intptr_t)mix_list_get(inc_paths, i));
        }
        setenv("CPPFLAGS", cppflags, 1);
    }

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "mix: build failed for '%s' (exit %d)\n", name, ret);
        exit(1);
    }
}

// --- Unicode ord/chr ---

// Decode the first UTF-8 character from s and return its Unicode code point.
int64_t mix_ord(const char *s) {
    if (!s || !*s) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && s[1]) {
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    return c; // fallback: return byte value
}

// Encode a Unicode code point as a UTF-8 string. Returns a heap-allocated string.
const char *mix_chr(int64_t cp) {
    char *buf = mix_alloc_zero_current(5); // max 4 UTF-8 bytes + null
    if (cp < 0x80) {
        buf[0] = (char)cp;
        buf[1] = '\0';
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        buf[2] = '\0';
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        buf[3] = '\0';
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        buf[4] = '\0';
    }
    return buf;
}

#include <sys/time.h>

void mix_random_seed(int64_t seed) {
    srand((unsigned int)seed);
}

int64_t mix_random_int(void) {
    /* rand() is only 31-bit on some platforms; combine two calls. */
    int64_t hi = (int64_t)rand();
    int64_t lo = (int64_t)rand();
    return (hi << 31) ^ lo;
}

double mix_random_float(void) {
    return (double)rand() / (double)RAND_MAX;
}

int64_t mix_time_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

/* int -> hex string (no "0x" prefix). Caller frees. */
char *mix_int_to_hex(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIx64, (uint64_t)n);
    return mix_strdup_current(buf);
}

/* int -> binary string. Caller frees. */
char *mix_int_to_bin(int64_t n) {
    char buf[65];
    uint64_t u = (uint64_t)n;
    int i = 63;
    buf[64] = '\0';
    if (u == 0) {
        buf[63] = '0';
        i = 62;
    } else {
        while (u) {
            buf[i--] = (u & 1) ? '1' : '0';
            u >>= 1;
        }
    }
    return mix_strdup_current(buf + i + 1);
}
