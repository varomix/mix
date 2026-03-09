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

void mix_print_float(double val) {
    printf("%g\n", val);
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
    printf("%g", val);
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

// ---- Lists ----
// List layout: { int64_t len; int64_t cap; int64_t elem_size; void *data; }
// All elements stored as 8-byte (int64_t) values for simplicity.
// Floats stored via union punning.

typedef struct {
    int64_t len;
    int64_t cap;
    int64_t *data;
} MixList;

void *mix_list_new(void) {
    MixList *list = malloc(sizeof(MixList));
    if (!list) mix_panic("out of memory");
    list->len = 0;
    list->cap = 8;
    list->data = malloc(sizeof(int64_t) * 8);
    if (!list->data) mix_panic("out of memory");
    return list;
}

int64_t mix_list_len(const void *list_ptr) {
    const MixList *list = list_ptr;
    return list->len;
}

void mix_list_push(void *list_ptr, int64_t val) {
    MixList *list = list_ptr;
    if (list->len >= list->cap) {
        list->cap *= 2;
        list->data = realloc(list->data, sizeof(int64_t) * list->cap);
        if (!list->data) mix_panic("out of memory");
    }
    list->data[list->len++] = val;
}

int64_t mix_list_get(const void *list_ptr, int64_t index) {
    const MixList *list = list_ptr;
    if (index < 0 || index >= list->len) {
        fprintf(stderr, "panic: list index %"PRId64" out of bounds (len %"PRId64")\n", index, list->len);
        exit(1);
    }
    return list->data[index];
}

void mix_list_set(void *list_ptr, int64_t index, int64_t val) {
    MixList *list = list_ptr;
    if (index < 0 || index >= list->len) {
        fprintf(stderr, "panic: list index %"PRId64" out of bounds (len %"PRId64")\n", index, list->len);
        exit(1);
    }
    list->data[index] = val;
}

void *mix_list_slice(const void *list_ptr, int64_t start, int64_t end, int32_t inclusive) {
    const MixList *list = list_ptr;
    // Handle negative indices
    if (start < 0) start = list->len + start;
    if (end < 0) end = list->len + end;
    if (start < 0) start = 0;
    if (end > list->len) end = list->len;
    if (inclusive && end < list->len) end++;

    MixList *result = mix_list_new();
    for (int64_t i = start; i < end; i++) {
        mix_list_push(result, list->data[i]);
    }
    return result;
}

// Print a list of ints
void mix_print_list_int(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%"PRId64, list->data[i]);
    }
    printf("]\n");
}

// Print a list of strings
void mix_print_list_str(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("\"%s\"", (const char *)list->data[i]);
    }
    printf("]\n");
}

// Print a list of floats
void mix_print_list_float(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        double val;
        memcpy(&val, &list->data[i], sizeof(double));
        printf("%g", val);
    }
    printf("]\n");
}

// Print a list of bools
void mix_print_list_bool(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%s", list->data[i] ? "true" : "false");
    }
    printf("]\n");
}

int64_t mix_list_pop(void *list_ptr) {
    MixList *list = list_ptr;
    if (list->len <= 0) mix_panic("pop from empty list");
    return list->data[--list->len];
}

void mix_list_remove(void *list_ptr, int64_t idx) {
    MixList *list = list_ptr;
    if (idx < 0 || idx >= list->len) {
        fprintf(stderr, "panic: list index %" PRId64 " out of bounds (len %" PRId64 ")\n", idx, list->len);
        exit(1);
    }
    for (int64_t i = idx; i < list->len - 1; i++) {
        list->data[i] = list->data[i + 1];
    }
    list->len--;
}

void mix_list_insert(void *list_ptr, int64_t idx, int64_t val) {
    MixList *list = list_ptr;
    if (idx < 0 || idx > list->len) {
        fprintf(stderr, "panic: list insert index %" PRId64 " out of bounds (len %" PRId64 ")\n", idx, list->len);
        exit(1);
    }
    // Ensure capacity
    if (list->len >= list->cap) {
        list->cap *= 2;
        list->data = realloc(list->data, sizeof(int64_t) * list->cap);
        if (!list->data) mix_panic("out of memory");
    }
    for (int64_t i = list->len; i > idx; i--) {
        list->data[i] = list->data[i - 1];
    }
    list->data[idx] = val;
    list->len++;
}

static int cmp_int64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

void mix_list_sort(void *list_ptr) {
    MixList *list = list_ptr;
    qsort(list->data, list->len, sizeof(int64_t), cmp_int64);
}

static int cmp_str(const void *a, const void *b) {
    const char *sa = (const char *)*(const int64_t *)a;
    const char *sb = (const char *)*(const int64_t *)b;
    return strcmp(sa, sb);
}

void mix_list_sort_str(void *list_ptr) {
    MixList *list = list_ptr;
    qsort(list->data, list->len, sizeof(int64_t), cmp_str);
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
    qsort(list->data, list->len, sizeof(int64_t), cmp_float);
}

void mix_list_reverse(void *list_ptr) {
    MixList *list = list_ptr;
    for (int64_t i = 0, j = list->len - 1; i < j; i++, j--) {
        int64_t tmp = list->data[i];
        list->data[i] = list->data[j];
        list->data[j] = tmp;
    }
}

int32_t mix_list_contains(const void *list_ptr, int64_t val) {
    const MixList *list = list_ptr;
    for (int64_t i = 0; i < list->len; i++) {
        if (list->data[i] == val) return 1;
    }
    return 0;
}

int64_t mix_list_index_of(const void *list_ptr, int64_t val) {
    const MixList *list = list_ptr;
    for (int64_t i = 0; i < list->len; i++) {
        if (list->data[i] == val) return i;
    }
    return -1;
}

// ---- Zones ----
// Simple zone allocator: track allocations in a stack, free on zone exit

#define MAX_ZONE_DEPTH 32
#define MAX_ZONE_ALLOCS 1024

// Thread-local zone state so 'go' tasks don't race with the main thread
static _Thread_local void *zone_allocs[MAX_ZONE_ALLOCS];
static _Thread_local int64_t zone_alloc_count = 0;
static _Thread_local int64_t zone_watermarks[MAX_ZONE_DEPTH];
static _Thread_local int zone_depth = 0;

void mix_zone_enter(void) {
    if (zone_depth >= MAX_ZONE_DEPTH) mix_panic("zone nesting too deep");
    zone_watermarks[zone_depth++] = zone_alloc_count;
}

void mix_zone_exit(void) {
    if (zone_depth <= 0) mix_panic("zone exit without enter");
    int64_t watermark = zone_watermarks[--zone_depth];
    // Free all allocations since watermark
    while (zone_alloc_count > watermark) {
        free(zone_allocs[--zone_alloc_count]);
    }
}

// Zone-aware allocation: if inside a zone, track for cleanup
void *mix_zone_alloc(int64_t size) {
    void *ptr = calloc(1, (size_t)size);
    if (!ptr) mix_panic("out of memory");
    if (zone_depth > 0) {
        if (zone_alloc_count >= MAX_ZONE_ALLOCS) {
            mix_panic("too many zone allocations (max 1024)");
        }
        zone_allocs[zone_alloc_count++] = ptr;
    }
    return ptr;
}

// ---- Optionals ----
// Layout: { int64_t has_value; int64_t value; }
// has_value: 0 = none, 1 = some

// ---- Results ----
// Layout: { int64_t is_ok; int64_t value; }
// is_ok: 1 = ok (value is the success value), 0 = error (value is error string ptr)

void *mix_result_ok(int64_t value) {
    int64_t *res = calloc(2, sizeof(int64_t));
    if (!res) mix_panic("out of memory");
    res[0] = 1; // is_ok
    res[1] = value;
    return res;
}

void *mix_result_err(int64_t err_value) {
    int64_t *res = calloc(2, sizeof(int64_t));
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
    int64_t *opt = calloc(2, sizeof(int64_t));
    if (!opt) mix_panic("out of memory");
    opt[0] = 1; // has_value
    opt[1] = value;
    return opt;
}

void *mix_optional_none(void) {
    int64_t *opt = calloc(2, sizeof(int64_t));
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
    char *result = malloc(len + 1);
    if (!result) mix_panic("out of memory");
    for (int64_t i = 0; i < len; i++) result[i] = toupper((unsigned char)s[i]);
    result[len] = '\0';
    return result;
}

char *mix_str_lower(const char *s) {
    if (!s) s = "";
    int64_t len = strlen(s);
    char *result = malloc(len + 1);
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
        char *result = malloc(1);
        result[0] = '\0';
        return result;
    }
    const char *end = s + strlen(s) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    int64_t len = end - start + 1;
    char *result = malloc(len + 1);
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
            char *ch = malloc(2);
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
            char *part = malloc(strlen(p) + 1);
            strcpy(part, p);
            mix_list_push(list, (int64_t)part);
            break;
        }
        int64_t len = found - p;
        char *part = malloc(len + 1);
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
        char *result = malloc(slen + 1);
        strcpy(result, s);
        return result;
    }
    // Count occurrences
    int count = 0;
    const char *p = s;
    while ((p = strstr(p, old_str))) { count++; p += olen; }
    int64_t rlen = slen + count * (nlen - olen);
    char *result = malloc(rlen + 1);
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
    char *result = malloc(alen + blen + 1);
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
    char *result = malloc(2);
    if (!result) mix_panic("out of memory");
    result[0] = s[idx];
    result[1] = '\0';
    return result;
}

char *mix_str_join(const void *list_ptr, const char *sep) {
    const MixList *list = list_ptr;
    if (list->len == 0) {
        char *result = malloc(1);
        result[0] = '\0';
        return result;
    }
    int64_t seplen = strlen(sep);
    // Calculate total length
    int64_t total = 0;
    for (int64_t i = 0; i < list->len; i++) {
        total += strlen((const char *)list->data[i]);
        if (i < list->len - 1) total += seplen;
    }
    char *result = malloc(total + 1);
    if (!result) mix_panic("out of memory");
    char *dst = result;
    for (int64_t i = 0; i < list->len; i++) {
        const char *s = (const char *)list->data[i];
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
    char *result = malloc(len + 1);
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
    char *result = malloc(len + 1);
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
        char *r = malloc(1);
        r[0] = '\0';
        return r;
    }
    int64_t slen = end - start;
    char *r = malloc(slen + 1);
    if (!r) mix_panic("out of memory");
    memcpy(r, s + start, slen);
    r[slen] = '\0';
    return r;
}

char *mix_str_repeat(const char *s, int64_t n) {
    if (!s) s = "";
    if (n <= 0) {
        char *r = malloc(1);
        r[0] = '\0';
        return r;
    }
    int64_t slen = strlen(s);
    if (slen > 0 && n > INT64_MAX / slen) {
        mix_panic("string repeat: size overflow");
    }
    int64_t total = slen * n;
    char *r = malloc(total + 1);
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
    char *result = malloc(strlen(buf) + 1);
    if (!result) mix_panic("out of memory");
    strcpy(result, buf);
    return result;
}

char *mix_to_string_float(double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    char *result = malloc(strlen(buf) + 1);
    if (!result) mix_panic("out of memory");
    strcpy(result, buf);
    return result;
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
    char *buf = malloc(cap);
    if (!buf) mix_panic("out of memory");
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
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
    char *buf = malloc(size + 1);
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
} MixMap;

static uint64_t hash_str(const char *s) {
    uint64_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

void *mix_map_new(void) {
    MixMap *map = malloc(sizeof(MixMap));
    if (!map) mix_panic("out of memory");
    map->len = 0;
    map->cap = 16;
    map->entries = calloc(16, sizeof(MixMapEntry));
    if (!map->entries) mix_panic("out of memory");
    return map;
}

int64_t mix_map_len(const void *map_ptr) {
    const MixMap *map = map_ptr;
    return map->len;
}

static void map_grow(MixMap *map) {
    int64_t new_cap = map->cap * 2;
    MixMapEntry *new_entries = calloc(new_cap, sizeof(MixMapEntry));
    if (!new_entries) mix_panic("out of memory");
    // Rehash all existing entries
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            uint64_t h = hash_str(map->entries[i].key) % new_cap;
            while (new_entries[h].occupied) {
                h = (h + 1) % new_cap;
            }
            new_entries[h] = map->entries[i];
        }
    }
    free(map->entries);
    map->entries = new_entries;
    map->cap = new_cap;
}

void mix_map_set(void *map_ptr, const char *key, int64_t val) {
    MixMap *map = map_ptr;
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
    map->entries[h].key = strdup(key);
    map->entries[h].value = val;
    map->entries[h].occupied = 1;
    map->len++;
}

int64_t mix_map_get(const void *map_ptr, const char *key) {
    const MixMap *map = map_ptr;
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
    uint64_t h = hash_str(key) % map->cap;
    for (int64_t i = 0; i < map->cap; i++) {
        int64_t idx = (h + i) % map->cap;
        if (!map->entries[idx].occupied) return;
        if (strcmp(map->entries[idx].key, key) == 0) {
            free(map->entries[idx].key);
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
                mix_map_set(map, e.key, e.value);
                free(e.key);
                j = (j + 1) % map->cap;
            }
            return;
        }
    }
}

void *mix_map_keys(const void *map_ptr) {
    const MixMap *map = map_ptr;
    MixList *list = mix_list_new();
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            mix_list_push(list, (int64_t)map->entries[i].key);
        }
    }
    return list;
}

void *mix_map_values(const void *map_ptr) {
    const MixMap *map = map_ptr;
    MixList *list = mix_list_new();
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            mix_list_push(list, map->entries[i].value);
        }
    }
    return list;
}

void mix_print_map(const void *map_ptr) {
    const MixMap *map = map_ptr;
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

void mix_print_map_str(const void *map_ptr) {
    const MixMap *map = map_ptr;
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
    return mix_map_new();
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
    void *result = mix_set_new();
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
    void *result = mix_set_new();
    for (int64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].occupied && mix_map_has(b_ptr, a->entries[i].key)) {
            mix_map_set(result, a->entries[i].key, 1);
        }
    }
    return result;
}

void *mix_set_diff(const void *a_ptr, const void *b_ptr) {
    const MixMap *a = a_ptr;
    void *result = mix_set_new();
    for (int64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].occupied && !mix_map_has(b_ptr, a->entries[i].key)) {
            mix_map_set(result, a->entries[i].key, 1);
        }
    }
    return result;
}

void mix_print_set(const void *set_ptr) {
    const MixMap *map = set_ptr;
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
    void *set = mix_set_new();
    for (int64_t i = 0; i < list->len; i++) {
        mix_set_add(set, (const char *)list->data[i]);
    }
    return set;
}

// Int-element set operations (convert int <-> string internally)
void mix_set_add_int(void *set_ptr, int64_t val) {
    char *s = mix_to_string_int(val);
    mix_map_set(set_ptr, s, val);
    free(s);
}

void mix_set_remove_int(void *set_ptr, int64_t val) {
    char *s = mix_to_string_int(val);
    mix_map_remove(set_ptr, s);
    free(s);
}

int32_t mix_set_has_int(const void *set_ptr, int64_t val) {
    char *s = mix_to_string_int(val);
    int32_t result = mix_map_has(set_ptr, s);
    free(s);
    return result;
}

void *mix_set_values_int(const void *set_ptr) {
    const MixMap *map = set_ptr;
    MixList *list = mix_list_new();
    for (int64_t i = 0; i < map->cap; i++) {
        if (map->entries[i].occupied) {
            mix_list_push(list, map->entries[i].value);
        }
    }
    return list;
}

void mix_print_set_int(const void *set_ptr) {
    const MixMap *map = set_ptr;
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
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%"PRId64, list->data[i]);
    }
    printf("]");
}

void mix_write_list_str(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("\"%s\"", (const char *)list->data[i]);
    }
    printf("]");
}

void mix_write_list_float(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        double val;
        memcpy(&val, &list->data[i], sizeof(double));
        printf("%g", val);
    }
    printf("]");
}

void mix_write_list_bool(const void *list_ptr) {
    const MixList *list = list_ptr;
    printf("[");
    for (int64_t i = 0; i < list->len; i++) {
        if (i > 0) printf(", ");
        printf("%s", list->data[i] ? "true" : "false");
    }
    printf("]");
}

void mix_write_map(const void *map_ptr) {
    const MixMap *map = map_ptr;
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
    void *set = mix_set_new();
    for (int64_t i = 0; i < list->len; i++) {
        mix_set_add_int(set, list->data[i]);
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

int64_t mix_shell(const char *cmd) {
    return (int64_t)system(cmd);
}

char *mix_shell_output(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        char *empty = malloc(1);
        empty[0] = '\0';
        return empty;
    }
    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) mix_panic("out of memory");
    while (1) {
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        if (n == 0) break;
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) mix_panic("out of memory");
        }
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

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
        char *name = malloc(strlen(entry->d_name) + 1);
        strcpy(name, entry->d_name);
        mix_list_push(list, (int64_t)name);
    }
    closedir(dir);
    return list;
}

char *mix_env(const char *name) {
    char *val = getenv(name);
    if (val) {
        char *copy = malloc(strlen(val) + 1);
        strcpy(copy, val);
        return copy;
    }
    char *empty = malloc(1);
    empty[0] = '\0';
    return empty;
}

void mix_exit(int64_t code) {
    exit((int)code);
}

char *mix_getcwd(void) {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) {
        char *result = malloc(strlen(buf) + 1);
        strcpy(result, buf);
        return result;
    }
    char *dot = malloc(2);
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
            off += snprintf(cmd + off, sizeof(cmd) - off, " -l%s", (char *)libs->data[i]);
        }
    }

    // Append -L flags from lib_paths list
    if (lib_paths) {
        for (int64_t i = 0; i < lib_paths->len; i++) {
            off += snprintf(cmd + off, sizeof(cmd) - off, " -L%s", (char *)lib_paths->data[i]);
        }
    }

    // Append --debug if debug != 0
    if (debug) {
        off += snprintf(cmd + off, sizeof(cmd) - off, " --debug");
    }

    // Append extra flags
    if (flags_list) {
        for (int64_t i = 0; i < flags_list->len; i++) {
            off += snprintf(cmd + off, sizeof(cmd) - off, " %s", (char *)flags_list->data[i]);
        }
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
    char *buf = mix_alloc(5); // max 4 UTF-8 bytes + null
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
