#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>    

#include "cpu.h"

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
    int subch = arg->subchannel;
    
    acr7k_msch_t *channel = arg->msch;
    acr7k_subch_t *subchannel = &(arg->msch->subchannel[subch]);
    
    subchannel->running = 1;
    
    while (subchannel->running) {
        pthread_mutex_lock(&channel->status_lock);
        while (!subchannel->command) {
            pthread_cond_wait(&subchannel->cmd_cond, &channel->status_lock);
            
        }
        
        int command = subchannel->command;
        (void) command;
        pthread_mutex_unlock(&channel->status_lock);
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