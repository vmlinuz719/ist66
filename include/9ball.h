#ifndef _NINEBALL_
#define _NINEBALL_

/*
 * "Nineball" tape format
 */

#include <stdio.h>
#include <stdint.h>

typedef struct {
    FILE *fd;
    int position, data_valid, data_changed, eof, writable;
    uint8_t current_bytes[7], extra_bits;
} nbt_ctx_t;

#define NBT_READ_MARK -1
#define NBT_READ_BOT -2
#define NBT_READ_EOM -3
#define NBT_BAD_TAPE -4

/*
 * nbt_flush: write out the buffer to file; use when seeking, reading or
 * closing a 9-bit file
 */

int nbt_flush(nbt_ctx_t *ctx);

/*
 * nbt_seek: seek just the same as a stream (no SEEK_END support)
 */

int nbt_seek(nbt_ctx_t *ctx, int offset, int whence);

/*
 * nbt_buffer: fill the buffer with data
 */

int nbt_buffer(nbt_ctx_t *ctx);

/*
 * nbt_eof: EOF status
 */

int nbt_eof(nbt_ctx_t *ctx);

/*
 * nbt_tell: File position
 */

int nbt_tell(nbt_ctx_t *ctx);

/*
 * nbt_can_write: Is file writable
 */

int nbt_can_write(nbt_ctx_t *ctx);

/*
 * nbt_getc: get single 9-bit character
 */

int nbt_getc(nbt_ctx_t *ctx);

/*
 * nbt_rgetc: get single 9-bit character (reverse)
 */

int nbt_rgetc(nbt_ctx_t *ctx);

/*
 * nbt_putc: write single 9-bit character
 */

int nbt_putc(int c, nbt_ctx_t *ctx);

int nbt_read(nbt_ctx_t *ctx, int max_len, uint8_t *out);

int nbt_read_reverse(nbt_ctx_t *ctx, int max_len, uint8_t *out);

int nbt_write(nbt_ctx_t *ctx, int len, uint8_t *in);

int nbt_write_mark(nbt_ctx_t *ctx);

int nbt_write_security(nbt_ctx_t *ctx);

int nbt_write_erase(nbt_ctx_t *ctx, int len);

#endif