#ifndef _AWS_
#define _AWS_

#include <stdio.h>
#include <stdint.h>

#define ENDREC 0x20
#define ENDFIL 0x40
#define NEWREC 0x80

typedef struct {
    FILE *fd;
    int bot, eot, bad_tape, protect;
    uint16_t size, prev_size, tag;
} aws_ctx_t;

int aws_read_forward(aws_ctx_t *ctx, uint16_t size, uint8_t *buf);
int aws_seek_backward(aws_ctx_t *ctx);
void aws_rewind(aws_ctx_t *ctx);
void aws_unwind(aws_ctx_t *ctx);
int aws_write_record(aws_ctx_t *ctx, uint16_t size, uint8_t *buf);
int aws_write_eof(aws_ctx_t *ctx);

int aws_init(aws_ctx_t *ctx, char *fn, int protect);
void aws_close(aws_ctx_t *ctx);

#endif
