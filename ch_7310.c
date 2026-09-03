#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "cpu.h"
#include "msc.h"

/* ACR 7310 Channel Debug Thingy */

int64_t load_byte(acr7k_cu_t *cpu, uint64_t index, unsigned int size) {
    uint64_t ea = index & MASK_ADDR;
    uint64_t sh = index >> 27;
    if (sh > 36) sh = 36;
    
    if (ea >= cpu->mem_size) return -1;
    
    uint64_t data = cpu->memory[ea] & 0xFFFFFFFFF;
    
    data >>= sh;
    data &= (1L << size) - 1;
    return (int64_t) data;
}

int store_byte(acr7k_cu_t *cpu, uint64_t index, unsigned int size, uint64_t b) {
    uint64_t ea = index & MASK_ADDR;
    uint64_t sh = index >> 27;
    if (sh > 36) sh = 36;
    
    if (ea >= cpu->mem_size) return -1;
    
    uint64_t data = cpu->memory[ea] & 0xFFFFFFFFF;
    uint64_t mask = ((1L << size) - 1) << sh;
    uint64_t wr_data = (b << sh) & mask;
    data &= ~mask;
    data |= wr_data;
    
    cpu->memory[ea] = data;
    return 0;
}

uint64_t inc_byte_index(uint64_t index, unsigned int size) {
    uint64_t ea = index & MASK_ADDR;
    uint64_t sh = index >> 27;
    
    sh -= size;
    if (sh > 36) {
        sh = (36 - size) & 0x3F;
        ea = (ea + 1) & MASK_ADDR;
    }
    
    return ea | (sh << 27);
}

typedef struct {
    int tx_type;
    int write;
    
    FILE *file;
    
    int ch_id, sch_id;
} ch7310_device_t;

void ch7310_detach(acr7k_subch_t *subch) {
    ch7310_device_t *device = subch->device;
    
    pthread_cancel(subch->thread);
    if (device->file != NULL) fclose(device->file);
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

void ch7310_fopen7(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch,
    uint64_t addr,
    int writable
) {
    ch7310_device_t *device = subch->device;
    
    if (addr >= cpu->mem_size) {
        subch->flags |= CH_DATA_CHECK;
        subch->residual = 0;
        return;
    }
    
    uint64_t size = cpu->memory[addr];
    if (size > 255) {
        subch->flags |= CH_DATA_CHECK;
        subch->residual = 0;
        return;
    }
    
    char *fname = calloc(size + 1, sizeof(char));
    for (int i = 0; i < size; i++) {
        addr = inc_byte_index(addr, 7);
        
        int64_t ch = load_byte(cpu, addr, 7);
        if (ch < 0) {
            subch->flags |= CH_DATA_CHECK;
            subch->residual = size - i;
            free(fname);
            return;
        }
        
        fname[i] = ch;
    }
    
    if (device->file != NULL) {
        fclose(device->file);
        device->file = NULL;
    }
    
    device->file = fopen(fname, writable ? "wb+" : "rb");
    
    if (device->file == NULL) {
        fprintf(stderr, "7310: %04o:%02o Open File %s FAILED\n",
            device->ch_id, device->sch_id,
            fname);
        subch->flags |= CH_UNIT_EXCEPTION;
    }
    
    else {
        fprintf(stderr, "7310: %04o:%02o Open File %s %s\n",
            device->ch_id, device->sch_id,
            fname, writable ? "READ/WRITE" : "READ");
    }
    
    free(fname);
    subch->residual = 0;
}

void ch7310_control(
    acr7k_cu_t *cpu,
    acr7k_subch_t *subch,
    uint8_t opcode,
    uint64_t addr
) {
    ch7310_device_t *device = subch->device;
    
    if (opcode == 0 || opcode == 1) {
        ch7310_fopen7(cpu, subch, addr, opcode);
    }
    
    else {
        fprintf(stderr, "7310: %04o:%02o CONTROL(%02o, %09o)\n",
            device->ch_id, device->sch_id,
            (unsigned int) opcode, (unsigned int) addr);
    }
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