/*
 * nbt: helper for 9-bit file I/O
 */

#include <stdio.h>
#include <stdint.h>

typedef struct {
    FILE *fd;
    int word_index, byte_index, position, data_valid, data_changed;
    uint8_t current_bytes[7], extra_bits;
} nbt_ctx_t;

int nbt_flush(nbt_ctx_t *ctx) {
    if (ctx->data_changed) {
        int seek_status = fseek(ctx->fd, ctx->word_index * 8, SEEK_SET);
        if (seek_status) return seek_status;
        
        size_t write_status = fwrite(ctx->current_bytes, 7, 1, ctx->fd);
        if (write_status != 1) return -1;
        
        write_status = fwrite(&(ctx->extra_bits), 1, 1, ctx->fd);
        if (write_status != 1) return -1;
    }
    
    ctx->data_changed = 0;
    
    return 0;
}