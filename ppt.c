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
    acr7k_cu_t *cpu;
    int id, irq;
    
    FILE *file;
    uint8_t buf;
    
    pthread_t thread;
    
    pthread_mutex_t lock;
    pthread_cond_t cmd_cond;
    int loaded, running, command, done;
    
    char media_name[256];
} acr7k_ppt_t;

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
    acr7k_ppt_t *ctx = (acr7k_ppt_t *) vctx;
    acr7k_cu_t *cpu = ctx->cpu;
    
    ctx->running = 1;
    int whirr_count = 0;
    
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
            if (ctx->loaded) {
                int ch = fgetc(ctx->file);
                if (ch == EOF) {
                    pthread_mutex_lock(&ctx->lock);
                    fclose(ctx->file);
                    ctx->loaded = 0;
                    pthread_mutex_unlock(&ctx->lock);
                    
                    fprintf(
                        stderr,
                        "PPT: %04o End of tape\n", ctx->id
                    );
                } else {
                    ctx->buf = (uint8_t) ch;
                    whirr_count = 0;
                    
                    pthread_mutex_lock(&ctx->lock);
                    ctx->command = 0;
                    if (!ctx->done) {
                        ctx->done = 1;
                        intr_assert(cpu, ctx->irq);
                    }
                    pthread_mutex_unlock(&ctx->lock);
                }
            } else {
                whirr_count = (whirr_count + 1) % 2048;
                if (whirr_count == 0) {
                    fprintf(
                        stderr,
                        "PPT: %04o whirrrrrrrr...\n", ctx->id
                    );
                }
            }
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
    acr7k_ppt_t *ctx = (acr7k_ppt_t *) vctx;
    acr7k_cu_t *cpu = ctx->cpu;
    
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

int ppt_media (
    void *vctx,
    enum media_cmd command,
    char *argument
) {
    acr7k_ppt_t *ctx = (acr7k_ppt_t *) vctx;
    
    switch (command) {
        case MEDIA_GET: {
            snprintf(argument, sizeof(ctx->media_name), "%s", ctx->media_name);
        } break;
        
        case MEDIA_SET: {
            pthread_mutex_lock(&ctx->lock);
            
            if (ctx->loaded) {
                fclose(ctx->file);
                ctx->loaded = 0;
            }
            
            FILE *fd = fopen(argument, "rb");
            if (fd == NULL) {
                pthread_mutex_unlock(&ctx->lock);
                fprintf(stderr, "PPT: file error\n");
                return -1;
            }
            
            ctx->file = fd;
            ctx->loaded = 1;
            snprintf(ctx->media_name, sizeof(ctx->media_name), "%s", argument);
            
            fprintf(stderr, "PPT: file %s\n", argument);
            
            pthread_mutex_unlock(&ctx->lock);
        } break;
        
        case MEDIA_UNSET: {
            pthread_mutex_lock(&ctx->lock);
            
            if (ctx->loaded) {
                fclose(ctx->file);
                ctx->loaded = 0;
            }
            
            fprintf(stderr, "PPT: unloaded\n");
            
            pthread_mutex_unlock(&ctx->lock);
        } break;
    }
    
    return 0;
}

void destroy_ppt(acr7k_cu_t *cpu, int id) {
    acr7k_ppt_t *ctx = (acr7k_ppt_t *) cpu->ioctx[id];
    
    if (ctx->running) {
        pthread_cancel(ctx->thread);
    }
    
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cmd_cond);
    if (ctx->loaded) fclose(ctx->file);
    free(ctx);
    
    fprintf(stderr, "PPT: %04o deinitialized\n", id);
}

void init_ppt_any(acr7k_cu_t *cpu, int id, int irq, FILE *fd) {
    acr7k_ppt_t *ctx = calloc(sizeof(acr7k_ppt_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_ppt;
    cpu->io[id] = ppt_io;
    cpu->media[id] = ppt_media;
    
    ctx->cpu = cpu;
    ctx->id = id;
    ctx->irq = irq;
    ctx->file = fd;
    if (fd != NULL) ctx->loaded = 1;
    
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cmd_cond, NULL);
    
    pthread_create(&ctx->thread, NULL, ppt, ctx);
}

void init_ppt(acr7k_cu_t *cpu, int id, int irq) {
    init_ppt_any(cpu, id, irq, NULL);
    fprintf(stderr, "PPT: %04o IRQ %02o\n", id, irq);
}

void init_ppt_ex(acr7k_cu_t *cpu, int id, int irq, char *fname) {
    FILE *fd = fopen(fname, "rb");
    if (fd == NULL) {
        fprintf(stderr, "PPT: %04o file error\n", id);
        return;
    }
    
    init_ppt_any(cpu, id, irq, fd);
    fprintf(stderr, "PPT: %04o IRQ %02o, file %s\n", id, irq, fname);
}
