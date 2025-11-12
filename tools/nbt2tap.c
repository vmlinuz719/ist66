/*
 * nbt2tap: convert "Nineball" tape image to SimH .tap format
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <9ball.h>

#define BUF_SIZE 64

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <src> <dst>\n", argv[0]);
        return -1;
    }
    
    FILE *sfd;
    if ((sfd = fopen(argv[1], "rb")) == NULL) {
        fprintf(stderr, "Error opening file %s\n", argv[1]);
        return -1;
    }
    
    FILE *dfd;
    if ((dfd = fopen(argv[2], "wb+")) == NULL) {
        fprintf(stderr, "Error opening file %s\n", argv[2]);
        fclose(sfd);
        return -1;
    }
    
    nbt_ctx_t *ctx = calloc(1, sizeof(nbt_ctx_t));
    ctx->fd = sfd;
    ctx->writable = 0;

    uint8_t buf[BUF_SIZE];
    int rc = 0;
    
    while (1) {
        int read_data = nbt_read(ctx, BUF_SIZE, buf);
        
        if (read_data == NBT_BAD_TAPE) {
            fprintf(stderr, "Error while reading file %s\n", argv[1]);
            rc = -1;
            goto end;
        }
        else if (read_data == NBT_READ_EOM) {
            break;
        }
        else if (read_data == NBT_READ_MARK) {
            uint8_t mark[] = {0, 0, 0, 0};
            if (fwrite(mark, 4, 1, dfd) != 1) {
                fprintf(stderr, "Error while writing file %s\n", argv[2]);
                rc = -1;
                goto end;
            }
        }
        else {
            long header_position = ftell(dfd);
            fseek(dfd, 4, SEEK_CUR);
            int record_len = read_data;
            if (fwrite(buf, 1, read_data, dfd) != read_data) {
                fprintf(stderr, "Error while writing file %s\n", argv[2]);
                rc = -1;
                goto end;
            }
            
            while (!nbt_eor(ctx)) {
                read_data = nbt_read(ctx, BUF_SIZE, buf);
                if (read_data < 0 && read_data != NBT_READ_MARK) {
                    fprintf(stderr, "Error while reading file %s\n", argv[1]);
                    rc = -1;
                    goto end;
                }
                record_len += read_data;
                if (fwrite(buf, 1, read_data, dfd) != read_data) {
                    fprintf(stderr, "Error while writing file %s\n", argv[2]);
                    rc = -1;
                    goto end;
                }
            }
            
            fseek(dfd, header_position, SEEK_SET);
            uint8_t header[] = {
                (record_len & 0xFF),
                ((record_len >> 8) & 0xFF),
                ((record_len >> 16) & 0xFF),
                ((record_len >> 24) & 0xFF)
            };
            if (fwrite(header, 4, 1, dfd) != 1) {
                fprintf(stderr, "Error while writing file %s\n", argv[2]);
                rc = -1;
                goto end;
            }
            fseek(dfd, record_len, SEEK_CUR);
            if ((record_len & 1)) {
                if (fputc(0, dfd) == EOF) {
                    fprintf(stderr, "Error while writing file %s\n", argv[2]);
                    rc = -1;
                    goto end;
                }
            }
            if (fwrite(header, 4, 1, dfd) != 1) {
                fprintf(stderr, "Error while writing file %s\n", argv[2]);
                rc = -1;
                goto end;
            }
        }
    }

    end:
    fclose(dfd);
    fclose(ctx->fd);
    free(ctx);
    return rc;
}