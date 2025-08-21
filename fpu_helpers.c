/**
 * @file fpu_helpers.c
 * Helper functions for float conversion
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "alu.h"

#include "softfloat.h"

/**
 * @brief Convert bias-8 to int36
 * @param exp Exponent
 * @return int36 stored in uint64_t
 */
uint64_t exp8_to_i36(uint8_t exp) {
    int64_t result = (int64_t) exp;
    result -= 127;
    return result & MASK_36;
}

/**
 * @brief Convert bias-11 to int36
 * @param exp Exponent
 * @return int36 stored in uint64_t
 */
uint64_t exp11_to_i36(uint16_t exp) {
    int64_t result = (int64_t) exp;
    result -= 1023;
    return result & MASK_36;
}

/**
 * @brief Convert bias-15 to int36
 * @param exp Exponent
 * @return int36 stored in uint64_t
 */
uint64_t exp15_to_i36(uint16_t exp) {
    int64_t result = (int64_t) exp;
    result -= 16383;
    return result & MASK_36;
}

/**
 * @brief Convert int36 to bias-8
 * @param exp Exponent as int36 stored in uint64_t
 * @return Bias-8 exponent (0x00 if too small, 0xFF if too big)
 */
uint8_t i36_to_exp8(uint64_t exp) {
    int64_t exp_s = (int64_t) (EXT36(exp));
    if (exp_s < -127) return 0;
    else if (exp_s > 128) return 0xFF;
    else {
        exp_s += 127;
        return ((uint64_t) exp_s) & 0xFF;
    }
}

/**
 * @brief Convert int36 to bias-11
 * @param exp Exponent as int36 stored in uint64_t
 * @return Bias-11 exponent (0x000 if too small, 0x7FF if too big)
 */
uint16_t i36_to_exp11(uint64_t exp) {
    int64_t exp_s = (int64_t) (EXT36(exp));
    if (exp_s < -1023) return 0;
    else if (exp_s > 1024) return 0x7FF;
    else {
        exp_s += 1023;
        return ((uint64_t) exp_s) & 0x7FF;
    }
}

/**
 * @brief Convert int36 to bias-15
 * @param exp Exponent as int36 stored in uint64_t
 * @return Bias-15 exponent (0x0000 if too small, 0x7FFF if too big)
 */
uint16_t i36_to_exp15(uint64_t exp) {
    int64_t exp_s = (int64_t) (EXT36(exp));
    if (exp_s < -16383) return 0;
    else if (exp_s > 16384) return 0x7FFF;
    else {
        exp_s += 16383;
        return ((uint64_t) exp_s) & 0x7FFF;
    }
}

/**
 * @brief Convert IST-66 float to extF80M
 * @param x float36 stored in uint64_t
 * @param y remaining significand bits (or 0 for float36)
 * @param z Berkeley SoftFloat extFloat80_t *
 */
void ist66f_to_extF80M(uint64_t x, uint64_t y, extFloat80_t *z) {
    z->signif = ((x & 0777777777L) << 36) | y;
    
    uint8_t exp = (uint8_t) ((x >> 27) & 0xFF);
    uint16_t new_exp = i36_to_exp15(exp8_to_i36(exp));
    if (new_exp) z->signif |= 1L << 63;
    if ((x & (1L << 35))) new_exp |= 1 << 15;
    z->signExp = new_exp;
}

/**
 * @brief Round a 64-bit significand to a 27-bit one
 *
 * Note: the 64-bit significand does NOT have an implicit leading one. The
 * result DOES.
 *
 * @param x uint64_t significand
 * @param y address to store 27-bit result
 * @return 1 if normalization occurs else 0
 */
int rndsig(uint64_t src, uint64_t *dst) {
    int orig_leading = src >> 63;
    uint64_t to_truncate = src & MASK_36;
    
    uint64_t result = src & (~((uint64_t) MASK_36));
    if (
        (to_truncate == 1L << 36 && (src & (1L << 37))) ||
        (to_truncate > 1L << 36)
    ) {
        // round to nearest, ties to even
        src += (1L << 37);
    }
    
    *dst = (src >> 36) & 0777777777L;
    
    int new_leading = src >> 63;
    return (!!(orig_leading ^ new_leading));
}

/**
 * @brief Convert extF80M to IST-66 float72
 * @param x Berkeley SoftFloat extFloat80_t *
 * @param y first word of float72 stored in uint64_t
 * @param z remaining significand bits
 * @return 1 on overflow, -1 on underflow, else 0
 */
int extF80M_to_ist66f72(extFloat80_t *x, uint64_t *y, uint64_t *z) {
    if (x->signExp & 0x7FFF == 0x7FFF) {
        // NaN or infinity
        *y = (0xFFL << 27) | ((x->signif >> 36) & 0777777777L);
        *z = x->signif & MASK_36;
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 0;
    }
    
    int sign = !!(x->signExp & (1 << 15));
    uint8_t new_exp = i36_to_exp8(exp15_to_i36(x->signExp & 0x7FFF));
    
    if (new_exp == 0 && (x->signif & (1L << 63))) {
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

/**
 * @brief Convert extF80M to IST-66 float36
 * @param x Berkeley SoftFloat extFloat80_t *
 * @param y float36 stored in uint64_t
 * @param rnd Whether to round result
 * @return 1 on overflow, -1 on underflow, else 0
 */
int extF80M_to_ist66f36(extFloat80_t *x, uint64_t *y, int rnd) {
    if (x->signExp & 0x7FFF == 0x7FFF) {
        // NaN or infinity
        *y = (0xFFL << 27) | ((x->signif >> 36) & 0777777777L);
        if ((x->signExp & (1 << 15))) {
            *y |= 1L << 35;
        }
        return 0;
    }
    
    int sign = !!(x->signExp & (1 << 15));
    uint8_t new_exp = i36_to_exp8(exp15_to_i36(x->signExp & 0x7FFF));
    
    if (new_exp == 0 && (x->signif & (1L << 63))) {
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
