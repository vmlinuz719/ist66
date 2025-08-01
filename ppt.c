#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>    

#include "cpu.h"
#include "ppt.h"

typedef struct {
    ist66_cu_t *cpu;
    int id, irq;
    
    FILE *file;
    uint8_t buf;
    
    pthread_t thread;
    
    pthread_mutex_t lock;
    pthread_cond_t cmd_cond;
    int running, command, done;
} ist66_ppt_t;

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

void *ppt(void *vctx) {
    ist66_ppt_t *ctx = (ist66_ppt_t *) vctx;
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
            msleep(2);
            int ch = fgetc(ctx->file);
            if (ch == EOF) {
                fclose(ctx->file);
                ctx->running = 0;
                ctx->buf = 0;
                fprintf(
                    stderr,
                    "/DEV-I-UNIT %04o PPT END OF TAPE\n", ctx->id
                );
            } else {
                ctx->buf = (uint8_t) ch;
            }
            
            pthread_mutex_lock(&ctx->lock);
            ctx->command = 0;
            if (!ctx->done) {
                ctx->done = 1;
                intr_assert(cpu, ctx->irq);
            }
            pthread_mutex_unlock(&ctx->lock);
        }
    }
    
    return NULL;
}

uint64_t ppt_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) {
    ist66_ppt_t *ctx = (ist66_ppt_t *) vctx;
    ist66_cu_t *cpu = ctx->cpu;
    
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
    
    else if (transfer == 0) {
        return ctx->buf;
    }
    
    else return 0;
}

void destroy_ppt(ist66_cu_t *cpu, int id) {
    ist66_ppt_t *ctx = (ist66_ppt_t *) cpu->ioctx[id];
    
    if (ctx->running) {
        pthread_cancel(ctx->thread);
    }
    
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cmd_cond);
    fclose(ctx->file);
    free(ctx);
    
    fprintf(stderr, "/DEV-I-UNIT %04o PPT CLOSED\n", id);
}

void init_ppt_any(ist66_cu_t *cpu, int id, int irq, FILE *fd) {
    ist66_ppt_t *ctx = calloc(sizeof(ist66_ppt_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_ppt;
    cpu->io[id] = ppt_io;
    
    ctx->cpu = cpu;
    ctx->id = id;
    ctx->irq = irq;
    ctx->file = fd;
    
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cmd_cond, NULL);
    
    pthread_create(&ctx->thread, NULL, ppt, ctx);
}

void init_ppt(ist66_cu_t *cpu, int id, int irq) {
    init_ppt_any(cpu, id, irq, stdin);
    fprintf(stderr, "/DEV-I-UNIT %04o PPT IRQ %02o STDIN\n", id, irq);
}

void init_ppt_ex(ist66_cu_t *cpu, int id, int irq, char *fname) {
    FILE *fd = fopen(fname, "rb");
    if (fd == NULL) {
        fprintf(stderr, "/DEV-E-UNIT %04o PPT FILE ERROR\n", id);
        return;
    }
    
    init_ppt_any(cpu, id, irq, fd);
    fprintf(stderr, "/DEV-I-UNIT %04o PPT IRQ %02o %s\n", id, irq, fname);
}