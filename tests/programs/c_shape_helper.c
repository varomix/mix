#include "c_shape_helper.h"

void cvec2_scale(CVec2* v, double k) {
    v->x *= k;
    v->y *= k;
}

double cvec2_dot(const CVec2* a, const CVec2* b) {
    return a->x * b->x + a->y * b->y;
}

void cbig_zero(CBig* b) {
    b->a = 0; b->b = 0; b->c = 0; b->d = 0;
    b->e = 0; b->f = 0; b->g = 0; b->h = 0;
}

void cbig_set_seq(CBig* b, double start) {
    b->a = start + 0;
    b->b = start + 1;
    b->c = start + 2;
    b->d = start + 3;
    b->e = start + 4;
    b->f = start + 5;
    b->g = start + 6;
    b->h = start + 7;
}

double cbig_sum(const CBig* b) {
    return b->a + b->b + b->c + b->d + b->e + b->f + b->g + b->h;
}
