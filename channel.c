#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>    

#include "cpu.h"

#define MASK_36 0xFFFFFFFFFL
#define MASK_18 0x3FFFFL

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
    
    int attached;
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
    
    if (!(subchannel->done)) {
        subchannel->done = 1;
        
        int prev_done = msch->lowest_subch_done;
        
        if (sc < msch->lowest_subch_done) {
            msch->lowest_subch_done = sc;
        }
        
        if (prev_done == 16) {
            intr_assert(msch->cpu, msch->irq);
        }
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
    free(vctx);

    acr7k_subch_t *subchannel = &(channel->subchannel[sc_id]);
    acr7k_cu_t *cpu = channel->cpu;
    
    subchannel->attached = 1;
    
    while (subchannel->attached) {
        pthread_mutex_lock(&channel->status_lock);
        while (!subchannel->command) {
            pthread_cond_wait(&subchannel->cmd_cond, &channel->status_lock);
            
        }
        
        int command = subchannel->command;
        (void) command;
        pthread_mutex_unlock(&channel->status_lock);
        
        if (command == -1) {
            subchannel->attached = 0;
        }
        
        else {
            while (subchannel->attached && subchannel->command) {
                // fetch CCW
                // first fail out if CAW is not valid
                if (subchannel->caw >= cpu->mem_size) {
                    // TODO: handle CCW load check
                    fprintf(stderr, "MSC: %04o:%02o CCW Load Check\n", channel->id, sc_id);
                    subchannel->command = 0;
                    break;
                }
                
                uint64_t ccw = cpu->memory[subchannel->caw] & MASK_36;
                
                uint64_t opcode = (ccw >> 30) & 0xF;
                uint64_t op_type = ccw >> 34;
                uint64_t chain = (ccw >> 27) & 1;
                uint64_t data_addr = ccw & MASK_ADDR;
                
                const char *ops[] = {
                    "SENSE",
                    "CONTROL",
                    "READ",
                    "WRITE"
                };
                
                fprintf(stderr, "MSC: %04o:%02o CCW %s(%02o, %09o)\n",
                    channel->id, sc_id, ops[op_type], (unsigned int) opcode, (unsigned int) data_addr);
                
                if (op_type > 1) { // read/write transaction
                    if (data_addr >= cpu->mem_size) {
                        // TODO: handle count/displacement list check
                        fprintf(stderr, "MSC: %04o:%02o CDL Load Check\n", channel->id, sc_id);
                        subchannel->command = 0;
                        break;
                    }
                    
                    int status_ok = 1;
                    uint64_t cdl_header = cpu->memory[data_addr];
                    uint64_t cdl_len = cdl_header >> 27;
                    uint64_t cdl_base = cdl_header & MASK_ADDR;
                    
                    if (cdl_len == 0) {
                        fprintf(stderr, "MSC: %04o:%02o Begin Transaction %s(%02o) (Suppressed)\n",
                            channel->id, sc_id, ops[op_type], (unsigned int) opcode);
                        fprintf(stderr, "MSC: %04o:%02o End Transaction\n", channel->id, sc_id);
                    } else {
                        fprintf(stderr, "MSC: %04o:%02o Begin Transaction %s(%02o)\n",
                            channel->id, sc_id, ops[op_type], (unsigned int) opcode);
                        
                        int current_cdl_entry = 1;
                        while (current_cdl_entry <= cdl_len) {
                            uint64_t cdl_entry_addr = (data_addr + current_cdl_entry) & MASK_ADDR;
                            if (cdl_entry_addr >= cpu->mem_size) {
                                // TODO: handle count/displacement list check
                                fprintf(stderr, "MSC: %04o:%02o CDL Load Check\n", channel->id, sc_id);
                                subchannel->command = 0;
                                status_ok = 0;
                                break;
                            }
                            
                            uint64_t cdl_entry = cpu->memory[cdl_entry_addr];
                            
                            uint64_t cdl_entry_count = cdl_entry >> 18;
                            uint64_t cdl_entry_disp = cdl_entry & MASK_18;
                            uint64_t cdl_tx_addr = (cdl_entry_disp + cdl_base) & MASK_ADDR;
                            
                            // TODO: make device API call
                            fprintf(
                                stderr,
                                "MSC: %04o:%02o     %09o:%03o\n",
                                channel->id, sc_id,
                                (unsigned int) cdl_tx_addr, (unsigned int) cdl_entry_count
                            );
                            
                            current_cdl_entry++;
                        }
                        
                        fprintf(stderr, "MSC: %04o:%02o End Transaction\n", channel->id, sc_id);
                    }
                    
                    if (!status_ok) break;
                }
                
                if (chain) {
                    subchannel->caw++;
                    subchannel->caw &= MASK_ADDR;
                } else {
                    fprintf(stderr, "MSC: %04o:%02o Channel Program Completed\n", channel->id, sc_id);
                    subchannel->command = 0;
                    break;
                }
            }
            
            pthread_mutex_lock(&channel->status_lock);
            if (subchannel->command != -1) subchannel->command = 0;
            set_done(channel, sc_id);
            pthread_mutex_unlock(&channel->status_lock);
        }
    }
    
    fprintf(stderr, "MSC: %04o:%02o detached\n", channel->id, sc_id);
    
    return NULL;
}

uint64_t msch_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) { 
    acr7k_msch_t *ctx = (acr7k_msch_t *) vctx;
    pthread_mutex_lock(&ctx->status_lock);
    
    // acr7k_cu_t *cpu = ctx->cpu;
    
    uint64_t intr_poll_result = 0xFFFFFFFFF, result = 0;
    
    if (transfer == 1) {
        ctx->subch_select = data & 0xF;
    }
    
    else if (transfer == 3) {
        ctx->subchannel[ctx->subch_select].caw = data;
    }
    
    else if (transfer == 8) { // interrupt pop
        if (ctx->lowest_subch_done != 16) {
            ctx->subch_select = intr_poll_result = ctx->lowest_subch_done;
        }
    }
    
    acr7k_subch_t *subchannel = &(ctx->subchannel[ctx->subch_select]);
    
    if (transfer != 14) {
        switch (ctl) {
            case 1: {
                subchannel->command = 1;
                clear_done(ctx, ctx->subch_select);
                pthread_cond_signal(&subchannel->cmd_cond);
            } break;
            case 2: {
                // ctx->command = 0;
                clear_done(ctx, ctx->subch_select);
            } break;
        }
    }
    
    if (transfer == 8) {
        result = intr_poll_result;
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
    
    else */ 
    pthread_mutex_unlock(&ctx->status_lock);
    return result;
}

void destroy_msch(acr7k_cu_t *cpu, int id) {
    acr7k_msch_t *ctx = (acr7k_msch_t *) cpu->ioctx[id];
    
    for (int i = 0; i < 16; i++) {
        if (ctx->subchannel[i].attached) {
            pthread_cancel(ctx->subchannel[i].thread);
        }
        pthread_cond_destroy(&ctx->subchannel[i].cmd_cond);
    }
    
    pthread_mutex_destroy(&ctx->status_lock);
    free(ctx);
    
    fprintf(stderr, "MSC: %04o deinitialized\n", id);
}

int sc_attach(acr7k_cu_t *cpu, int id, int sc_id) {
    acr7k_msch_t *ctx = (acr7k_msch_t *) cpu->ioctx[id];// Unsafe horrible hack
    
    if (sc_id < 0 || sc_id > 15) {
        fprintf(stderr, "MSC: %04o invalid subchannel %02o\n", id, sc_id);
        return -1;
    }
    
    else if (ctx->subchannel[sc_id].attached) {
        fprintf(stderr, "MSC: %04o:%02o already attached\n", id, sc_id);
        return -1;
    }
    
    msch_arg_t *msch_arg = malloc(sizeof(msch_arg_t));
    msch_arg->msch = ctx;
    msch_arg->subchannel = sc_id;
    pthread_create(&ctx->subchannel[sc_id].thread, NULL, subch, msch_arg);
    
    fprintf(stderr, "MSC: %04o:%02o attached\n", id, sc_id);
    return 0;
}

int sc_detach(acr7k_cu_t *cpu, int id, int sc_id) {
    acr7k_msch_t *ctx = (acr7k_msch_t *) cpu->ioctx[id];// Unsafe horrible hack
    
    if (sc_id < 0 || sc_id > 15) {
        fprintf(stderr, "MSC: %04o invalid subchannel %02o\n", id, sc_id);
        return -1;
    }
    
    else if (!ctx->subchannel[sc_id].attached) {
        fprintf(stderr, "MSC: %04o:%02o already detached\n", id, sc_id);
        return -1;
    }
    
    ctx->subchannel[sc_id].attached = 0;
    // TODO: call some kind of detach handler?
    // pthread_cancel(ctx->subchannel[sc_id].thread);
    return 0;
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
    
    for (int i = 0; i < 16; i++) {
        
        pthread_cond_init(&ctx->subchannel[i].cmd_cond, NULL);
        
    }
    
    fprintf(stderr, "MSC: %04o IRQ %02o\n", id, irq);
}
