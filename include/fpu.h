#ifndef _FMATH_
#define _FMATH_

#include "softfloat.h"

void ist66f_to_extF80M(uint64_t x, uint64_t y, extFloat80_t *z);
int extF80M_to_ist66f72(extFloat80_t *x, uint64_t *y, uint64_t *z);
int extF80M_to_ist66f36(extFloat80_t *x, uint64_t *y, int rnd);

#endif

