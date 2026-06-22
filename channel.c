#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>    

#include "cpu.h"

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

typedef struct {
    pthread_t thread;
    
    int running;
    
    // use status_lock
    pthread_cond_t cmd_cond;
    int command, done;
} acr7k_subch_t;

typedef struct {
    acr7k_cu_t *cpu;
    int id, irq;
    
    pthread_mutex_t status_lock;
    acr7k_subch_t subchannel[16];
} acr7k_msch_t;

typedef struct {
    acr7k_msch_t *msch;
    int subchannel;
} msch_arg_t;

void *subch(void *vctx) {
    msch_arg_t *arg = (msch_arg_t *) vctx;
    int sc_id = arg->subchannel;
    
    acr7k_msch_t *channel = arg->msch;
    acr7k_subch_t *subchannel = &(arg->msch->subchannel[sc_id]);
    
    subchannel->running = 1;
    
    while (subchannel->running) {
        pthread_mutex_lock(&channel->status_lock);
        while (!subchannel->command) {
            pthread_cond_wait(&subchannel->cmd_cond, &channel->status_lock);
            
        }
        
        int command = subchannel->command;
        (void) command;
        pthread_mutex_unlock(&channel->status_lock);
        
        if (command == -1) {
            subchannel->running = 0;
        }
        
        else {
            fprintf(
                stderr, "Channel %03X.%01X got command\n",
                channel->id, sc_id
            );
            
            msleep(1000);
            
            fprintf(
                stderr, "Channel %03X.%01X done\n",
                channel->id, sc_id
            );
            
            pthread_mutex_lock(&channel->status_lock);
            subchannel->command = 0;
            pthread_mutex_unlock(&channel->status_lock);
        }
    }
    
    /*
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
                // fclose(ctx->file);
                ctx->running = 0;
                ctx->buf = 0;
                fprintf(
                    stderr,
                    "PPT: %04o End of tape\n", ctx->id
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
    
    */
    
    return NULL;
}

void destroy_msch(acr7k_cu_t *cpu, int id) {
    acr7k_msch_t *ctx = (acr7k_msch_t *) cpu->ioctx[id];
    
    for (int i = 0; i < 16; i++) {
        if (ctx->subchannel[i].running) {
            pthread_cancel(ctx->subchannel[i].thread);
        }
        pthread_cond_destroy(&ctx->subchannel[i].cmd_cond);
    }
    
    pthread_mutex_destroy(&ctx->status_lock);
    free(ctx);
    
    fprintf(stderr, "MSC: %04o deinitialized\n", id);
}

void init_msch(acr7k_cu_t *cpu, int id, int irq) {
    acr7k_msch_t *ctx = calloc(sizeof(acr7k_msch_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_msch;
    // cpu->io[id] = msch_io;
    
    ctx->cpu = cpu;
    ctx->id = id;
    ctx->irq = irq;
    
    pthread_mutex_init(&ctx->status_lock, NULL);
    
    msch_arg_t msch_arg[16];
    
    for (int i = 0; i < 16; i++) {
        msch_arg[i].msch = ctx;
        msch_arg[i].subchannel = i;
        
        pthread_cond_init(&ctx->subchannel[i].cmd_cond, NULL);
        pthread_create(&ctx->subchannel[i].thread, NULL, subch, &msch_arg[i]);
    }
    
    fprintf(stderr, "MSC: %04o IRQ %02o\n", id, irq);
}