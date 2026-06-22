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
} acr7k_float_t;

int f80_round_to_f36(acr7k_float_t *src, acr7k_float_t *dst);
int f80_round_to_f72(acr7k_float_t *src, acr7k_float_t *dst);

int get_f36(acr7k_float_t *src, uint64_t *dst);
void set_f36(uint64_t *src, acr7k_float_t *dst);
int get_f72(acr7k_float_t *src, uint64_t *dst, uint64_t *dst_l);
void set_f72(uint64_t *src, uint64_t *src_l, acr7k_float_t *dst);

int is_nan(acr7k_float_t *n);
int is_inf(acr7k_float_t *n);
int is_zero(acr7k_float_t *n);

void acr7k_fnorm(acr7k_float_t *src, acr7k_float_t *dst);
void acr7k_fneg(acr7k_float_t *src, acr7k_float_t *dst);
int acr7k_fconorm(
    acr7k_float_t *src, acr7k_float_t *tgt,
    acr7k_float_t *dst_g, acr7k_float_t *dst_l
);

int acr7k_fadd(
    acr7k_float_t *src,
    acr7k_float_t *tgt,
    acr7k_float_t *dst
);
int acr7k_fmul(
    acr7k_float_t *src,
    acr7k_float_t *tgt,
    acr7k_float_t *dst
);
int acr7k_fdiv(
    acr7k_float_t *src,
    acr7k_float_t *tgt,
    acr7k_float_t *dst
);

void print_rdc_float(acr7k_float_t *f);

#endif

