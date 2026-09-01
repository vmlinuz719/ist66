#ifndef _MSC_
#define _MSC_

#include "cpu.h"

void init_msch(acr7k_cu_t *cpu, int id, int irq);

int sc_attach(acr7k_cu_t *cpu, int id, int sc_id);
int sc_detach(acr7k_cu_t *cpu, int id, int sc_id);

#define CH_UNIT_INDICATOR   1
#define CH_UNIT_EXCEPTION   2
#define CH_ATTENTION        4

#define CH_BUSY             8
#define CH_INTERFACE_CHECK  16
#define CH_DATA_CHECK       32
#define CH_COMMAND_CHECK    64
#define CH_INCORRECT_LENGTH 128
#define CH_PROGRAM_INTR     256

typedef struct acr7k_subch {
    pthread_t thread;
    
    int attached;
    uint64_t caw;
    uint64_t flags, addr_list_entry, residual;
    
    void *device;
    
    // TODO: define what these return and call them in the channel emulation
    
    void (*detach)(void *device);
    
    uint64_t (*sense_reg)(void *device);
    
    int (*sense)(acr7k_cu_t *cpu, void *device, uint8_t opcode, uint64_t addr);
    int (*control)(acr7k_cu_t *cpu, void *device, uint8_t opcode, uint64_t addr);
    
    int (*start_transact)(acr7k_cu_t *cpu, void *device, int write, uint8_t opcode);
    int (*transfer)(acr7k_cu_t *cpu, void *device, uint64_t tx_addr, uint64_t count);
    int (*end_transact)(acr7k_cu_t *cpu, void *device);
    
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

#endif