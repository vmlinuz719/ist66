#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>    

#include "cpu.h"
#include "lpt.h"

typedef struct {
    ist66_cu_t *cpu;
    int id, irq;
    
    FILE *file;
    uint8_t buf;
    
    int zbuf_pos;
    uint8_t zbuf[132];
    
    pthread_t thread;
    
    pthread_mutex_t lock;
    pthread_cond_t cmd_cond;
    int running, command, done;
} ist66_lpt_t;

static inline int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0) {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

void *lpt(void *vctx) {
    ist66_lpt_t *ctx = (ist66_lpt_t *) vctx;
    ist66_cu_t *cpu = ctx->cpu;
    
    ctx->running = 1;
    while (ctx->running) {
        pthread_mutex_lock(&ctx->lock);
        while (!ctx->command) {
            pthread_cond_wait(&ctx->cmd_cond, &ctx->lock);
            
        }
        
        int command = ctx->command;
        pthread_mutex_unlock(&ctx->lock);
        
        if (command == -1) {
            ctx->running = 0;
        }
        
        else if (command == 1) {            
            uint8_t ch = ctx->buf;
            ctx->zbuf[ctx->zbuf_pos++] = ch;
            int len = ctx->zbuf_pos;
            if (
                ctx->zbuf_pos == 132
                || ch == 015
                || ch == 012
                || ch == 014
            ) {
                ctx->zbuf_pos = 0;
            }
            
            if (ctx->zbuf_pos == 0) {
                fwrite(&ctx->zbuf, 1, len, ctx->file);
                if (len == 132) {
                    fwrite("\n", 1, 1, ctx->file);
                }
                msleep(4);
            }
            
            pthread_mutex_lock(&ctx->lock);
            ctx->command = 0;
            if (!ctx->done && ctx->zbuf_pos == 0) {
                ctx->done = 1;
                intr_assert(cpu, ctx->irq);
            }
            pthread_mutex_unlock(&ctx->lock);
        }
    }
    
    return NULL;
}

uint64_t lpt_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) {
    ist66_lpt_t *ctx = (ist66_lpt_t *) vctx;
    ist66_cu_t *cpu = ctx->cpu;
    
    if (transfer == 1) {
        ctx->buf = (uint8_t) data;
    }
    
    if (transfer != 14) {
        switch (ctl) {
            case 1: {
                pthread_mutex_lock(&ctx->lock);
                ctx->command = 1;
                if (ctx->done) {
                    ctx->done = 0;
                    intr_release(cpu, ctx->irq);
                }
                pthread_cond_signal(&ctx->cmd_cond);
                pthread_mutex_unlock(&ctx->lock);
            } break;
            case 2: {
                pthread_mutex_lock(&ctx->lock);
                ctx->command = 0;
                if (ctx->done) {
                    ctx->done = 0;
                    intr_release(cpu, ctx->irq);
                }
                pthread_mutex_unlock(&ctx->lock);
            } break;
        }
    }
    
    if (transfer == 14) {
        int status = (ctx->done << 1) | (ctx->command & 1);
        return (uint64_t) status;
    }
    
    if (transfer == 0) {
        return ctx->command & 1;
    }
    
    else return 0;
}

void destroy_lpt(ist66_cu_t *cpu, int id) {
    ist66_lpt_t *ctx = (ist66_lpt_t *) cpu->ioctx[id];
    
    if (ctx->running) {
        pthread_cancel(ctx->thread);
    }
    
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cmd_cond);
    fclose(ctx->file);
    free(ctx);
    
    fprintf(stderr, "LPT: %04o deinitialized\n", id);
}

void init_lpt_any(ist66_cu_t *cpu, int id, int irq, FILE *fd) {
    ist66_lpt_t *ctx = calloc(sizeof(ist66_lpt_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_lpt;
    cpu->io[id] = lpt_io;
    
    ctx->cpu = cpu;
    ctx->id = id;
    ctx->irq = irq;
    ctx->file = fd;
    
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cmd_cond, NULL);
    
    pthread_create(&ctx->thread, NULL, lpt, ctx);
}

void init_lpt(ist66_cu_t *cpu, int id, int irq, FILE *fd) {
    init_lpt_any(cpu, id, irq, fd);
    fprintf(stderr, "LPT: %04o IRQ %02o\n", id, irq);
}

void init_lpt_ex(ist66_cu_t *cpu, int id, int irq, char *fname) {
    FILE *fd = fopen(fname, "wb");
    if (fd == NULL) {
        fprintf(stderr, "LPT: %04o file error\n", id);
        return;
    }
    
    init_lpt_any(cpu, id, irq, fd);
    fprintf(stderr, "LPT: %04o IRQ %02o, file %s\n", id, irq, fname);
}
