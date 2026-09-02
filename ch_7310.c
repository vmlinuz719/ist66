#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "cpu.h"
#include "msc.h"

/* ACR 7310 Channel Debug Thingy */

typedef struct {
    int tx_type;
    int write;
    
    int ch_id, sch_id;
} ch7310_device_t;

void ch7310_detach(acr7k_subch_t *subch) {
    ch7310_device_t *device = subch->device;
    
    pthread_cancel(subch->thread);
    fprintf(stderr, "7310: %04o:%02o detached\n",
        device->ch_id, device->sch_id);
    free(subch->device);
}

uint64_t ch7310_sense_reg(acr7k_subch_t *subch) {
    ch7310_device_t *device = subch->device;
    
    return (device->write << 4) | device->tx_type;
}

void ch7310_sense(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch,
    uint8_t opcode,
    uint64_t addr
) {
    ch7310_device_t *device = subch->device;
    fprintf(stderr, "7310: %04o:%02o SENSE(%02o, %09o)\n",
        device->ch_id, device->sch_id,
        (unsigned int) opcode, (unsigned int) addr);
}

void ch7310_control(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch,
    uint8_t opcode,
    uint64_t addr
) {
    ch7310_device_t *device = subch->device;
    fprintf(stderr, "7310: %04o:%02o CONTROL(%02o, %09o)\n",
        device->ch_id, device->sch_id,
        (unsigned int) opcode, (unsigned int) addr);
}

void ch7310_start_transact(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch,
    int write,
    uint8_t opcode
) {
    ch7310_device_t *device = subch->device;
    
    fprintf(stderr, "7310: %04o:%02o Start Transaction %s(%02o)\n",
        device->ch_id, device->sch_id,
        write ? "WRITE" : "READ", (unsigned int) opcode);
    
    device->write = write;
    device->tx_type = opcode;
    subch->flags |= CH_BUSY;
}

void ch7310_end_transact(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch
) {
    ch7310_device_t *device = subch->device;
    
    fprintf(stderr, "7310: %04o:%02o End Transaction %s(%02o)\n",
        device->ch_id, device->sch_id,
        device->write ? "WRITE" : "READ", (unsigned int) device->tx_type);
    
    device->write = 0;
    device->tx_type = 0;
    
    subch->flags &= ~CH_BUSY;
    subch->residual = 0;
}

void ch7310_transfer(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch,
    uint64_t tx_addr, uint64_t count
) {
    ch7310_device_t *device = subch->device;
    
    fprintf(stderr, "7310: %04o:%02o     %s(%02o) %09o[%03o]\n",
        device->ch_id, device->sch_id,
        device->write ? "WRITE" : "READ", (unsigned int) device->tx_type,
        (unsigned int) tx_addr, (unsigned int) count);
}

void init_7310(acr7k_cu_t *cpu, int id, int sc_id) {
    acr7k_msch_t *ctx = (acr7k_msch_t *) cpu->ioctx[id];// Unsafe horrible hack
    
    if (sc_id < 0 || sc_id > 15) {
        fprintf(stderr, "7310: %04o invalid subchannel %02o\n", id, sc_id);
        return;
    }
    
    else if (ctx->subchannel[sc_id].attached) {
        fprintf(stderr, "7310: %04o:%02o already attached\n", id, sc_id);
        return;
    }
    
    acr7k_subch_t *subch = &ctx->subchannel[sc_id];
    
    subch->device = calloc(sizeof(ch7310_device_t), 1);
    ch7310_device_t *device = subch->device;
    device->ch_id = id;
    device->sch_id = sc_id;
    
    subch->detach = ch7310_detach;
    subch->sense_reg = ch7310_sense_reg;
    subch->sense = ch7310_sense;
    subch->control = ch7310_control;
    subch->start_transact = ch7310_start_transact;
    subch->end_transact = ch7310_end_transact;
    subch->transfer = ch7310_transfer;
    
    fprintf(stderr, "7310: %04o:%02o (no configurable options)\n",
        device->ch_id, device->sch_id);
        
    sc_attach(cpu, id, sc_id);
}