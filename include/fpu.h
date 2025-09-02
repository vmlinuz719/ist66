#ifndef _FMATH_
#define _FMATH_

#include <stdint.h>
#include "softfloat.h"

void ist66f_to_extF80M(uint64_t x, uint64_t y, extFloat80_t *z);
int extF80M_to_ist66f72(extFloat80_t *x, uint64_t *y, uint64_t *z);
int extF80M_to_ist66f36(extFloat80_t *x, uint64_t *y, int rnd);

uint64_t exp15_to_i36(uint16_t exp);
uint16_t i36_to_exp15(uint64_t exp);

#endif

