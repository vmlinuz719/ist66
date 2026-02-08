#include <stdint.h>
#include <stdio.h>
#include "fpu.h"

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

int f80_round_to_f36(rdc700_float_t *src, rdc700_float_t *dst) {
    uint16_t exp = src->sign_exp & 0x7FFF;
    if (exp < (1 + 16383 - 127)) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return F_UNDF;
    }
    else if (exp > (254 + 16383 - 127)) {
        dst->sign_exp = src->sign_exp | 0x7FFF;
        dst->signif = 0;
        return F_OVRF;
    }

    int round_one = (src->signif >> 36) & 1;
    uint64_t new_signif = (src->signif >> 37) + round_one;
    if ((new_signif & (1L << 27))) {
        exp++;
        if (exp > (254 + 16383 - 127)) {
            dst->sign_exp = src->sign_exp | 0x7FFF;
            dst->signif = 0;
            return F_OVRF;
        }
        new_signif = (new_signif >> 1) + (new_signif & 1);
    }

    dst->sign_exp = exp | (src->sign_exp & 0x8000);
    dst->signif = new_signif << 37;
    return 0;
}

int f80_round_to_f72(rdc700_float_t *src, rdc700_float_t *dst) {
    uint16_t exp = src->sign_exp & 0x7FFF;
    if (exp < (1 + 16383 - 127)) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return F_UNDF;
    }
    else if (exp > (254 + 16383 - 127)) {
        dst->sign_exp = src->sign_exp | 0x7FFF;
        dst->signif = 0;
        return F_OVRF;
    }

    int round_one = src->signif & 1;
    uint64_t new_signif = (src->signif >> 1) + round_one;
    if ((new_signif & (1L << 63))) {
        exp++;
        if (exp > (254 + 16383 - 127)) {
            dst->sign_exp = src->sign_exp | 0x7FFF;
            dst->signif = 0;
            return F_OVRF;
        }
        new_signif = (new_signif >> 1) + (new_signif & 1);
    }

    dst->sign_exp = exp | (src->sign_exp & 0x8000);
    dst->signif = new_signif << 1;
    return 0;
}

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

int get_f36(rdc700_float_t *src, uint64_t *dst) {
    if (is_nan(src)) {
        *dst = 0x800000000;
        return 0;
    }
    else if (is_inf(src)) {
        *dst = ((src->sign_exp & 0x8000) ? 0x800000000 : 0) + 0x7F8000000;
        return 0;
    }

    int exp_src = ((int) (src->sign_exp & 0x7FFF)) - 16383;
    if (exp_src > 127) {
        *dst = ((src->sign_exp & 0x8000) ? 0x800000000 : 0) + 0x7F8000000;
        return F_OVRF;
    } else if (exp_src < -126) {
        *dst = 0;
        return F_UNDF;
    }

    uint64_t result = ((uint64_t) (exp_src + 127)) << 27;
    result |= ((src->sign_exp & 0x8000) ? 0x800000000 : 0);
    result |= src->signif >> 37;
    *dst = result;
    return 0;
}

void set_f36(uint64_t *src, rdc700_float_t *dst) {
    if (*src == 0) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return;
    }

    else if (
        (*src & 0xFF8000000) == 0x800000000 || ( // true NaN
            ((*src & 0xFF8000000) == 0) && // pseudo NaN
            ((*src & 0x7FFFFFF) != 0)
        )
    ) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return;
    }

    dst->sign_exp = (*src & 0x800000000) ? 0x8000 : 0;
    uint64_t new_exp = (*src & 0x7F8000000) >> 27;
    if (new_exp == 0x1FF) {
        dst->sign_exp |= 0x7FFF;
    } else if (new_exp != 0) {
        dst->sign_exp |= new_exp + 16256;
    }
    dst->signif = *src << 37;
}

int get_f72(rdc700_float_t *src, uint64_t *dst, uint64_t *dst_l) {
    if (is_nan(src)) {
        *dst = 0x800000000;
        *dst_l = 0;
        return 0;
    }
    else if (is_inf(src)) {
        *dst = ((src->sign_exp & 0x8000) ? 0x800000000 : 0) + 0x7F8000000;
        *dst_l = 0;
        return 0;
    }

    int exp_src = ((int) (src->sign_exp & 0x7FFF)) - 16383;
    if (exp_src > 127) {
        *dst = ((src->sign_exp & 0x8000) ? 0x800000000 : 0) + 0x7F8000000;
        *dst_l = 0;
        return F_OVRF;
    } else if (exp_src < -126) {
        *dst = 0;
        *dst_l = 0;
        return F_UNDF;
    }

    uint64_t result = ((uint64_t) (exp_src + 127)) << 27;
    result |= ((src->sign_exp & 0x8000) ? 0x800000000 : 0);
    result |= src->signif >> 37;
    *dst = result;
    *dst_l = (src->signif >> 1) & 0xFFFFFFFFF;
    return 0;
}

void set_f72(uint64_t *src, uint64_t *src_l, rdc700_float_t *dst) {
    if (*src == 0 && *src_l == 0) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return;
    }

    else if (
        (*src & 0xFF8000000) == 0x800000000 || ( // true NaN
            ((*src & 0xFF8000000) == 0) && ( // pseudo NaN
                ((*src & 0x7FFFFFF) != 0) ||
                (*src_l != 0)
            )
        )
    ) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return;
    }

    dst->sign_exp = (*src & 0x800000000) ? 0x8000 : 0;
    uint64_t new_exp = (*src & 0x7F8000000) >> 27;
    if (new_exp == 0x1FF) {
        dst->sign_exp |= 0x7FFF;
    } else if (new_exp != 0) {
        dst->sign_exp |= new_exp + 16256;
    }
    dst->signif = *src << 37;
    dst->signif |= *src_l << 1;
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
 * 80-bit floating-point conormalize: adjust lesser exponent
 */

int rdc700_fconorm(
    rdc700_float_t *src, rdc700_float_t *tgt,
    rdc700_float_t *dst_g, rdc700_float_t *dst_l
) {
    if (
        is_inf(src) ||
        is_nan(src) ||
        is_inf(tgt) ||
        is_nan(tgt) ||
        (src->sign_exp & 0x7FFF) == (tgt->sign_exp & 0x7FFF)
    ) {
        dst_g->sign_exp = src->sign_exp;
        dst_g->signif = src->signif;
        dst_l->sign_exp = tgt->sign_exp;
        dst_l->signif = tgt->signif;
        return 0;
    }

    else if (is_zero(src)) {
        dst_l->sign_exp = 0;
        dst_l->signif = 0;
        if (is_zero(tgt)) {
            dst_g->sign_exp = 0;
            dst_g->signif = 0;
        } else {
            dst_g->sign_exp = tgt->sign_exp;
            dst_g->signif = tgt->signif;
        }
        return 0;
    }

    else if (is_zero(tgt)) {
        dst_l->sign_exp = 0;
        dst_l->signif = 0;

        dst_g->sign_exp = src->sign_exp;
        dst_g->signif = src->signif;

        return 0;
    }

    rdc700_float_t lesser;

    if ((src->sign_exp & 0x7FFF) > (tgt->sign_exp & 0x7FFF)) {
        dst_g->sign_exp = src->sign_exp;
        dst_g->signif = src->signif;
        lesser.sign_exp = tgt->sign_exp;
        lesser.signif = tgt->signif;
    } else {
        dst_g->sign_exp = tgt->sign_exp;
        dst_g->signif = tgt->signif;
        lesser.sign_exp = src->sign_exp;
        lesser.signif = src->signif;
    }

    uint16_t greater_exp = dst_g->sign_exp & 0x7FFF;
    uint16_t lesser_exp = lesser.sign_exp & 0x7FFF;
    uint16_t diff_exp = greater_exp - lesser_exp;

    if (diff_exp > 64) {
        // insignificant
        dst_l->sign_exp = 0;
        dst_l->signif = 0;
        return F_INSG;
    }

    if (diff_exp) {
        uint64_t round_one = (lesser.signif >> (diff_exp - 1)) & 1;
        uint64_t new_signif = (lesser.signif >> diff_exp) + round_one;
        dst_l->sign_exp = greater_exp | (dst_g->sign_exp & 0x8000);
        dst_l->signif = new_signif;
    } else {
        dst_l->sign_exp = greater_exp | (dst_g->sign_exp & 0x8000);
        dst_l->signif = lesser.signif;
    }



    return 0;
}

/*
 * 80-bit floating-point add
 */

int rdc700_fadd(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
) {
    if (is_nan(src) || is_nan(tgt)) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return F_ILGL;
    }

    else if (is_zero(src)) {
        dst->sign_exp = tgt->sign_exp;
        dst->signif = tgt->signif;
        return 0;
    }

    else if (is_zero(tgt)) {
        dst->sign_exp = src->sign_exp;
        dst->signif = src->signif;
        return 0;
    }

    else if (is_inf(src)) {
        if (is_inf(tgt)) {
            if ((src->sign_exp & 0x8000) != (tgt->sign_exp & 0x8000)) {
                dst->sign_exp = 0;
                dst->signif = 0;
            } else {
                dst->sign_exp = src->sign_exp;
                dst->signif = 0;
            }
        } else {
            dst->sign_exp = src->sign_exp;
            dst->signif = src->signif;
        }
        return 0;
    }

    else if (is_inf(tgt)) {
        dst->sign_exp = tgt->sign_exp;
        dst->signif = tgt->signif;
        return 0;
    }

    rdc700_float_t a, b;
    int insignificant = rdc700_fconorm(src, tgt, &a, &b);
    if (insignificant) {
        dst->sign_exp = a.sign_exp;
        dst->signif = a.signif;
        return F_INSG;
    }

    rdc700_float_t *greater = (a.signif > b.signif) ? &a : &b;
    rdc700_float_t *lesser = (a.signif > b.signif) ? &b : &a;
    int carry = 0;
    if ((src->sign_exp & 0x8000) == (tgt->sign_exp & 0x8000)) {
        dst->signif = greater->signif + lesser->signif;
        carry = (dst->signif < greater->signif);

    } else {
        dst->signif = greater->signif - lesser->signif;
    }
    if (carry) {
        uint64_t round_one = dst->signif & 1;
        dst->signif = ((dst->signif >> 1) | (1L << 63)) + round_one;
        dst->sign_exp = greater->sign_exp + 1;
        if ((dst->sign_exp & 0x7FFF) == 0x7FFF) return F_OVRF;
    } else {
        dst->sign_exp = greater->sign_exp;
    }

    return 0;
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
        (src->sign_exp & (1 << 15)) ^ (tgt->sign_exp & (1 << 15))
    );

    if (is_nan(src) || is_nan(tgt)) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return F_ILGL;
    }

    else if (
        (is_zero(src) && is_inf(tgt)) ||
        (is_inf(src) && is_zero(tgt))
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

/*
 * 80-bit floating-point divide
 */

int rdc700_fdiv(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
) {
    uint16_t new_sign_exp = (
        (src->sign_exp & (1 << 15)) ^ (tgt->sign_exp & (1 << 15))
    );

    if (is_nan(src) || is_nan(tgt)) {
        dst->sign_exp = 0x8000;
        dst->signif = 0;
        return F_ILGL;
    }
    
    else if (is_zero(tgt)) {
        dst->sign_exp = new_sign_exp | 0x7FFF;
        dst->signif = 0;
        return F_ILGL;
    }
    
    else if (is_inf(src)) {
        if (is_inf(tgt)) {
            dst->sign_exp = new_sign_exp | 16383;
            dst->signif = 1L << 63;
        } else {
            dst->sign_exp = new_sign_exp | 0x7FFF;
            dst->signif = 0;
        }
        return 0;
    }
    
    else if (is_inf(tgt)) {
        dst->sign_exp = 0;
        dst->signif = 0;
        return 0;
    }
    
    int exp_src = ((int) (src->sign_exp & 0x7FFF)) - 16383;
    int exp_tgt = ((int) (tgt->sign_exp & 0x7FFF)) - 16383;
    
    unsigned __int128 a = src->signif, b = tgt->signif;
    a <<= 64;
    unsigned __int128 c_lll = a / b;
    
    int round_one = c_lll & 1;
    c_lll = (c_lll >> 1) + round_one;
    
    int exp_dst = exp_src - exp_tgt;
    
    unsigned __int128 mask = 0xFFFFFFFFFFFFFFFF;
    mask <<= 64;
    while ((c_lll & mask)) {
        round_one = c_lll & 1;
        c_lll = (c_lll >> 1);
        exp_dst++;
    }
    
    c_lll += round_one;
    if (((c_lll >> 64) & 1)) {
        exp_dst++;
        c_lll = (c_lll >> 1) + (c_lll & 1);
    }
    
    uint64_t c = c_lll & 0xFFFFFFFFFFFFFFFF;
    
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
    /* BROKEN!!! */

    printf("% 6d %064lb", ((int) (f->sign_exp & 0x7FFF)) - 16383, f->signif);
}

/*
int main(int argc, char *argv[]) {
    rdc700_float_t src, tgt, result_a, result_b, result_c;
    uint64_t to_float = 0x4C8000000;

    uint64_t numerator;
    uint64_t denominator;

    
    numerator = 132 | to_float;
    set_f36(&numerator, &src);
    rdc700_fnorm(&src, &src);
    denominator = 100 | to_float;
    set_f36(&denominator, &tgt);
    rdc700_fnorm(&tgt, &tgt);
    rdc700_fdiv(&src, &tgt, &result_a);
    rdc700_fnorm(&result_a, &result_a);
    // f80_round_to_f72(&result_a, &result_a);

    numerator = 7 | to_float;
    set_f36(&numerator, &src);
    rdc700_fnorm(&src, &src);
    denominator = 10 | to_float;
    set_f36(&denominator, &tgt);
    rdc700_fnorm(&tgt, &tgt);
    rdc700_fdiv(&src, &tgt, &result_b);
    rdc700_fnorm(&result_b, &result_b);
    // f80_round_to_f72(&result_b, &result_b);

    rdc700_fadd(&result_a, &result_b, &result_c);
    printf("%d %lX\n", ((int) (result_c.sign_exp & 0x7FFF)) - 16383, result_c.signif);

    printf("==== Should be ====\n");

    numerator = 202 | to_float;
    set_f36(&numerator, &src);
    rdc700_fnorm(&src, &src);
    denominator = 100 | to_float;
    set_f36(&denominator, &tgt);
    rdc700_fnorm(&tgt, &tgt);
    rdc700_fdiv(&src, &tgt, &result_a);
    printf("%d %064lb\n", ((int) (result_a.sign_exp & 0x7FFF)) - 16383, result_a.signif);

    return 0;
}

*/
