#ifndef LSP_HOVER_H
#define LSP_HOVER_H

#include "../types.h"

// Format a MixType as a human-readable string. Returns chars written.
int mix_type_to_string(MixType *type, char *buf, int size);

#endif // LSP_HOVER_H
