#include <stdint.h>
#include <stdio.h>
#include "alu.h"

static inline uint64_t rotl36(uint64_t a, int b) {
    if (b > 35) b -= 36;
    return ((a << b) | (a >> (36 - b))) & MASK_36;
}

static inline uint64_t rotr36(uint64_t a, int b) {
    if (b > 35) b -= 36;
    return ((a >> b) | (a << (36 - b))) & MASK_36;
}

static inline uint64_t rotl37(uint64_t a, int b) {
    if (b > 36) b -= 37;
    return ((a << b) | (a >> (37 - b))) & MASK_37;
}

static inline uint64_t rotr37(uint64_t a, int b) {
    if (b > 36) b -= 37;
    return ((a >> b) | (a << (37 - b))) & MASK_37;
}

static inline uint64_t rotate(uint64_t a, int b, int rc) {
    if (rc) {
        return b >= 0
            ? rotl37(a, b)
            : rotr37(a, -b);
    } else {
        uint64_t old_carry = a & (1L << 36);
        a &= MASK_36;
        uint64_t result = b >= 0
            ? rotl36(a, b)
            : rotr36(a, -b);
        return result | old_carry;
    }
}

static inline uint64_t maskl(uint64_t a, int b) {
    int64_t mask = (~(0xFFFFFFFFFL)) >> b;
    uint64_t mask_u = (uint64_t) mask;
    return (
        a & (1L << 36)
            ? a | mask_u
            : a & (~mask_u)
    ) & MASK_37;
}

static inline uint64_t maskr(uint64_t a, int b) {
    uint64_t mask = (~1L) << (b > 35 ? 36 : b);
    return (
        a & (1L << 36)
            ? a | mask
            : a & (~mask)
    ) & MASK_37;
}

static inline uint64_t mask(uint64_t a, int b) {
    return (
        b >= 0
            ? maskl(a, b)
            : maskr(a, -b)
    ) & MASK_37;
}

static inline uint64_t rotmask(uint64_t a, int rc, int mk, int rt) {
    return mask(rotate(a, rt, rc), mk);
}

static inline uint64_t skip(uint64_t a, int cond) {
    uint64_t result = 0;
    switch (cond) {
        case 1: result = 1; break;
        case 2: result = !(a & (1L << 36)); break;
        case 3: result = !!(a & (1L << 36)); break;
        case 4: result = !(a & MASK_36); break;
        case 5: result = !!(a & MASK_36); break;
        case 6: result = (!(a & MASK_36)) || (!(a & (1L << 36))); break;
        case 7: result = (!!(a & MASK_36)) && (!!(a & (1L << 36))); break;
    }
    return a | (result << 37);
}

static inline uint64_t opr(uint64_t a, uint64_t b, int c, int op) {
    uint64_t result = 0;
    switch (op) {
        case 0: {
            result = ~a & MASK_36;
        } break;
        case 1: {
            result = (~a + 1) & MASK_36;
        } break;
        case 2: {
            result = a & MASK_36;
        } break;
        case 3: {
            result = (a + 1) & MASK_36;
            if (a == MASK_36) c = !c;
        } break;
        case 4: {
            result = (~a + b) & MASK_36;
            if (a < b) c = !c;
        } break;
        case 5: {
            result = ((~a + 1) + b) & MASK_36;
            if (a <= b) c = !c;
        } break;
        case 6: {
            result = (a + b) & MASK_36;
            if (a + b > MASK_36) c = !c;
        } break;
        case 7: {
            result = (a & b) & MASK_36;
        } break;
        case 10: {
            result = (a | b) & MASK_36;
        } break;
        case 15: {
            result = (a ^ b) & MASK_36;
        } break;
    }
    
    result |= ((uint64_t) c) << 36;
    return result & MASK_37;
}

uint64_t compute(
    uint64_t a, uint64_t b, int c,  // a, b, carry
    int op,                         // opcode
    int ci,                         // carry init
    int cond,                       // skip condition
    int nl,                         // no load
    int rc,                         // rotate through carry
    int mk,                         // mask
    int rt                          // rotate
) {
    /*
    if ((mk & 0x70) == 0x40) {
        mk = -rt;
    }
    */
    
    switch (ci) {
        case 1: c = 0; break;
        case 2: c = 1; break;
        case 3: c = !c; break;
    }
    
    uint64_t result = skip(rotmask(opr(a, b, c, op), rc, mk, rt), cond);
    
    if (nl) {
        return b | (result & (~(MASK_36)));
    } else {
        return result;
    }
}
