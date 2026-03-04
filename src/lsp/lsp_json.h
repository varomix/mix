#ifndef LSP_JSON_H
#define LSP_JSON_H

#include "../mix.h"
#include "../arena.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_INT,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonKind;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;
    JsonValue *value;
} JsonPair;

struct JsonValue {
    JsonKind kind;
    union {
        bool boolean;
        int64_t integer;
        struct { char *data; int length; } string;
        struct { JsonValue **items; int count; int cap; } array;
        struct { JsonPair *pairs; int count; int cap; } object;
    };
};

// Parse a JSON string. Returns NULL on error.
JsonValue *json_parse(const char *input, int length, Arena *arena);

// Object accessor helpers
JsonValue *json_get(JsonValue *obj, const char *key);
const char *json_get_string(JsonValue *obj, const char *key);
int64_t json_get_int(JsonValue *obj, const char *key);
bool json_get_bool(JsonValue *obj, const char *key);

// JSON writer — builds JSON strings into a growable buffer
typedef struct {
    char *buf;
    int len;
    int cap;
    bool need_comma;
    int depth;
} JsonWriter;

void jw_init(JsonWriter *w);
void jw_free(JsonWriter *w);
void jw_object_start(JsonWriter *w);
void jw_object_end(JsonWriter *w);
void jw_array_start(JsonWriter *w);
void jw_array_end(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);
void jw_string(JsonWriter *w, const char *val);
void jw_int(JsonWriter *w, int64_t val);
void jw_bool(JsonWriter *w, bool val);
void jw_null(JsonWriter *w);
void jw_raw(JsonWriter *w, const char *json);

#endif // LSP_JSON_H
