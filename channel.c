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
    uint64_t caw;
    
    // use status_lock
    pthread_cond_t cmd_cond;
    int command, done;
} acr7k_subch_t;

typedef struct {
    acr7k_cu_t *cpu;
    int id, irq;
    
    int subch_select;
    
    pthread_mutex_t status_lock;
    acr7k_subch_t subchannel[16];
    int lowest_subch_done;          // 16 if no channels done
} acr7k_msch_t;

typedef struct {
    acr7k_msch_t *msch;
    int subchannel;
} msch_arg_t;

void set_done(acr7k_msch_t *msch, int sc) {
    // must have status_lock
    
    acr7k_subch_t *subchannel = &(msch->subchannel[sc]);
    
    subchannel->done = 1;
    
    int prev_done = msch->lowest_subch_done;
    
    if (sc < msch->lowest_subch_done) {
        msch->lowest_subch_done = sc;
    }
    
    if (prev_done == 16) {
        intr_assert(msch->cpu, msch->irq);
    }
}

void clear_done(acr7k_msch_t *msch, int sc) {
    // must have status_lock
    
    acr7k_subch_t *subchannel = &(msch->subchannel[sc]);
    
    if (subchannel->done) {
        subchannel->done = 0;
        
        int new_lowest;
        for (new_lowest = 0; new_lowest < 16; new_lowest++) {
            if (msch->subchannel[new_lowest].done) break;
        }
        msch->lowest_subch_done = new_lowest;
        
        if (new_lowest == 16) {
            intr_release(msch->cpu, msch->irq);
        }
    }
}

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
                stderr, "MSC: %03X.%01X busy\n",
                channel->id, sc_id
            );
            
            msleep(1000);
            
            fprintf(
                stderr, "MSC: %03X.%01X done\n",
                channel->id, sc_id
            );
            
            pthread_mutex_lock(&channel->status_lock);
            subchannel->command = 0;
            set_done(channel, sc_id);
            pthread_mutex_unlock(&channel->status_lock);
        }
    }
    
    return NULL;
}

uint64_t msch_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) {
    acr7k_msch_t *ctx = (acr7k_msch_t *) vctx;
    // acr7k_cu_t *cpu = ctx->cpu;
    
    if (transfer == 1) {
        ctx->subch_select = data & 0xF;
    }
    
    else if (transfer == 3) {
        ctx->subchannel[ctx->subch_select].caw = data;
    }
    
    acr7k_subch_t *subchannel = &(ctx->subchannel[ctx->subch_select]);
    
    if (transfer != 14) {
        switch (ctl) {
            case 1: {
                pthread_mutex_lock(&ctx->status_lock);
                subchannel->command = 1;
                clear_done(ctx, ctx->subch_select);
                pthread_cond_signal(&subchannel->cmd_cond);
                pthread_mutex_unlock(&ctx->status_lock);
            } break;
            case 2: {
                pthread_mutex_lock(&ctx->status_lock);
                // ctx->command = 0;
                clear_done(ctx, ctx->subch_select);
                pthread_mutex_unlock(&ctx->status_lock);
            } break;
        }
    }
    
    /*
    if (transfer == 14) {
        int status = (ctx->done << 1) | (ctx->command & 1);
        return (uint64_t) status;
    }
    
    else if (transfer == 0) {
        return pop_char(ctx);
    }

    else if (transfer == 2) {
        return (ctx->control << 8) | ctx->threshold;
    }

    else if (transfer == 4) {
        return ctx->len;
    }
    
    else */ return 0;
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
    cpu->io[id] = msch_io;
    
    ctx->cpu = cpu;
    ctx->id = id;
    ctx->irq = irq;
    
    ctx->lowest_subch_done = 16; // start with all clear
    
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