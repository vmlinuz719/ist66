#include <stdint.h>

#include "fpu.h"

void xmul_u128(uint64_t op1, uint64_t op2, uint64_t *lo, uint64_t *hi) {
    // printf("%016lX * %016lX", op1, op2);
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
    
    // printf(" = %016lX%016lX\n", *hi, *lo);
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

int add_u65(
    uint64_t a,
    uint64_t b,
    uint64_t* result
) {
    // printf("%021lo + %021lo", a, b);
    uint64_t low = (a & 0xFFFFFFFF) + (b & 0xFFFFFFFF);
    uint64_t high = (a >> 32) + (b >> 32) + (low >> 32);
    *result = (high << 32) | (low & 0xFFFFFFFF);
    // printf(" = %01lo%021loX\n", high >> 32, *result);
    return (int) (high >> 32);
}

uint64_t fdiv(
    ist66_fpac_t *a,
    ist66_fpac_t *b,
    ist66_fpac_t *result
) {
    if (
        (a->exp_biased == 0 && a->significand == 0) &&
        (b->exp_biased == 0 && b->significand == 0)
    ) { // zero over zero, NAN
        result->sign = 0;
        result->exp_biased = 0xFF;
        result->significand = 0x7FFFFFFFFFFFFFFF;
        return FP_INVALID;
    } else if (
        (a->exp_biased == 0 && a->significand == 0)
    ) { // zero
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return 0;
    } else if (
        (b->exp_biased == 0 && b->significand == 0)
    ) { // divide by zero
        result->sign = a->sign;
        result->exp_biased = 0xFF;
        result->significand = 0;
        return FP_DIVZERO;
    } else if (
        (a->exp_biased == 0xFF && a->significand == 0)
    ) { // infinity
        result->sign = a->sign ^ b->sign;
        result->exp_biased = 0xFF;
        result->significand = 0;
        return 0;
    } else if (
        (b->exp_biased == 0xFF && b->significand == 0)
    ) { // divide by infinity
        result->sign = 0;
        result->exp_biased = 0x0;
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
        (a->exp_biased == 0xFF && a->significand != 0) ||
        (b->exp_biased == 0xFF && b->significand != 0)
    ) { // NaN
        result->sign = 0;
        result->exp_biased = 0xFF;
        result->significand = 0x7FFFFFFFFFFFFFFF;
        return FP_INVALID;
    }
    
    uint64_t m_a = (1L << 63) |  a->significand;
    uint64_t m_b = (1L << 63) |  b->significand;
    
    int new_exp = ((int) (a->exp_biased)) - ((int) (b->exp_biased));
    new_exp += EXP_BIAS;
    if (new_exp < 0) {
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return FP_UNDRFLOW;
    }
    
    unsigned __int128 m_a_big = m_a;
    m_a_big <<= 63;
    uint64_t m_c = m_a_big / m_b;
    int inexact = (m_a_big % m_b != 0);
    
    while (!(m_c & (1L << 63))) {
        new_exp--;
        if (new_exp == 0) {
            result->sign = a->sign ^ b->sign;
            result->exp_biased = 0;
            result->significand = m_c & 0x7FFFFFFFFFFFFFFF;
            return FP_UNDRFLOW;
        }
        m_c <<= 1;
    }
    
    if (new_exp > 254) {
        result->sign = a->sign ^ b->sign;
        result->exp_biased = 0xFF;
        result->significand = 0;
        return FP_OVERFLOW;
    }
    
    result->sign = a->sign ^ b->sign;
    result->exp_biased = new_exp;
    result->significand = m_c & 0x7FFFFFFFFFFFFFFF;
    return inexact * FP_INEXACT;
}

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
    
    uint64_t m_a = (1L << 63) |  a->significand;
    uint64_t m_bl = (1L << 63) |  b->significand;
    uint64_t m_b = m_bl >> (a->exp_biased - b->exp_biased);
    
    int inexact = (m_bl != (m_b << (a->exp_biased - b->exp_biased)));
    
    uint64_t m_c;
    int m_out;
    
    if (a->sign != b -> sign) {
        m_out = 0;
        m_c = m_a - m_b;
    } else {
        m_out = add_u65(m_a, m_b, &m_c);
    }
    
    if (m_c == 0 && m_out == 0) {
        result->sign = 0;
        result->exp_biased = 0;
        result->significand = 0;
        return 0;
    } else if (m_out) {
        int new_exp = a->exp_biased + 1;
        result->sign = a->sign;
        if (new_exp > 254) {
            result->exp_biased = 0xFF;
            result->significand = 0;
            return FP_OVERFLOW | (inexact * FP_INEXACT);
        }
        result->exp_biased = new_exp;
        result->significand = m_c >> 1;
        // TODO: rounding
        return ((m_c & 1) * FP_INEXACT) | (inexact * FP_INEXACT);
    } else {
        int new_exp = a->exp_biased;
        while (!(m_c & (1L << 63))) {
            new_exp--;
            if (new_exp == 0) {
                result->sign = a->sign;
                result->exp_biased = 0;
                result->significand = m_c & 0x7FFFFFFFFFFFFFFF;
                return FP_UNDRFLOW | (inexact * FP_INEXACT);
            }
            m_c <<= 1;
        }
        result->sign = a->sign;
        result->exp_biased = new_exp;
        result->significand = m_c & 0x7FFFFFFFFFFFFFFF;
        return 0 | (inexact * FP_INEXACT);
    }
}

/*
int main(int argc, char *argv[]) {    
    ist66_fpac_t a, b, c;
    
    a.sign = 1;
    a.exp_biased = 0x85;
    a.significand = 0b0111111000010000L << 48;
    
    b.sign = 1;
    b.exp_biased = 0x83;
    b.significand = 0b0000011110000000L << 48;;
    
    uint64_t result = fmul(&a, &b, 0, &c);
    
    printf("%d %02hhX %021lo x\n", a.sign, a.exp_biased, a.significand);
    printf("%d %02hhX %021lo =\n", b.sign, b.exp_biased, b.significand);
    printf("%d %02hhX %021lo .\n", c.sign, c.exp_biased, c.significand);
    printf("(%02lX)\n\n", result);
    
    result = fadd(&a, &b, 0, &c);
    
    printf("%d %02hhX %021lo +\n", a.sign, a.exp_biased, a.significand);
    printf("%d %02hhX %021lo =\n", b.sign, b.exp_biased, b.significand);
    printf("%d %02hhX %021lo .\n", c.sign, c.exp_biased, c.significand);
    printf("(%02lX)\n\n", result);
    
    result = fdiv(&a, &b, &c);
    
    printf("%d %02hhX %021lo /\n", a.sign, a.exp_biased, a.significand);
    printf("%d %02hhX %021lo =\n", b.sign, b.exp_biased, b.significand);
    printf("%d %02hhX %021lo .\n", c.sign, c.exp_biased, c.significand);
    printf("(%02lX)\n\n", result);
    
    return 0;
}
*/
