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
#define EXT13(x) ((x) & (1L << 12) ? (x) | 0xFFFFFFFFFFFFE000 : (x))
#define EXT36(x) ((x) & (1L << 35) ? (x) | 0xFFFFFFF000000000 : (x))

#include <stdint.h>

uint64_t compute(
    uint64_t a, uint64_t b, int c,  // a, b, carry
    int op,                         // opcode
    int ci,                         // carry init
    int cond,                       // skip condition
    int rc,                         // rotate through carry
    int mk,                         // mask
    int rt                          // rotate
);

static inline void xmul(
    uint64_t a,
    uint64_t b,
    uint64_t *rl,
    uint64_t *rh
) {
    // TODO: need signed option
    /*
    int negate = 0;
    
    if (a & (1L << 35)) {
        negate ^= 1;
        a = ((~a) + 1) & MASK_36;
    }
    
    if (b & (1L << 35)) {
        negate ^= 1;
        b = ((~b) + 1) & MASK_36;
    }
    */
    
    uint64_t ah = (a >> 18) & 0777777;
    uint64_t al = a & 0777777;
    uint64_t bh = (b >> 18) & 0777777;
    uint64_t bl = b & 0777777;
    
    uint64_t blal = bl * al;
    uint64_t blah = bl * ah;
    uint64_t bhal = bh * al;
    uint64_t bhah = bh * ah;
    
    // AAAAAAaaaaaa * BBBBBBbbbbbb
    // = (aaaaaa * bbbbbb)
    // + (AAAAAA * bbbbbb) << 18
    // + (aaaaaa * BBBBBB) << 18
    // + (AAAAAA * BBBBBB) << 36
    
    *rl = blal + ((blah & 0777777) << 18) + ((bhal & 0777777) << 18);
    *rh = bhah + (blah >> 18) + (bhal >> 18);
    *rh += (*rl >> 36);
    *rl &= MASK_36;
    
    /*
    if (negate) {
        *rl = (~(*rl) + 1) & MASK_37;
        *rh = (~(*rh)) & MASK_36;
        *rh += *rl >> 36;
        *rl &= MASK_36;
    }
    */
}

#endif

