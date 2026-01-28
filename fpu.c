#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

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
 */

void rdc700_fmul_n(
    rdc700_float_t *src,
    rdc700_float_t *tgt,
    rdc700_float_t *dst
) {
    uint16_t new_sign_exp = (
        (src->sign_exp & (1 << 15)) ^ (src->sign_exp & (1 << 15))
    );
    
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
        // TODO: signal underflow
        dst->sign_exp = 0;
        dst->signif = 0;
        return;
    } else if (exp_dst > 16383) {
        // TODO: signal overflow
        exp_dst = 16384;
    }
    
    new_sign_exp |= ((uint16_t) (exp_dst + 16383));
    dst->sign_exp = new_sign_exp;
    dst->signif = c;
}

void print_rdc_float(rdc700_float_t *f) {
    int is_neg = f->sign_exp >> 15;
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
    
    printf("%c%d.%s(%d)", is_neg ? '-' : 0, whole, frac_print, 2 * exp);
}

int main(int argc, char *argv) {
    rdc700_float_t src = {
        .sign_exp = 16384,
        .signif = 0x5000000000000000
    };
    
    rdc700_float_t tgt = {
        .sign_exp = 16383,
        .signif = 0x8000000000000000
    };
    
    rdc700_float_t result;
    rdc700_fmul_n(&src, &tgt, &result);
    
    print_rdc_float(&src);
    printf(" x ");
    print_rdc_float(&tgt);
    printf(" = ");
    print_rdc_float(&result);
    printf("\n");
    
    return 0;
}