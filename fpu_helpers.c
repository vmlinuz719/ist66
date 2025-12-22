#include <stdint.h>

#include "alu.h"

#include "softfloat_types.h"

uint64_t exp8_to_i36(uint8_t exp) {
    int64_t result = (int64_t) exp;
    result -= 127;
    return result & MASK_36;
}

uint64_t exp15_to_i36(uint16_t exp) {
    int64_t result = (int64_t) exp;
    result -= 16383;
    return result & MASK_36;
}

uint8_t i36_to_exp8(uint64_t exp) {
    int64_t exp_s = (int64_t) (EXT36(exp));
    if (exp_s < -127) return 0;
    else if (exp_s > 128) return 0xFF;
    else {
        exp_s += 127;
        return (uint8_t) exp_s;
    }
}

uint16_t i36_to_exp15(uint64_t exp) {
    int64_t exp_s = (int64_t) (EXT36(exp));
    if (exp_s < -16383) return 0;
    else if (exp_s > 16384) return 0x7FFF;
    else {
        exp_s += 16383;
        return ((uint64_t) exp_s) & 0x7FFF;
    }
}

// TODO: consider enhanced subnormal support

void ist66f_to_extF80M(uint64_t x, uint64_t y, extFloat80_t *z) {
    z->signif = ((x & 0777777777L) << 36) | y;
    
    uint8_t exp = (uint8_t) ((x >> 27) & 0xFF);
    uint16_t new_exp = i36_to_exp15(exp8_to_i36(exp));
    if (new_exp) z->signif |= ((uint64_t) 1L) << 63;
    if ((x & (1L << 35))) new_exp |= 1 << 15;
    z->signExp = new_exp;
}

int rndsig(uint64_t src, uint64_t *dst) {
    int orig_leading = src >> 63;
    uint64_t to_truncate = src & MASK_36;
    
    if (
        (to_truncate == 1L << 35 && (src & (1L << 36))) ||
        (to_truncate > 1L << 35)
    ) {
        // round to nearest, ties to even
        src += (1L << 36);
    }
    
    *dst = (src >> 36) & 0777777777L;
    
    int new_leading = src >> 63;
    return (!!(orig_leading ^ new_leading));
}

int extF80M_to_ist66f72(extFloat80_t *x, uint64_t *y, uint64_t *z) {
    if ((x->signExp & 0x7FFF) == 0x7FFF) {
        // NaN or infinity
        *y = (0xFFL << 27) | ((x->signif >> 36) & 0777777777L);
        *z = x->signif & MASK_36;
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 0;
    }
    
    uint8_t new_exp = i36_to_exp8(exp15_to_i36(x->signExp & 0x7FFF));
    
    if (new_exp == 0 && (x->signif)) {
        // underflow
        *y = 0;
        *z = 0;
        return -1;
    }
    else if (new_exp == 0xFF) {
        // overflow
        *y = (0xFFL << 27);
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        *z = 0;
        return 1;
    }
    else {
        // value
        *y = (((uint64_t) new_exp) << 27) | ((x->signif >> 36) & 0777777777L);
        
        *z = x->signif & MASK_36;
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 0;
    }
}

int extF80M_to_ist66f36(extFloat80_t *x, uint64_t *y, int rnd) {
    if ((x->signExp & 0x7FFF) == 0x7FFF) {
        // NaN or infinity
        *y = (0xFFL << 27) | ((x->signif >> 36) & 0777777777L);
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 0;
    }
    
    uint8_t new_exp = i36_to_exp8(exp15_to_i36(x->signExp & 0x7FFF));
    
    if (new_exp == 0 && (x->signif)) {
        // underflow
        *y = 0;
        return -1;
    }
    else if (new_exp == 0xFF) {
        // overflow
        *y = (0xFFL << 27);
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 1;
    }
    else {
        // value
        int normalized = 0;
        uint64_t new_signif;
        if (rnd) {
            normalized = rndsig(x->signif, &new_signif);
        } else {
            new_signif = (x->signif >> 36) & 0777777777L;
        }
        
        if (normalized) new_exp++;
        if (new_exp == 0xFF) {
            // overflow
            *y = (0xFFL << 27);
            if ((x->signExp & (1 << 15))) {
                *y |= 1L << 35;
            }
            return 1;
        }
        
        *y = (((uint64_t) new_exp) << 27) | new_signif;
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 0;
    }
}

/*
int main(int argc, char *argv[]) {
    extFloat80_t f;
    f.signExp = 16510;
    f.signif = 0UL - 1;
    
    uint64_t a, b, c;
    
    int z = extF80M_to_ist66f72(&f, &a, &b);
    printf(
        "%02lX:%016lX %c %c\n",
        (a >> 27) & 0xFF,
        ((a & 0777777777L) << 36) | b,
        (a >> 35) ? '-' : ' ',
        z ? '*' : ' '
    );
    
    z = extF80M_to_ist66f36(&f, &a, 0);
    printf(
        "%02lX:%016lX %c %c\n",
        (a >> 27) & 0xFF,
        ((a & 0777777777L) << 36),
        (a >> 35) ? '-' : ' ',
        z ? '*' : ' '
    );
    
    z = extF80M_to_ist66f36(&f, &a, 1);
    printf(
        "%02lX:%016lX %c %c\n",
        (a >> 27) & 0xFF,
        ((a & 0777777777L) << 36),
        (a >> 35) ? '-' : ' ',
        z ? '*' : ' '
    );
    
    return 0;
}
*/
