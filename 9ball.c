/*
 * nbt: "Nineball" tape format
 */

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <9ball.h>

/*
 * nbt_flush: write out the buffer to file; use when seeking, reading or
 * closing a 9-bit file
 */

int nbt_flush(nbt_ctx_t *ctx) {
    if (!(ctx->writable)) return -1;
    
    if (ctx->data_changed) {
        int seek_status = fseek(ctx->fd, (ctx->position / 8) * 9, SEEK_SET);
        if (seek_status) return seek_status;
        
        size_t write_status = fwrite(ctx->current_bytes, 8, 1, ctx->fd);
        if (write_status != 1) return -1;
        
        write_status = fwrite(&(ctx->extra_bits), 1, 1, ctx->fd);
        if (write_status != 1) return -1;
    }
    
    ctx->data_changed = 0;
    ctx->eof = 0;
    
    return 0;
}

/*
 * nbt_seek: seek just the same as a stream (no SEEK_END support)
 */

int nbt_seek(nbt_ctx_t *ctx, int offset, int whence) {
    int new_position =
        (whence == SEEK_CUR)
            ? (ctx->position + offset)
            : offset;
    
    if (new_position < 0) new_position = 0;
    
    if ((new_position / 8 != ctx->position / 8) && ctx->data_valid) {
        // in this case we are seeking to another block of 8 bytes
        // flush and invalidate buffer
        int flush_status = nbt_flush(ctx);
        if (flush_status && ctx->writable) return flush_status;
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
    uint8_t temp_buf[9];
    
    int seek_status = fseek(ctx->fd, (ctx->position / 8) * 9, SEEK_SET);
    if (seek_status) {
        return seek_status;
    }
    
    size_t result = fread(temp_buf, 9, 1, ctx->fd);
    if (result != 1) {
        if (feof(ctx->fd)) {
            memset(temp_buf, 0, 9);
            ctx->eof = 1;
        } else if (ferror(ctx->fd)) {
            return -1;
        }
    }
    
    memcpy(ctx->current_bytes, temp_buf, 8);
    ctx->extra_bits = temp_buf[8];
    ctx->data_valid = 1;
    ctx->data_changed = 0;
    return 0;
}

/*
 * nbt_eof: EOF status
 */

int nbt_eof(nbt_ctx_t *ctx) {
    return ctx->eof;
}

/*
 * nbt_eor: EOR status
 */

int nbt_eor(nbt_ctx_t *ctx) {
    return ctx->eor;
}

/*
 * nbt_error: data error status
 */

int nbt_error(nbt_ctx_t *ctx) {
    return ctx->data_error;
}

/*
 * nbt_tell: File position
 */

int nbt_tell(nbt_ctx_t *ctx) {
    return ctx->position;
}

/*
 * nbt_can_write: Is file writable
 */

int nbt_can_write(nbt_ctx_t *ctx) {
    return ctx->writable;
}

/*
 * nbt_getc: get single 9-bit character
 */

int nbt_getc(nbt_ctx_t *ctx) {
    if (!ctx->data_valid) {
        int buffer_status = nbt_buffer(ctx);
        if (buffer_status) return EOF;
    }
    
    int byte_index = ctx->position % 8;
    int result = ctx->current_bytes[byte_index];
    result |= ((int) ((ctx->extra_bits >> byte_index) & 1)) << 8;
    
    nbt_seek(ctx, 1, SEEK_CUR);
    
    return result;
}

/*
 * nbt_rgetc: get single 9-bit character (reverse)
 */

int nbt_rgetc(nbt_ctx_t *ctx) {
    if (!ctx->data_valid) {
        int buffer_status = nbt_buffer(ctx);
        if (buffer_status) return EOF;
    }
    
    nbt_seek(ctx, -1, SEEK_CUR);
    
    int byte_index = ctx->position % 8;
    int result = ctx->current_bytes[byte_index];
    result |= ((int) ((ctx->extra_bits >> byte_index) & 1)) << 8;
    
    return result;
}

/*
 * nbt_putc: write single 9-bit character
 */

int nbt_putc(int c, nbt_ctx_t *ctx) {
    if (!(ctx->writable)) return EOF;
    
    if (!ctx->data_valid) {
        int buffer_status = nbt_buffer(ctx);
        if (buffer_status) return EOF;
    }
    
    int byte_index = ctx->position % 8;
    uint8_t mask = ~(1 << byte_index);
    uint8_t extra_bit = (c >> 8) << byte_index;
    
    ctx->current_bytes[byte_index] = (c & 0xFF);
    ctx->extra_bits &= mask;
    ctx->extra_bits |= extra_bit;
    ctx->data_changed = 1;
    
    nbt_seek(ctx, 1, SEEK_CUR);
    
    return c;
}

int nbt_read(nbt_ctx_t *ctx, int max_len, uint8_t *out) {
    ctx->eor = 0;
    ctx->data_error = 0;
    
    if (nbt_eof(ctx)) return NBT_READ_EOM;
    
    int first_read;
    while ((first_read = nbt_getc(ctx)) < 0x100) {
        if (first_read == 0) {
            nbt_seek(ctx, -1, SEEK_CUR);
            return NBT_READ_EOM;
        }
        else if (first_read == 0x1C) {
            // tape mark
            return NBT_READ_MARK;
        }
    }
    
    nbt_seek(ctx, -1, SEEK_CUR);
    int old_pos = nbt_tell(ctx);
    
    int read_bytes = 0;
    while (read_bytes < max_len) {
        int data = nbt_getc(ctx);
        
        if (data == 0) {
            // unexpected EOM
            nbt_seek(ctx, old_pos, SEEK_SET);
            return NBT_BAD_TAPE;
        }
        else if (data == 0x1E || data == 0x1C) {
            // EOR or unexpected tape mark
            ctx->eor = 1;
            break;
        }
        else if (data == 0x7F) {
            // erase gap
            continue;
        }
        else if (data < 0x100) {
            // bad mark
            ctx->data_error = 1;
            break;
        }
        
        if (out != NULL) out[read_bytes] = data & 0xFF;
        read_bytes++;
    }
    
    while (nbt_getc(ctx) == 0x1E) ctx->eor = 1;
    nbt_seek(ctx, -1, SEEK_CUR);
    
    return read_bytes;
}

int nbt_read_reverse(nbt_ctx_t *ctx, int max_len, uint8_t *out) {
    ctx->eor = 0;
    ctx->data_error = 0;
    
    if (nbt_tell(ctx) == 0) return NBT_READ_BOT;
    
    int first_read;
    while ((first_read = nbt_getc(ctx)) < 0x100) {
        if (nbt_tell(ctx) == 0) {
            return NBT_READ_BOT;
        }
        else if (first_read == 0x1C) {
            // tape mark
            return NBT_READ_MARK;
        }
    }
    
    nbt_seek(ctx, 1, SEEK_CUR);
    
    int read_bytes = 0;
    while (read_bytes < max_len) {
        int data = nbt_rgetc(ctx);
        
        if (data == 0) {
            // unexpected EOM
            nbt_seek(ctx, 1, SEEK_CUR);
            return NBT_READ_EOM;
        }
        else if (data == 0x1E || data == 0x1C) {
            // EOR or expected tape mark
            ctx->eor = 1;
            nbt_seek(ctx, 1, SEEK_CUR);
            return read_bytes;
        }
        else if (data == 0x7F) {
            // erase gap
            continue;
        }
        else if (data < 0x100) {
            // bad mark
            ctx->data_error = 1;
            nbt_seek(ctx, 1, SEEK_CUR);
            return read_bytes;
        }
        
        if (out != NULL) out[read_bytes] = data & 0xFF;
        read_bytes++;
        if (nbt_tell(ctx) == 0) break;
    }
    
    if (nbt_tell(ctx) != 0) {
        int final = nbt_rgetc(ctx);
        if (final == 0x1E || final == 0x1C) ctx->eor = 1;
        nbt_seek(ctx, 1, SEEK_CUR);
    }
    
    return read_bytes;
}

int nbt_write(nbt_ctx_t *ctx, int len, uint8_t *in) {
    ctx->data_error = 0;
    
    for (int i = 0; i < len; i++) {
        int ch = ((int) (in[i])) | 0x100;
        int result = nbt_putc(ch, ctx);
        if (result != ch) {
            ctx->data_error = 1;
            return NBT_BAD_DATA;
        }
    }
    
    int result = nbt_putc(0x1E, ctx);
    if (result != 0x1E) {
        ctx->data_error = 1;
        return NBT_BAD_DATA;
    }
    
    return 0;
}

int nbt_write_mark(nbt_ctx_t *ctx) {
    ctx->data_error = 0;
    int result = nbt_putc(0x1C, ctx);
    if (result != 0x1C) {
        ctx->data_error = 1;
        return NBT_BAD_DATA;
    }
    
    return 0;
}

int nbt_write_security(nbt_ctx_t *ctx) {
    ctx->data_error = 0;
    int result = nbt_putc(0x00, ctx);
    if (result != 0x00) {
        ctx->data_error = 1;
        return NBT_BAD_DATA;
    }
    
    return 0;
}

int nbt_write_erase(nbt_ctx_t *ctx, int len) {
    ctx->data_error = 0;
    for (int i = 0; i < len; i++) {
        int result = nbt_putc(0x7F, ctx);
        if (result != 0x7F) {
            ctx->data_error = 1;
            return NBT_BAD_DATA;
        }
    }
    
    return 0;
}
