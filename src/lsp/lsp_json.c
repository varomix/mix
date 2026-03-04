#include "lsp_json.h"
#include <inttypes.h>

// --- JSON Parser ---

typedef struct {
    const char *src;
    int pos;
    int len;
    Arena *arena;
} JsonParser;

static void jp_skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else break;
    }
}

static char jp_peek(JsonParser *p) {
    jp_skip_ws(p);
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static char jp_next(JsonParser *p) {
    jp_skip_ws(p);
    return p->pos < p->len ? p->src[p->pos++] : '\0';
}

static JsonValue *jp_alloc(Arena *a, JsonKind kind) {
    JsonValue *v = arena_alloc(a, sizeof(JsonValue));
    memset(v, 0, sizeof(JsonValue));
    v->kind = kind;
    return v;
}

static JsonValue *jp_parse_value(JsonParser *p);

static JsonValue *jp_parse_string(JsonParser *p) {
    if (jp_next(p) != '"') return NULL;
    int start = p->pos;
    // First pass: find end and check for escapes
    bool has_escapes = false;
    while (p->pos < p->len && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') { has_escapes = true; p->pos++; }
        p->pos++;
    }
    int end = p->pos;
    p->pos++; // skip closing "

    JsonValue *v = jp_alloc(p->arena, JSON_STRING);
    if (!has_escapes) {
        v->string.length = end - start;
        v->string.data = arena_strndup(p->arena, p->src + start, v->string.length);
    } else {
        // Decode escapes
        char *buf = arena_alloc(p->arena, end - start + 1);
        int j = 0;
        for (int i = start; i < end; i++) {
            if (p->src[i] == '\\' && i + 1 < end) {
                i++;
                switch (p->src[i]) {
                    case '"': buf[j++] = '"'; break;
                    case '\\': buf[j++] = '\\'; break;
                    case '/': buf[j++] = '/'; break;
                    case 'n': buf[j++] = '\n'; break;
                    case 'r': buf[j++] = '\r'; break;
                    case 't': buf[j++] = '\t'; break;
                    default: buf[j++] = p->src[i]; break;
                }
            } else {
                buf[j++] = p->src[i];
            }
        }
        buf[j] = '\0';
        v->string.data = buf;
        v->string.length = j;
    }
    return v;
}

static JsonValue *jp_parse_number(JsonParser *p) {
    int start = p->pos;
    bool neg = false;
    if (p->src[p->pos] == '-') { neg = true; p->pos++; }
    int64_t val = 0;
    while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
        val = val * 10 + (p->src[p->pos] - '0');
        p->pos++;
    }
    (void)start;
    JsonValue *v = jp_alloc(p->arena, JSON_INT);
    v->integer = neg ? -val : val;
    return v;
}

static JsonValue *jp_parse_object(JsonParser *p) {
    p->pos++; // skip {
    JsonValue *v = jp_alloc(p->arena, JSON_OBJECT);
    v->object.cap = 16;
    v->object.pairs = arena_alloc(p->arena, sizeof(JsonPair) * v->object.cap);
    v->object.count = 0;

    while (jp_peek(p) != '}' && p->pos < p->len) {
        if (v->object.count > 0) {
            if (jp_peek(p) == ',') jp_next(p);
        }
        JsonValue *key = jp_parse_string(p);
        if (!key) return NULL;
        if (jp_next(p) != ':') return NULL;
        JsonValue *val = jp_parse_value(p);
        if (!val) return NULL;

        if (v->object.count >= v->object.cap) {
            int new_cap = v->object.cap * 2;
            JsonPair *new_pairs = arena_alloc(p->arena, sizeof(JsonPair) * new_cap);
            memcpy(new_pairs, v->object.pairs, sizeof(JsonPair) * v->object.count);
            v->object.pairs = new_pairs;
            v->object.cap = new_cap;
        }
        v->object.pairs[v->object.count].key = key->string.data;
        v->object.pairs[v->object.count].value = val;
        v->object.count++;
    }
    jp_next(p); // skip }
    return v;
}

static JsonValue *jp_parse_array(JsonParser *p) {
    p->pos++; // skip [
    JsonValue *v = jp_alloc(p->arena, JSON_ARRAY);
    v->array.cap = 16;
    v->array.items = arena_alloc(p->arena, sizeof(JsonValue *) * v->array.cap);
    v->array.count = 0;

    while (jp_peek(p) != ']' && p->pos < p->len) {
        if (v->array.count > 0) {
            if (jp_peek(p) == ',') jp_next(p);
        }
        JsonValue *item = jp_parse_value(p);
        if (!item) return NULL;

        if (v->array.count >= v->array.cap) {
            int new_cap = v->array.cap * 2;
            JsonValue **new_items = arena_alloc(p->arena, sizeof(JsonValue *) * new_cap);
            memcpy(new_items, v->array.items, sizeof(JsonValue *) * v->array.count);
            v->array.items = new_items;
            v->array.cap = new_cap;
        }
        v->array.items[v->array.count++] = item;
    }
    jp_next(p); // skip ]
    return v;
}

static JsonValue *jp_parse_value(JsonParser *p) {
    char c = jp_peek(p);
    if (c == '"') return jp_parse_string(p);
    if (c == '{') return jp_parse_object(p);
    if (c == '[') return jp_parse_array(p);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);
    if (c == 't') { p->pos += 4; JsonValue *v = jp_alloc(p->arena, JSON_BOOL); v->boolean = true; return v; }
    if (c == 'f') { p->pos += 5; JsonValue *v = jp_alloc(p->arena, JSON_BOOL); v->boolean = false; return v; }
    if (c == 'n') { p->pos += 4; return jp_alloc(p->arena, JSON_NULL); }
    return NULL;
}

JsonValue *json_parse(const char *input, int length, Arena *arena) {
    JsonParser p = { .src = input, .pos = 0, .len = length, .arena = arena };
    return jp_parse_value(&p);
}

JsonValue *json_get(JsonValue *obj, const char *key) {
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->object.count; i++) {
        if (strcmp(obj->object.pairs[i].key, key) == 0)
            return obj->object.pairs[i].value;
    }
    return NULL;
}

const char *json_get_string(JsonValue *obj, const char *key) {
    JsonValue *v = json_get(obj, key);
    return (v && v->kind == JSON_STRING) ? v->string.data : NULL;
}

int64_t json_get_int(JsonValue *obj, const char *key) {
    JsonValue *v = json_get(obj, key);
    return (v && v->kind == JSON_INT) ? v->integer : 0;
}

bool json_get_bool(JsonValue *obj, const char *key) {
    JsonValue *v = json_get(obj, key);
    return (v && v->kind == JSON_BOOL) ? v->boolean : false;
}

// --- JSON Writer ---

static void jw_grow(JsonWriter *w, int need) {
    if (w->len + need >= w->cap) {
        w->cap = (w->cap + need) * 2;
        w->buf = realloc(w->buf, w->cap);
    }
}

static void jw_put(JsonWriter *w, const char *s, int n) {
    jw_grow(w, n);
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_putc(JsonWriter *w, char c) {
    jw_grow(w, 1);
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

static void jw_comma(JsonWriter *w) {
    if (w->need_comma) jw_putc(w, ',');
    w->need_comma = false;
}

void jw_init(JsonWriter *w) {
    w->cap = 256;
    w->buf = malloc(w->cap);
    w->buf[0] = '\0';
    w->len = 0;
    w->need_comma = false;
    w->depth = 0;
}

void jw_free(JsonWriter *w) {
    free(w->buf);
    w->buf = NULL;
    w->len = w->cap = 0;
}

void jw_object_start(JsonWriter *w) {
    jw_comma(w);
    jw_putc(w, '{');
    w->need_comma = false;
    w->depth++;
}

void jw_object_end(JsonWriter *w) {
    jw_putc(w, '}');
    w->need_comma = true;
    w->depth--;
}

void jw_array_start(JsonWriter *w) {
    jw_comma(w);
    jw_putc(w, '[');
    w->need_comma = false;
    w->depth++;
}

void jw_array_end(JsonWriter *w) {
    jw_putc(w, ']');
    w->need_comma = true;
    w->depth--;
}

void jw_key(JsonWriter *w, const char *key) {
    jw_comma(w);
    jw_putc(w, '"');
    jw_put(w, key, (int)strlen(key));
    jw_putc(w, '"');
    jw_putc(w, ':');
    w->need_comma = false;
}

void jw_string(JsonWriter *w, const char *val) {
    jw_comma(w);
    jw_putc(w, '"');
    // Escape special characters
    for (const char *p = val; *p; p++) {
        switch (*p) {
            case '"':  jw_put(w, "\\\"", 2); break;
            case '\\': jw_put(w, "\\\\", 2); break;
            case '\n': jw_put(w, "\\n", 2); break;
            case '\r': jw_put(w, "\\r", 2); break;
            case '\t': jw_put(w, "\\t", 2); break;
            default:   jw_putc(w, *p); break;
        }
    }
    jw_putc(w, '"');
    w->need_comma = true;
}

void jw_int(JsonWriter *w, int64_t val) {
    jw_comma(w);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRId64, val);
    jw_put(w, buf, n);
    w->need_comma = true;
}

void jw_bool(JsonWriter *w, bool val) {
    jw_comma(w);
    if (val) jw_put(w, "true", 4);
    else jw_put(w, "false", 5);
    w->need_comma = true;
}

void jw_null(JsonWriter *w) {
    jw_comma(w);
    jw_put(w, "null", 4);
    w->need_comma = true;
}

void jw_raw(JsonWriter *w, const char *json) {
    jw_comma(w);
    jw_put(w, json, (int)strlen(json));
    w->need_comma = true;
}
