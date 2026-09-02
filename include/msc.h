#ifndef _MSC_
#define _MSC_

#include "cpu.h"

void init_msch(acr7k_cu_t *cpu, int id, int irq);

int sc_attach(acr7k_cu_t *cpu, int id, int sc_id);
int sc_detach(acr7k_cu_t *cpu, int id, int sc_id);

#define CH_UNIT_INDICATOR   1
#define CH_UNIT_EXCEPTION   2

#define CH_BUSY             8
#define CH_INTERFACE_CHECK  16
#define CH_DATA_CHECK       32
#define CH_COMMAND_CHECK    64
#define CH_INCORRECT_LENGTH 128

typedef struct acr7k_subch {
    pthread_t thread;
    
    int attached;
    uint64_t caw;
    uint64_t flags, addr_list_entry, residual;
    
    void *device;
    
    // TODO: define what these return and call them in the channel emulation
    
    void (*detach)(struct acr7k_subch *subch);
    
    uint64_t (*sense_reg)(struct acr7k_subch *subch);
    
    void (*sense)(acr7k_cu_t *cpu, struct acr7k_subch *subch, uint8_t opcode, uint64_t addr);
    void (*control)(acr7k_cu_t *cpu, struct acr7k_subch *subch, uint8_t opcode, uint64_t addr);
    
    // should at minimum set flags to just CH_BUSY
    // or raise COMMAND_CHECK if invalid command
    void (*start_transact)(acr7k_cu_t *cpu, struct acr7k_subch *subch, int write, uint8_t opcode);
    
    // should clear BUSY and set residual for the whole transaction
    void (*end_transact)(acr7k_cu_t *cpu, struct acr7k_subch *subch);
    
    // sets flags and residual
    // may set DATA_CHECK, UNIT_EXCEPTION or UNIT_INDICATOR
    void (*transfer)(acr7k_cu_t *cpu, struct acr7k_subch *subch, uint64_t tx_addr, uint64_t count);
    
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