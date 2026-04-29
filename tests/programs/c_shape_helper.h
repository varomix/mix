#ifndef C_SHAPE_HELPER_H
#define C_SHAPE_HELPER_H

// Layouts must match the MIX shape declarations in 106_c_shape_abi.mix.
// This is the contract the ABI is required to preserve across backends.

typedef struct {
    double x;
    double y;
} CVec2;

typedef struct {
    double a, b, c, d, e, f, g, h;
} CBig;

void cvec2_scale(CVec2* v, double k);
double cvec2_dot(const CVec2* a, const CVec2* b);

void cbig_zero(CBig* b);
void cbig_set_seq(CBig* b, double start);
double cbig_sum(const CBig* b);

#endif
