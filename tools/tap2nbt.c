/*
 * tap2nbt: convert SimH tape image to "Nineball" format
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <9ball.h>

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
    ctx->fd = dfd;
    ctx->writable = 1;

    char buf[4];
    uint32_t marker;
    int rc = 0;
    
    while (1) {
        size_t marker_result = fread(buf, 4, 1, sfd);
        if (marker_result != 1) {
            if (ferror(sfd)) {
                fprintf(stderr, "Error while reading file %s\n", argv[1]);
                rc = -1;
            }
            goto end;
        }
        
        marker =
            ((uint32_t) buf[0]) |
            (((uint32_t) buf[1]) << 8) |
            (((uint32_t) buf[2]) << 16) |
            (((uint32_t) buf[3]) << 24);
        
        switch (marker >> 28) {
            case 0: {
                if (marker == 0) {
                    if (nbt_write_mark(ctx)) {
                        fprintf
                            (stderr, "Error while writing file %s\n", argv[2]);
                        rc = -1;
                        goto end;
                    }
                }
                else {
                    uint32_t read_size = marker;
                    if ((read_size & 1)) read_size++;
                    
                    uint8_t *record = malloc(read_size);
                    size_t read_result = fread(record, 1, read_size, sfd);
                    if (read_result != read_size) {
                        fprintf
                            (stderr, "Error while reading file %s\n", argv[1]);
                        rc = -1;
                        goto end;
                    }
                    fseek(sfd, 4, SEEK_CUR);
                    
                    if (nbt_write(ctx, marker, record)) {
                        fprintf
                            (stderr, "Error while writing file %s\n", argv[2]);
                        rc = -1;
                        goto end;
                    }
                }
            } break;
            
            case 0xF: {
                if (marker == 0xFFFEFFFF) {
                    // half erase gap
                    if (nbt_write_erase(ctx, 2)) {
                        fprintf
                            (stderr, "Error while writing file %s\n", argv[2]);
                        rc = -1;
                        goto end;
                    }
                    fseek(sfd, -2, SEEK_CUR);
                }
                else if (marker == 0xFFFFFFFE) {
                    // full erase gap
                    if (nbt_write_erase(ctx, 4)) {
                        fprintf
                            (stderr, "Error while writing file %s\n", argv[2]);
                        rc = -1;
                        goto end;
                    }
                }
                else if (marker == 0xFFFFFFFF) {
                    // end of medium
                    if (nbt_write_security(ctx)) {
                        fprintf
                            (stderr, "Error while writing file %s\n", argv[2]);
                        rc = -1;
                        goto end;
                    }
                }
            } break;
            
            // TODO: handle bad/reserved/private data/markers
            // we only skip these for now
            
            case 0x7: break;
            
            default: {
                uint32_t read_size = marker;
                if ((read_size & 1)) read_size++;
                fseek(sfd, read_size + 4, SEEK_CUR);
            }
        }
    }

    end:
    fclose(sfd);
    nbt_flush(ctx);
    fclose(ctx->fd);
    free(ctx);
    return rc;
}