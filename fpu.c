#include <stdint.h>
#include <stdio.h>

#define F_OVRF 1    // overflow
#define F_UNDF 2    // underflow
#define F_ILGL 4    // illegal argument

typedef struct {
    uint16_t sign_exp;
    uint64_t signif;
} rdc700_float_t;

/*
 * RDC-700 floating point format
 *
 * Single-precision: 1-bit sign, 8-bit excess-127 exponent, 27-bit significand
 * Double-precision: 1-bit sign, 8-bit excess-127 exponent, 64-bit significand
 * Internal: 1-bit sign, 15-bit excess-16383 exponent, 64-bit significand
 * All significands use explicit leading one; unnormalized values are allowed
 *
 * Sign  Expt Signif. Value
 * ==========================================
 *    0     0       0 Zero
 *    0     0 Nonzero Pseudo NaN
 *    1     0     Any NaN
 *  Any   Max     Any +/-Infinity
 *  Any Range       0 Zero (Unnormalized)
 *  Any Range Nonzero Numeric value
 */

int is_nan(rdc700_float_t *n) {
    return (
        (
            (n->sign_exp & (1 << 15)) &&
            ((n->sign_exp & 0x7FFF) == 0)
        ) ||
        (n->sign_exp == 0 && n->signif != 0)
    );
}

int is_inf(rdc700_float_t *n) {
    return (
        (n->sign_exp & 0x7FFF) == 0x7FFF
    );
}

int is_zero(rdc700_float_t *n) {
    return (
        !is_nan(n) && !is_inf(n) && (n->signif == 0)
    );
}

/*
 * 80-bit floating-point normalize
 */

void rdc700_fnorm(rdc700_float_t *src, rdc700_float_t *dst) {
    if (is_inf(src)) {
        dst->sign_exp = src->sign_exp;
        dst->signif = src->signif;
        return;
    } else if (is_nan(src)) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return;
    } else if (src->signif == 0) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return;
    }

    uint16_t new_exp = src->sign_exp & 0x7FFF;
    uint64_t new_signif = src->signif;
    while (new_exp > 1 && !(new_signif & (1L << 63))) {
        new_signif <<= 1;
        new_exp--;
    }
    dst->sign_exp = (src->sign_exp & 0x8000) | new_exp;
    dst->signif = new_signif;
}

/*
 * 80-bit floating-point multiply
 *
 *      NaN * anything    => NaN
 *        0 * infinity    => NaN
 * infinity * number      => infinity
 * infinity * infinity    => infinity
 *        0 * number or 0 => 0 (normalized)
 *   number * number      => number
 */

int rdc700_fmul(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
) {
    uint16_t new_sign_exp = (
        (src->sign_exp & (1 << 15)) ^ (src->sign_exp & (1 << 15))
    );

    if (is_nan(src) || is_nan(tgt)) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return F_ILGL;
    }

    else if (
        (is_zero(src) && is_inf(dst)) ||
        (is_inf(src) && is_zero(dst))
    ) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return F_ILGL;
    }

    else if (is_zero(src) || is_zero(tgt)) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return 0;
    }

    else if (is_inf(src) || is_inf(tgt)) {
        new_sign_exp |= 0x7FFF;
        dst->sign_exp = new_sign_exp;
        dst->signif = 0;
        return F_OVRF;
    }
    
    int exp_src = ((int) (src->sign_exp & 0x7FFF)) - 16383;
    int exp_tgt = ((int) (tgt->sign_exp & 0x7FFF)) - 16383;
    int exp_norm = 0;
    
    unsigned __int128 a = src->signif, b = tgt->signif;
    unsigned __int128 c = a * b;
    
    int do_round_1 = (c >> 62) & 1;
    c = (c >> 63) + do_round_1;
    int do_round_2 = c & 1;
    if (((c >> 64) & 1)) {
        exp_norm = 1;
        c = (c >> 1) + do_round_2;
    }
    
    int exp_dst = exp_src + exp_tgt + exp_norm;
    if (exp_dst < -16382) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return F_UNDF;
    } else if (exp_dst > 16383) {
        exp_dst = 16384;
    }
    
    new_sign_exp |= ((uint16_t) (exp_dst + 16383));
    dst->sign_exp = new_sign_exp;
    dst->signif = c;

    if (exp_dst == 16384) return F_OVRF;
    else return 0;
}

void print_rdc_float(rdc700_float_t *f) {
    int is_neg = f->sign_exp >> 15;

    if (is_inf(f)) {
        printf("%cInfinity", is_neg ? '-' : 0);
        return;
    } else if (is_nan(f)) {
        printf("%cNaN", is_neg ? 0 : 'P');
        return;
    } else if (f->sign_exp == 0 && f->signif == 0) {
        printf("0.0");
        return;
    }

    int exp = ((int) (f->sign_exp & 0x7FFF)) - 16383;
    
    int whole = f->signif >> 63;
    uint64_t frac = f->signif << 1;
    
    uint64_t frac_digit = 5000000000000000000;
    uint64_t frac_out = 0;
    
    while (frac) {
        if ((frac & (1L << 63))) {
            frac_out += frac_digit;
        }
        frac_digit >>= 1;
        frac <<= 1;
    }
    
    char frac_print[20];
    snprintf(frac_print, 20, "%lu", frac_out);
    int last_nonzero = 0;
    for (int i = 0; i < 19; i++) {
        if (frac_print[i] == 0) break;
        if (frac_print[i] != '0') last_nonzero = i;
    }
    frac_print[last_nonzero + 1] = 0;
    
    printf("%c%d.%s(2^%d)", is_neg ? '-' : 0, whole, frac_print, exp);
}

int main(int argc, char *argv[]) {
    rdc700_float_t src = {
        .sign_exp = 16384,
        .signif = 0x5000000000000000
    };
    
    rdc700_float_t tgt = {
        .sign_exp = 16384,
        .signif = 0xC000000000000000
    };
    
    rdc700_float_t result;
    rdc700_fmul(&src, &tgt, &result);
    
    print_rdc_float(&src);
    printf(" x ");
    print_rdc_float(&tgt);
    printf(" = ");
    print_rdc_float(&result);
    printf(" = ");
    rdc700_fnorm(&result, &result);
    print_rdc_float(&result);
    printf("\n");
    
    return 0;
}
