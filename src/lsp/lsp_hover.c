#include "lsp_hover.h"

int mix_type_to_string(MixType *type, char *buf, int size) {
    if (!type) return snprintf(buf, size, "unknown");
    switch (type->kind) {
        case TYPE_INT:     return snprintf(buf, size, "int");
        case TYPE_FLOAT:   return snprintf(buf, size, "float");
        case TYPE_BOOL:    return snprintf(buf, size, "bool");
        case TYPE_STR:     return snprintf(buf, size, "str");
        case TYPE_VOID:    return snprintf(buf, size, "void");
        case TYPE_BYTE:    return snprintf(buf, size, "byte");
        case TYPE_INT8:    return snprintf(buf, size, "int8");
        case TYPE_INT16:   return snprintf(buf, size, "int16");
        case TYPE_INT32:   return snprintf(buf, size, "int32");
        case TYPE_INT64:   return snprintf(buf, size, "int64");
        case TYPE_UINT8:   return snprintf(buf, size, "uint8");
        case TYPE_UINT16:  return snprintf(buf, size, "uint16");
        case TYPE_UINT32:  return snprintf(buf, size, "uint32");
        case TYPE_UINT64:  return snprintf(buf, size, "uint64");
        case TYPE_FLOAT32: return snprintf(buf, size, "float32");
        case TYPE_FLOAT64: return snprintf(buf, size, "float64");
        case TYPE_PTR: {
            int n = snprintf(buf, size, "*");
            n += mix_type_to_string(type->ptr.base, buf + n, size - n);
            return n;
        }
        case TYPE_REF: {
            int n = snprintf(buf, size, "ref%s ",
                             type->ref.is_mutable ? "!" : "");
            n += mix_type_to_string(type->ref.base, buf + n, size - n);
            return n;
        }
        case TYPE_BOX: {
            int n = snprintf(buf, size, "Box[");
            n += mix_type_to_string(type->box.inner, buf + n, size - n);
            n += snprintf(buf + n, size - n, "]");
            return n;
        }
        case TYPE_LIST: {
            int n = snprintf(buf, size, "[");
            n += mix_type_to_string(type->list.elem_type, buf + n, size - n);
            n += snprintf(buf + n, size - n, "]");
            return n;
        }
        case TYPE_MAP: {
            int n = snprintf(buf, size, "{");
            n += mix_type_to_string(type->map.key_type, buf + n, size - n);
            n += snprintf(buf + n, size - n, ": ");
            n += mix_type_to_string(type->map.val_type, buf + n, size - n);
            n += snprintf(buf + n, size - n, "}");
            return n;
        }
        case TYPE_OPTIONAL: {
            int n = mix_type_to_string(type->optional.inner, buf, size);
            n += snprintf(buf + n, size - n, "?");
            return n;
        }
        case TYPE_FUNC: {
            int n = snprintf(buf, size, "(");
            for (int i = 0; i < type->func.param_count; i++) {
                if (i > 0) n += snprintf(buf + n, size - n, ", ");
                n += mix_type_to_string(type->func.param_types[i], buf + n, size - n);
            }
            n += snprintf(buf + n, size - n, ") -> ");
            n += mix_type_to_string(type->func.return_type, buf + n, size - n);
            return n;
        }
        case TYPE_SHAPE:
            if (type->shape.is_union)
                return snprintf(buf, size, "union %s", type->shape.name ? type->shape.name : "union");
            return snprintf(buf, size, "%s", type->shape.name ? type->shape.name : "shape");
        case TYPE_NAMED:
            return snprintf(buf, size, "%s", type->named.name ? type->named.name : "?");
        case TYPE_SHARED: {
            int n = snprintf(buf, size, "shared ");
            n += mix_type_to_string(type->shared.inner, buf + n, size - n);
            return n;
        }
        case TYPE_ZONE:
            return snprintf(buf, size, "Zone");
        case TYPE_TASK: {
            int n = snprintf(buf, size, "task<");
            n += mix_type_to_string(type->task.result_type, buf + n, size - n);
            n += snprintf(buf + n, size - n, ">");
            return n;
        }
        case TYPE_INFER: return snprintf(buf, size, "inferred");
        default: return snprintf(buf, size, "?");
    }
}
