#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>    

#include "cpu.h"
#include "pch.h"

typedef struct {
    ist66_cu_t *cpu;
    int irq;
    
    FILE *file;
    uint8_t buf;
    
    pthread_t thread;
    
    pthread_mutex_t lock;
    pthread_cond_t cmd_cond;
    int running, command, done;
} ist66_pch_t;

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

void *pch(void *vctx) {
    ist66_pch_t *ctx = (ist66_pch_t *) vctx;
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
            fputc(ctx->buf, ctx->file);
            msleep(16);
            
            pthread_mutex_lock(&ctx->lock);
            if (!ctx->done) {
                ctx->command = 0;
                ctx->done = 1;
                intr_assert(cpu, ctx->irq);
            }
            pthread_mutex_unlock(&ctx->lock);
        }
    }
    
    return NULL;
}

uint64_t pch_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) {
    ist66_pch_t *ctx = (ist66_pch_t *) vctx;
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
    
    else return 0;
}

void destroy_pch(ist66_cu_t *cpu, int id) {
    ist66_pch_t *ctx = (ist66_pch_t *) cpu->ioctx[id];
    
    if (ctx->running) {
        pthread_cancel(ctx->thread);
    }
    
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cmd_cond);
    fclose(ctx->file);
    free(ctx);
    
    fprintf(stderr, "EXIT: pch on %04o\n", id);
}

void init_pch(ist66_cu_t *cpu, int id) {
    ist66_pch_t *ctx = calloc(sizeof(ist66_pch_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_pch;
    cpu->io[id] = pch_io;
    
    ctx->cpu = cpu;
    ctx->irq = 6;
    ctx->file = stdout;
    
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cmd_cond, NULL);
    
    pthread_create(&ctx->thread, NULL, pch, ctx);
    
    fprintf(stderr, "INIT: pch on %04o\n", id);
}