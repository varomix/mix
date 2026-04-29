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
        case TYPE_REF:     return "ref";
        case TYPE_BOX:     return "Box";
        case TYPE_LIST:    return "list";
        case TYPE_MAP:     return "map";
        case TYPE_OPTIONAL: return "optional";
        case TYPE_FUNC:    return "func";
        case TYPE_SHAPE:   return "shape";
        case TYPE_NAMED:   return "named";
        case TYPE_INFER:   return "infer";
        case TYPE_SHARED:  return "shared";
        case TYPE_ZONE:    return "Zone";
        case TYPE_TASK:    return "task";
        case TYPE_RESULT:  return "result";
        case TYPE_SET:     return "set";
        case TYPE_GENERIC: return "T";
    }
    return "unknown";
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

int type_size(MixType *type) {
    if (!type) return 8;
    switch (type->kind) {
        case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL: return 1;
        case TYPE_INT16: case TYPE_UINT16: return 2;
        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: return 4;
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64:
        case TYPE_FLOAT: case TYPE_FLOAT64:
        case TYPE_PTR: case TYPE_REF: case TYPE_BOX:
        case TYPE_STR: case TYPE_ZONE: return 8;
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
        case TYPE_SHAPE: return type->shape.alignment > 0 ? type->shape.alignment : 8;
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
