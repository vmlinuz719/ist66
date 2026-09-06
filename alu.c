/*
 * alu.c - the ALU itself now lives in include/alu.h so that compute()
 * inlines into cpu.c's instruction handlers. This translation unit is
 * kept so the build and object list stay unchanged.
 */

#include <stdint.h>
#include "alu.h"
