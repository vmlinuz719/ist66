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