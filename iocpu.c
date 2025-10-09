/**
 * @file iocpu.c
 * Implements core IOCPU functionality
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alu.h"
#include "cpu.h"

#include "softfloat.h"

#define MASK_IO_ADDR 0xFFFFFFFL
#define MASK_18 0x3FFFFL
#define MASK_19 0x7FFFFL

#define EXT12(x) ((x) & (1L << 11) ? (x) | 0xFFFFFFFFFFFFF000 : (x))

uint64_t io_read_mem(ist66_cu_t *iocpu, uint32_t address) {
    address &= MASK_IO_ADDR;
    uint64_t word = 0;
    
    if (address <= MASK_18) {
        // local memory
        uint32_t dword_addr = address >> 1;
        if (dword_addr < iocpu->mem_size) {
            word = iocpu->memory[dword_addr];
        } else {
            // nothing there
            return 0;
        }
    } else {
        // host memory
        address -= (MASK_18 + 1);
        uint32_t dword_addr = address >> 1;
        word = read_mem(iocpu->host, 0, dword_addr);
        if (word >> 36) {
            // host bus error
            return 0;
        }
    }
    
    if (!(address & 1)) word >>= 18;
    word &= MASK_18;
    return word;
}

uint64_t io_write_mem(ist66_cu_t *iocpu, uint32_t address, uint64_t data) {
    address &= MASK_IO_ADDR;
    data &= MASK_18;
    uint64_t word = 0;
    
    if (address <= MASK_18) {
        // local memory
        uint32_t dword_addr = address >> 1;
        if (dword_addr < iocpu->mem_size) {
            word = iocpu->memory[dword_addr];
        } else {
            // nothing there
            return 1;
        }
    } else {
        // host memory
        address -= (MASK_18 + 1);
        uint32_t dword_addr = address >> 1;
        word = read_mem(iocpu->host, 0, dword_addr);
        if (word >> 36) {
            // host bus error
            return 1;
        }
    }
    
    uint64_t mask = (address & 1) ? MASK_18 << 18 : MASK_18;
    if (!(address & 1)) data <<= 18;
    word &= mask;
    word |= data;
    
    if (address <= MASK_18) {
        // local memory
        uint32_t dword_addr = address >> 1;
        if (dword_addr < iocpu->mem_size) {
            iocpu->memory[dword_addr] = word;
        } else {
            // nothing there
            return 1;
        }
    } else {
        // host memory
        address -= (MASK_18 + 1);
        uint32_t dword_addr = address >> 1;
        uint64_t result = write_mem(iocpu->host, 0, dword_addr, word);
        if (result) {
            // host bus error
            return 1;
        }
    }
    
    return 0;
}

uint64_t io_comp_mr(ist66_cu_t *iocpu, uint64_t inst) {
    int indirect = (inst >> 14) & 1;
    int index = (inst >> 13) & 1;
    int zero_pg = (inst >> 12) & 1;
    uint64_t disp_u = inst & 0xFFF;
    uint64_t disp = EXT12(disp_u);
    
    uint64_t ea = disp;
    if (!zero_pg) ea += iocpu->c[0];
    if (!index) ea += iocpu->a[1] << 10;
    ea &= MASK_IO_ADDR;
    
    if (!indirect) return ea;
    
    uint64_t ia = io_read_mem(iocpu, ea);
    if ((ea & MASK_18) >= 8 && (ea & MASK_18) < 16) {
        ia = (ia + 1) & MASK_18;
        io_write_mem(iocpu, ea, ia);
    }
    
    if (index) ia += iocpu->a[2] << 10;
    
    return ia & MASK_IO_ADDR;
}

void io_exec_mr(ist66_cu_t *iocpu, uint64_t inst) {
    uint64_t ea = io_comp_mr(iocpu, inst);
    
    switch (inst >> 15) {
        case 5: { // B
            iocpu->c[0] = ea;
        } break;
    }
}