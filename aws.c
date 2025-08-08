#include <stdio.h>
#include <stdint.h>

#include "aws.h"

static int read_header(aws_ctx_t *ctx) {
    uint16_t header[3];
    int result = fread(header, sizeof(uint16_t), 3, ctx->fd);
    if (result != 3) {
        if (feof(ctx->fd) && result == 0) {
            ctx->bot = 0;
            ctx->eot = 1;
        } else {
            ctx->bad_tape = 1;
        }
        return -1;
    }
    
    ctx->size = header[0];
    ctx->prev_size = header[1];
    ctx->tag = header[2];

    return 0;
}

int aws_read_forward(aws_ctx_t *ctx, uint16_t size, uint8_t *buf) {
    if (ctx->eot) return 0;
    
    int count = size >= ctx->size
        ? ctx->size
        : size;
    
    ctx->bot = 0;
    
    int result;
    if (count) {
        result = fread(buf, 1, count, ctx->fd);
    } else {
        result = 0;
    }
    
    if (result < count) {
        ctx->bad_tape = 1;
        return result;
    }
    
    if (count < ctx->size) {
        fseek(ctx->fd, ctx->size - count, SEEK_CUR);
    }
    
    read_header(ctx);
    return result;
}

int aws_seek_backward(aws_ctx_t *ctx) {
    if (ctx->bot) return 0;
    
    int result = ctx->prev_size;
    
    if (ctx->eot) {
        fseek(ctx->fd, -6, SEEK_END);
        ctx->eot = 0;
    }
    
    else {
        fseek(ctx->fd, -(result + 12), SEEK_CUR);
    }
    
    if (ftell(ctx->fd) == 0) ctx->bot = 1;
    
    read_header(ctx);
    return result;
}

void aws_rewind(aws_ctx_t *ctx) {
    ctx->bot = 1;
    ctx->eot = 0;
    fseek(ctx->fd, 0, SEEK_SET);
    
    read_header(ctx);
}

void aws_unwind(aws_ctx_t *ctx) {
    ctx->bot = 0;
    ctx->eot = 1;
    fseek(ctx->fd, 0, SEEK_END);
}

int aws_write_record(aws_ctx_t *ctx, uint16_t size, uint8_t *buf) {
    if (ctx->protect || ctx->eot) return -1;
    
    while ((ctx->tag & ENDFIL) && !ctx->eot) {
        // tape at EOF, we want to read the next header
        read_header(ctx);
    }
    
    if (ctx->eot) {
        aws_seek_backward(ctx);
    }
    
    uint16_t header[3];
    header[0] = size;
    
    if (ctx->tag & NEWREC) {
        // overwrite unsupported
        aws_read_forward(ctx, 0, NULL);
        return 0;
    }
    
    else if (ctx->tag & ENDREC) {
        fseek(ctx->fd, -6, SEEK_CUR);
        header[1] = ctx->prev_size;
        header[2] = ctx->tag | NEWREC;
    }
    
    else if (ctx->tag & ENDFIL) {
        header[1] = 0;
        header[2] = ENDREC | NEWREC;
    }
    
    fwrite(header, sizeof(uint16_t), 3, ctx->fd);
    
    fwrite(buf, sizeof(uint8_t), size, ctx->fd);
    
    header[0] = 0;
    header[1] = size;
    header[2] = ENDREC;
    fwrite(header, sizeof(uint16_t), 3, ctx->fd);
    
    ctx->size = header[0];
    ctx->prev_size = header[1];
    ctx->tag = header[2];
    
    ctx->bot = 0;
    return size;
}

int aws_write_eof(aws_ctx_t *ctx) {
    if (ctx->protect || ctx->eot) return -1;
    
    uint16_t header[3];
    
    if (ctx->tag & ENDREC) {
        fseek(ctx->fd, -6, SEEK_CUR);
    }
    
    header[0] = ctx->size;
    header[1] = ctx->tag & ENDFIL ? 0 : ctx->prev_size;
    header[2] = ENDFIL;
    fwrite(header, sizeof(uint16_t), 3, ctx->fd);
    
    ctx->size = header[0];
    ctx->prev_size = header[1];
    ctx->tag = header[2];
    
    ctx->bot = 0;
    
    return 0;
}

static int aws_create(char *fn) {
    FILE *fd = fopen(fn, "wb");
    
    if (fd == NULL) return -1;
    
    uint16_t header[3];
    header[0] = 0;
    header[1] = 0;
    header[2] = ENDREC;
    fwrite(header, sizeof(uint16_t), 3, fd);
    fclose(fd);
    
    return 0;
}

int aws_init(aws_ctx_t *ctx, char *fn, int protect) {
    FILE *fd = fopen(fn, "rb+");
    if (fd == NULL && !protect) {
        aws_create(fn);
        fd = fopen(fn, "rb+");
    }
    
    ctx->fd = fd;
    ctx->bot = 1;
    ctx->eot = 0;
    ctx->protect = protect;
    
    if (fd == NULL) {
        ctx->bad_tape = 1;
        return -1;
    } else {
        ctx->bad_tape = 0;
        return read_header(ctx);
    }
}

void aws_close(aws_ctx_t *ctx) {
    fclose(ctx->fd);
    ctx->fd = NULL;
}

