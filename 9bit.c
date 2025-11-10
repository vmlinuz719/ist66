/*
 * nbt: work with 9-bit files
 */

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

/*
 * nbt_iseof: EOF status
 */

int nbt_iseof(nbt_ctx_t *ctx) {
    return ctx->eof;
}

/*
 * nbt_getc: get single 9-bit character
 */

int nbt_getc(nbt_ctx_t *ctx) {
    if (!ctx->data_valid) {
        int buffer_status = nbt_buffer(ctx);
        if (buffer_status) return EOF;
    }
    
    int byte_index = ctx->position % 7;
    int result = ctx->current_bytes[byte_index];
    result |= ((int) ((ctx->extra_bits >> byte_index) & 1)) << 8;
    
    nbt_seek(ctx, 1, SEEK_CUR);
    
    return result;
}

/*
 * nbt_putc: write single 9-bit character
 */

int nbt_putc(int c, nbt_ctx_t *ctx) {
    if (!ctx->data_valid) {
        int buffer_status = nbt_buffer(ctx);
        if (buffer_status) return EOF;
    }
    
    int byte_index = ctx->position % 7;
    uint8_t mask = ~(1 << byte_index);
    uint8_t extra_bit = (c >> 8) << byte_index;
    
    ctx->current_bytes[byte_index] = (c & 0xFF);
    ctx->extra_bits &= mask;
    ctx->extra_bits |= extra_bit;
    ctx->data_changed = 1;
    
    nbt_seek(ctx, 1, SEEK_CUR);
    
    return c;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return -1;
    }
    
    FILE *fd;
    if ((fd = fopen(argv[1], "wb+")) == NULL) {
        fprintf(stderr, "Error opening file %s\n", argv[1]);
        return -1;
    }
    
    nbt_ctx_t *ctx = calloc(1, sizeof(nbt_ctx_t));
    ctx->fd = fd;
    
    char data[] = "hello world\n";
    for (int i = 0; i < sizeof(data); i++) {
        nbt_putc(((int) data[i]) | ((i & 1) << 8), ctx);
    }
    
    nbt_seek(ctx, 0, SEEK_SET);
    
    // spongebob case demo
    
    int read_data = 0;
    while (((read_data = nbt_getc(ctx)) & 0xFF) != 0) {
        char c = (read_data & (1 << 8))
            ? toupper(read_data & 0xFF)
            : tolower(read_data & 0xFF);
        printf("%c", c);
    }
    
    nbt_flush(ctx);
    fclose(ctx->fd);
    free(ctx);
    return 0;
}