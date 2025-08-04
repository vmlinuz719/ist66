#ifndef _MATH_
#define _MATH_

#define MASK_36 0xFFFFFFFFFL
#define MASK_37 0x1FFFFFFFFFL // bit 36 carry
#define MASK_38 0x3FFFFFFFFFL // bit 37 test

#define CARRY(x) (!!((x) & (1L << 36)))
#define SKIP(x) (!!((x) & (1L << 37)))

#define EXT6(x) ((x) & (1L << 5) ? (x) | 0xFFFFFFFFFFFFFFC0 : (x))
#define EXT7(x) ((x) & (1L << 6) ? (x) | 0xFFFFFFFFFFFFFF80 : (x))
#define EXT18(x) ((x) & (1L << 17) ? (x) | 0xFFFFFFFFFFFC0000 : (x))

#include <stdint.h>
#include <stdio.h>

uint64_t compute(
    uint64_t a, uint64_t b, int c,  // a, b, carry
    int op,                         // opcode
    int ci,                         // carry init
    int cond,                       // skip condition
    int nl,                         // no load
    int rc,                         // rotate through carry
    int mk,                         // mask
    int rt                          // rotate
);

static inline uint64_t exec_aa(
    uint64_t inst,
    uint64_t a, uint64_t b, int c
) {
    int op = (int) ((inst >> 20) & 0x7);
    op |= (int) ((inst >> 29) & 0x8);
    int ci = (int) ((inst >> 18) & 0x3);
    int cond = (int) ((inst >> 15) & 0x7);
    int nl = (int) ((inst >> 14) & 0x1);
    int rc = (int) ((inst >> 31) & 0x1);
    
    uint64_t mk_u = ((inst >> 7) & 0x3F);
    uint64_t rt_u = (inst & 0x3F);
    
    int mk = (int) (EXT7(mk_u));
    int rt = (int) (EXT7(rt_u));
    
    uint64_t result = compute(a, b, c, op, ci, cond, nl, rc, mk, rt);
    return result;
}

#endif

