/*
 * 9bit: helper for 9-bit file I/O
 */

#include <stdio.h>
#include <stdint.h>

typedef struct {
    FILE *fd;
    int word_index, byte_index;
    uint8_t current_bytes[7];
    uint8_t current_extra_bits;
} 9bit_ctx_t;
