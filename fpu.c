#include <stdint.h>

#include "alu.h"

void xmul_u128(uint64_t op1, uint64_t op2, uint64_t *lo, uint64_t *hi) {
    printf("%016lX * %016lX", op1, op2);
    uint64_t u1 = (op1 & 0xffffffff);
    uint64_t v1 = (op2 & 0xffffffff);
    uint64_t t = (u1 * v1);
    uint64_t w3 = (t & 0xffffffff);
    uint64_t k = (t >> 32);

    op1 >>= 32;
    t = (op1 * v1) + k;
    k = (t & 0xffffffff);
    uint64_t w1 = (t >> 32);

    op2 >>= 32;
    t = (u1 * op2) + k;
    k = (t >> 32);

    *hi = (op1 * op2) + w1 + k;
    *lo = (t << 32) + w3;
    
    printf(" = %016lX%016lX\n", *hi, *lo);
}

void shr_u128(
    uint64_t a, // most significant
    uint64_t b, // least significant
    int shamt,
    uint64_t* result,
    uint64_t* overflow
);

void shl_u128(
    uint64_t a, // most significant
    uint64_t b, // least significant
    int shamt,
    uint64_t* result,
    uint64_t* overflow
) {
    if (shamt < 0) {
        shr_u128(a, b, -shamt, result, overflow);
        return;
    }
    
    *result = b << shamt;
    *overflow = (a << shamt) | (b >> (64 - shamt));
}

void shr_u128(
    uint64_t a, // most significant
    uint64_t b, // least significant
    int shamt,
    uint64_t* result,
    uint64_t* overflow
) {
    if (shamt < 0) {
        shl_u128(a, b, -shamt, result, overflow);
        return;
    }
    
    *overflow = a >> shamt;
    *result = (b >> shamt) | (a << (64 - shamt));
}

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

uint64_t fmul(
    ist66_fpac_t *a,
    ist66_fpac_t *b,
    int rmode,
    ist66_fpac_t *result
) {
    if (
        (a->exp_biased == 0 && a->significand == 0) ||
        (b->exp_biased == 0 && b->significand == 0)
    ) { // zero
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return 0;
    } else if (
        (a->exp_biased == 0 && a->significand != 0) ||
        (b->exp_biased == 0 && b->significand != 0)
    ) { // denormalized!
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return FP_INVALID;
    } else if (
        (a->exp_biased == 0xFF && a->significand == 0) ||
        (b->exp_biased == 0xFF && b->significand == 0)
    ) { // infinity
        result->sign = a->sign ^ b->sign;
        result->exp_biased = 0xFF;
        result->significand = 0;
        return 0;
    } else if (
        (a->exp_biased == 0xFF && a->significand != 0) ||
        (b->exp_biased == 0xFF && b->significand != 0)
    ) { // NaN
        result->sign = 0;
        result->exp_biased = 0xFF;
        result->significand = 0x7FFFFFFFFFFFFFFF;
        return FP_INVALID;
    }
    
    uint64_t m_a = a->significand;
    uint64_t m_b = b->significand;
    
    // TODO: remove attempt at subnormal support
    int exp = -EXP_BIAS;
    if (a->exp_biased == 0) exp++;
    else {
        m_a |= 1L << 63;
        exp += a->exp_biased;
    }
    if (b->exp_biased == 0) exp++;
    else {
        m_b |= 1L << 63;
        exp += b->exp_biased;
    }
    
    if (exp < 0) {
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return FP_UNDRFLOW;
    } else if (exp > 254) {
        result->sign = a->sign ^ b->sign;
        result->exp_biased = 0xFF;
        result->significand = 0;
        return FP_OVERFLOW;
    }
    
    uint64_t m_ch, m_cl;
    xmul_u128(m_a, m_b, &m_cl, &m_ch);
    
    int inexact = 0;
    
    if (m_ch & (1L << 63)) {
        exp++;
        if (exp > 254) {
            result->sign = a->sign ^ b->sign;
            result->exp_biased = 0xFF;
            result->significand = 0;
            return FP_OVERFLOW;
        }
        
        if (m_cl) inexact = 1;
        // TODO: rounding
    } else if ((m_ch & (1L << 62)) && exp != 0) {
        shl_u128(m_ch, m_cl, 1, &m_cl, &m_ch);
        if (m_cl) inexact = 1;
    }
    
    result->sign = a->sign ^ b->sign;
    result->exp_biased = (uint8_t) (exp & 0xFF);
    result->significand = m_ch & 0x7FFFFFFFFFFFFFFF;
    return (FP_INEXACT * inexact)
        | ((result->exp_biased == 0 && result->significand != 0)
            * FP_UNDRFLOW);
}

uint64_t fadd(
    ist66_fpac_t *a,
    ist66_fpac_t *b,
    int rmode,
    ist66_fpac_t *result
) {
    if (
        (a->exp_biased == 0 && a->significand != 0) ||
        (b->exp_biased == 0 && b->significand != 0)
    ) { // denormalized!
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return FP_INVALID;
    } else if (
        (a->exp_biased == 0xFF && a->significand == 0) ||
        (b->exp_biased == 0xFF && b->significand == 0)
    ) { // infinity
        result->sign = a->sign ^ b->sign;
        result->exp_biased = 0xFF;
        result->significand = 0;
        return 0;
    } else if (
        (a->exp_biased == 0xFF && a->significand != 0) ||
        (b->exp_biased == 0xFF && b->significand != 0)
    ) { // NaN
        result->sign = 0;
        result->exp_biased = 0xFF;
        result->significand = 0x7FFFFFFFFFFFFFFF;
        return FP_INVALID;
    }
    
    if (
        (b->exp_biased > a->exp_biased) || (
            b->exp_biased == a->exp_biased &&
            b->significand > a->significand
        )
    ) { // make sure |a| >= |b|
        ist66_fpac_t *temp = a;
        a = b;
        b = temp;
    }
    // ...
}

int main(int argc, char *argv[]) {    
    ist66_fpac_t a, b, c;
    
    a.sign = 0;
    a.exp_biased = 128;
    a.significand = 1L << 62;
    
    b.sign = 0;
    b.exp_biased = 0xFE;
    b.significand = 1L << 62;
    
    uint64_t result = fmul(&a, &b, 0, &c);
    
    printf("%d %02hhX %021lo x\n", a.sign, a.exp_biased, a.significand);
    printf("%d %02hhX %021lo =\n", b.sign, b.exp_biased, b.significand);
    printf("%d %02hhX %021lo .\n", c.sign, c.exp_biased, c.significand);
    printf("(%02lX)\n", result);
    return 0;
}