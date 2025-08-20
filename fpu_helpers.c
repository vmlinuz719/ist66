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
    z->signif = (((x & 0777777777L) | 01000000000L) << 36) | y;
    
    uint8_t exp = (uint8_t) ((x >> 27) & 0xFF);
    uint16_t new_exp = i36_to_exp15(exp8_to_i36(exp));
    if ((x & (1L << 35))) new_exp |= 1 << 15;
    z->signExp = new_exp;
}

