#include "types.h"

const char *type_kind_name(TypeKind kind) {
    switch (kind) {
        case TYPE_INT:     return "int";
        case TYPE_FLOAT:   return "float";
        case TYPE_BOOL:    return "bool";
        case TYPE_BYTE:    return "byte";
        case TYPE_STR:     return "str";
        case TYPE_VOID:    return "void";
        case TYPE_INT8:    return "int8";
        case TYPE_INT16:   return "int16";
        case TYPE_INT32:   return "int32";
        case TYPE_INT64:   return "int64";
        case TYPE_UINT8:   return "uint8";
        case TYPE_UINT16:  return "uint16";
        case TYPE_UINT32:  return "uint32";
        case TYPE_UINT64:  return "uint64";
        case TYPE_FLOAT32: return "float32";
        case TYPE_FLOAT64: return "float64";
        case TYPE_PTR:     return "ptr";
        case TYPE_LIST:    return "list";
        case TYPE_MAP:     return "map";
        case TYPE_OPTIONAL: return "optional";
        case TYPE_FUNC:    return "func";
        case TYPE_SHAPE:   return "shape";
        case TYPE_NAMED:   return "named";
        case TYPE_INFER:   return "infer";
        case TYPE_SHARED:  return "shared";
        case TYPE_TASK:    return "task";
        case TYPE_RESULT:  return "result";
    }
    return "unknown";
}

const char *type_to_qbe(MixType *type) {
    if (!type) return "l";
    switch (type->kind) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: return "l";
        case TYPE_INT32: case TYPE_UINT32: return "w";
        case TYPE_INT16: case TYPE_UINT16: return "w";
        case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: return "w";
        case TYPE_BOOL: return "w";
        case TYPE_FLOAT: case TYPE_FLOAT64: return "d";
        case TYPE_FLOAT32: return "s";
        case TYPE_PTR: case TYPE_STR: return "l";
        case TYPE_LIST: return "l"; // lists are pointers to heap-allocated structs
        case TYPE_OPTIONAL: return "l"; // optionals are pointers to {has_value, value}
        case TYPE_SHAPE: return "l"; // aggregate types use :Name in QBE, but pointer representation is l
        default: return "l";
    }
}

bool type_is_integer(MixType *type) {
    if (!type) return false;
    switch (type->kind) {
        case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        case TYPE_BYTE:
            return true;
        default: return false;
    }
}

bool type_is_float(MixType *type) {
    if (!type) return false;
    return type->kind == TYPE_FLOAT || type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64;
}

bool type_is_numeric(MixType *type) {
    return type_is_integer(type) || type_is_float(type);
}

const char *type_to_qbe_mem(MixType *type) {
    if (!type) return "l";
    switch (type->kind) {
        case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: return "b";
        case TYPE_INT16: case TYPE_UINT16: return "h";
        case TYPE_INT32: case TYPE_UINT32: return "w";
        case TYPE_BOOL: return "w";
        case TYPE_FLOAT32: return "s";
        case TYPE_FLOAT64: case TYPE_FLOAT: return "d";
        default: return "l";
    }
}

const char *type_to_qbe_load(MixType *type) {
    if (!type) return "l";
    switch (type->kind) {
        case TYPE_INT8: return "sb";
        case TYPE_UINT8: case TYPE_BYTE: return "ub";
        case TYPE_INT16: return "sh";
        case TYPE_UINT16: return "uh";
        case TYPE_INT32: return "sw";
        case TYPE_UINT32: return "uw";
        case TYPE_BOOL: return "uw";
        case TYPE_FLOAT32: return "s";
        case TYPE_FLOAT64: case TYPE_FLOAT: return "d";
        default: return "l";
    }
}

int type_size(MixType *type) {
    if (!type) return 8;
    switch (type->kind) {
        case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL: return 1;
        case TYPE_INT16: case TYPE_UINT16: return 2;
        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: return 4;
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64:
        case TYPE_FLOAT: case TYPE_FLOAT64:
        case TYPE_PTR: case TYPE_STR: return 8;
        case TYPE_SHAPE: return type->shape.total_size;
        default: return 8;
    }
}

int type_alignment(MixType *type) {
    if (!type) return 8;
    switch (type->kind) {
        case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL: return 1;
        case TYPE_INT16: case TYPE_UINT16: return 2;
        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: return 4;
        default: return 8;
    }
}

ShapeFieldInfo *type_find_field(MixType *type, const char *name) {
    if (!type || type->kind != TYPE_SHAPE) return NULL;
    for (int i = 0; i < type->shape.field_count; i++) {
        if (strcmp(type->shape.fields[i].name, name) == 0) {
            return &type->shape.fields[i];
        }
    }
    return NULL;
}

ShapeVariant *type_find_variant(MixType *type, const char *name) {
    if (!type || type->kind != TYPE_SHAPE || !type->shape.is_tagged_union) return NULL;
    for (int i = 0; i < type->shape.variant_count; i++) {
        if (strcmp(type->shape.variants[i].name, name) == 0) {
            return &type->shape.variants[i];
        }
    }
    return NULL;
}
