#ifndef TYPES_H
#define TYPES_H

#include "mix.h"

typedef enum {
    TYPE_INT, TYPE_FLOAT, TYPE_BOOL, TYPE_BYTE, TYPE_STR, TYPE_VOID,
    TYPE_INT8, TYPE_INT16, TYPE_INT32, TYPE_INT64,
    TYPE_UINT8, TYPE_UINT16, TYPE_UINT32, TYPE_UINT64,
    TYPE_FLOAT32, TYPE_FLOAT64,
    TYPE_PTR,
    TYPE_LIST,
    TYPE_MAP,
    TYPE_OPTIONAL,
    TYPE_FUNC,
    TYPE_SHAPE,
    TYPE_NAMED,
    TYPE_INFER,
    TYPE_SHARED,
    TYPE_TASK,
    TYPE_RESULT,
    TYPE_SET,
    TYPE_GENERIC,
} TypeKind;

typedef struct ShapeFieldInfo {
    char *name;
    struct MixType *type;
    int offset;
    int size;
} ShapeFieldInfo;

typedef struct ShapeVariant {
    char *name;
    int tag;
    ShapeFieldInfo *fields;
    int field_count;
    int data_size;       // size of variant data (all fields)
} ShapeVariant;

struct MixType {
    TypeKind kind;
    union {
        struct { struct MixType *base; } ptr;
        struct { struct MixType *elem_type; } list;
        struct { struct MixType *key_type; struct MixType *val_type; } map;
        struct { struct MixType *inner; } optional;
        struct {
            struct MixType *return_type;
            struct MixType **param_types;
            int param_count;
            bool is_variadic;
        } func;
        struct {
            char *name;
            ShapeFieldInfo *fields;
            int field_count;
            int total_size;
            int alignment;
            // Tagged union variants (NULL if plain struct)
            ShapeVariant *variants;
            int variant_count;
            bool is_tagged_union;
        } shape;
        struct { char *name; } named;
        struct { struct MixType *inner; } shared;
        struct { struct MixType *result_type; } task;
        struct { struct MixType *ok_type; } result;
        struct { struct MixType *elem_type; } set;
    };
};

const char *type_kind_name(TypeKind kind);
const char *type_to_qbe(MixType *type);  // "w", "l", "s", "d" — register type
const char *type_to_qbe_mem(MixType *type);  // "b", "h", "w", "l", "s", "d" — memory store/load type
const char *type_to_qbe_load(MixType *type); // "ub", "uh", "w", "l", "s", "d" — load type (unsigned extend)
bool type_is_integer(MixType *type);
bool type_is_float(MixType *type);
bool type_is_numeric(MixType *type);
int type_size(MixType *type);       // size in bytes
int type_alignment(MixType *type);  // alignment in bytes
ShapeFieldInfo *type_find_field(MixType *type, const char *name);
ShapeVariant *type_find_variant(MixType *type, const char *name);

#endif // TYPES_H
