#ifndef _FLOAT_
#define _FLOAT_

#define EXP_BIAS 127

typedef struct {
    int sign;
    uint8_t exp_biased;
    uint64_t significand;
} ist66_fpac_t;

#define FP_INVALID  1
#define FP_DIVZERO  2
#define FP_OVERFLOW 4
#define FP_UNDRFLOW 8
#define FP_INEXACT  16

uint64_t fdiv(
    ist66_fpac_t *a,
    ist66_fpac_t *b,
    ist66_fpac_t *result
);

uint64_t fmul(
    ist66_fpac_t *a,
    ist66_fpac_t *b,
    int rmode,
    ist66_fpac_t *result
);

uint64_t fadd(
    ist66_fpac_t *a,
    ist66_fpac_t *b,
    int rmode,
    ist66_fpac_t *result
);

#endif

