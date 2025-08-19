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