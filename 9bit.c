/*
 * nbt: work with 9-bit files
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    FILE *fd;
    int position, data_valid, data_changed, eof;
    uint8_t current_bytes[7], extra_bits;
} nbt_ctx_t;

/*
 * nbt_flush: write out the buffer to file; use when seeking, reading or
 * closing a 9-bit file
 */

int nbt_flush(nbt_ctx_t *ctx) {
    if (ctx->data_changed) {
        int seek_status = fseek(ctx->fd, (ctx->position / 7) * 8, SEEK_SET);
        if (seek_status) return seek_status;
        
        size_t write_status = fwrite(ctx->current_bytes, 7, 1, ctx->fd);
        if (write_status != 1) return -1;
        
        write_status = fwrite(&(ctx->extra_bits), 1, 1, ctx->fd);
        if (write_status != 1) return -1;
    }
    
    ctx->data_changed = 0;
    
    return 0;
}

/*
 * nbt_seek: seek just the same as a stream (no SEEK_END support)
 */

int nbt_seek(nbt_ctx_t *ctx, int offset, int whence) {
    int new_position =
        whence == SEEK_CUR
            ? ctx->position + offset
            : offset;
    
    if (new_position / 7 != ctx->position / 7 && ctx->data_valid) {
        // in this case we are seeking to another block of 7 bytes
        // flush and invalidate buffer
        int flush_status = nbt_flush(ctx);
        if (flush_status) return flush_status;
        ctx->data_valid = 0;
    }
    
    ctx->position = new_position;
    ctx->eof = 0;
    return 0;
}

/*
 * nbt_buffer: fill the buffer with data
 */

int nbt_buffer(nbt_ctx_t *ctx) {
    uint8_t temp_buf[8];
    
    int seek_status = fseek(ctx->fd, (ctx->position / 7) * 8, SEEK_SET);
    if (seek_status) return seek_status;
    
    size_t result = fread(temp_buf, 8, 1, ctx->fd);
    if (result != 1) {
        if (feof(ctx->fd)) {
            memset(temp_buf, 0, 8);
            ctx->eof = 1;
        } else if (ferror(ctx->fd)) {
            return -1;
        }
    }
    
    memcpy(ctx->current_bytes, temp_buf, 7);
    ctx->extra_bits = temp_buf[7];
    ctx->data_valid = 1;
    ctx->data_changed = 0;
    return 0;
}