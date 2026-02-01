#ifndef _FMATH_
#define _FMATH_

#include <stdint.h>

#define F_OVRF 1    // overflow
#define F_UNDF 2    // underflow
#define F_INSG 4    // insignificant
#define F_ILGL 8    // illegal argument

typedef struct {
    uint16_t sign_exp;
    uint64_t signif;
} rdc700_float_t;

int f80_round_to_f36(rdc700_float_t *src, rdc700_float_t *dst);
int f80_round_to_f72(rdc700_float_t *src, rdc700_float_t *dst);
int get_f36(rdc700_float_t *src, uint64_t *dst);
int get_f72(rdc700_float_t *src, uint64_t *dst, uint64_t *dst_l);

int is_nan(rdc700_float_t *n);

int is_inf(rdc700_float_t *n);

int is_zero(rdc700_float_t *n);

void rdc700_fnorm(rdc700_float_t *src, rdc700_float_t *dst);

int rdc700_fconorm(
    rdc700_float_t *src, rdc700_float_t *tgt,
    rdc700_float_t *dst_g, rdc700_float_t *dst_l
);

int rdc700_fadd(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
);

int rdc700_fmul(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
);

int rdc700_fdiv(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
);

void print_rdc_float(rdc700_float_t *f);

#endif

