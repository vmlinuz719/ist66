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
#define MASK_17 0x1FFFFL
#define MASK_18 0x3FFFFL
#define MASK_19 0x7FFFFL

#define EXT12(x) ((x) & (1L << 11) ? (x) | 0xFFFFFFFFFFFFF000 : (x))

#define C_IOPC 0
#define C_ION 1
#define C_IRQ 2
#define C_API 3

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
    if (!zero_pg) ea += iocpu->c[C_IOPC];
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
        case 0: { // AN
            uint64_t data = (io_read_mem(iocpu, ea)) & MASK_18;
            iocpu->a[0] &= (iocpu->a[0] + data) | (1 << 18);
            iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
        } break;
        case 1: { // A
            uint64_t data = (io_read_mem(iocpu, ea)) & MASK_18;
            iocpu->a[0] = (iocpu->a[0] + data) & MASK_19;
            iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
        } break;
        case 2: { // ITN
            uint64_t data = (io_read_mem(iocpu, ea) + 1) & MASK_18;
            io_write_mem(iocpu, ea, data);
            iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + (data ? 1 : 2)) & MASK_18;
        } break;
        case 3: { // SC
            io_write_mem(iocpu, ea, iocpu->a[0]);
            iocpu->a[0] &= 1 << 18;
            iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
        } break;
        case 4: { // BL
            io_write_mem(iocpu, ea, iocpu->c[C_IOPC] + 1);
            iocpu->c[C_IOPC] = (ea + 1) & MASK_18;
        } break;
        case 5: { // B
            iocpu->c[C_IOPC] = ea & MASK_18;
        } break;
    }
}

void io_exec_io(ist66_cu_t *iocpu, uint64_t inst) {
    uint64_t device = inst & 0x7F;
    uint64_t post_swap = (inst >> 8) & 1;
    uint64_t pre_clear = (inst >> 7) & 1;
    uint64_t ctl = (inst >> 13) & 0x3;
    uint64_t transfer = (inst >> 9) & 0xF;
    uint64_t data = iocpu->a[0] & MASK_18;
    
    if (pre_clear) iocpu->a[0] &= 1 << 18;
    
    if (device < iocpu->max_io && iocpu->io[device] != NULL) {
        uint64_t result = iocpu->io[device](
            iocpu->ioctx[device],
            data,
            ctl,
            transfer
        );
        
        if (transfer < 14 && !(transfer & 1)) {
            iocpu->a[0] |= result & MASK_18;
        }
        
        else if (transfer == 14) { // last two bits of result Done, Busy
            int cond = 0;
            switch (ctl) {
                case 0: // skip if busy
                    cond = !!(result & 1);
                    break;
                case 1: // skip if not busy
                    cond = !(result & 1);
                    break;
                case 2: // skip if done
                    cond = !!(result & 2);
                    break;
                case 3: // skip if not done
                    cond = !(result & 2);
                    break;
            }
            if (cond) {
                iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
            }
        }
        
        iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
    }
    
    if (post_swap) iocpu->a[0] = (
        (iocpu->a[0] & (1 << 18))
        | ((iocpu->a[0] & 0x1FF) << 9)
        | ((iocpu->a[0] >> 9) & 0x1FF)
    );
}

void io_exec_opr_0(ist66_cu_t *iocpu, uint64_t inst) {
    if ((inst & (1 << 7))) {
        iocpu->a[0] &= 1 << 18;
    }
    
    if ((inst & (1 << 6))) {
        iocpu->a[0] &= MASK_18;
    }
    
    if ((inst & (1 << 5))) {
        iocpu->a[0] ^= MASK_18;
    }
    
    if ((inst & (1 << 4))) {
        iocpu->a[0] ^= 1 << 18;
    }
    
    if ((inst & 1)) {
        iocpu->a[0] = (iocpu->a[0] + 1) & MASK_19;
    }
    
    switch ((inst >> 1) & 7) {
        case 1: // BSW
            iocpu->a[0] = (
                (iocpu->a[0] & (1 << 18))
                | ((iocpu->a[0] & 0x1FF) << 9)
                | ((iocpu->a[0] >> 9) & 0x1FF)
            );
            break;
        case 2: // RAL
            iocpu->a[0] = (
                ((iocpu->a[0] & MASK_18) << 1)
                | (iocpu->a[0] >> 18)
            );
            break;
        case 3: // RTL
            iocpu->a[0] = (
                ((iocpu->a[0] & MASK_17) << 2)
                | (iocpu->a[0] >> 17)
            );
            break;
        case 4: // RAR
            iocpu->a[0] = (
                ((iocpu->a[0] & 1) << 18)
                | (iocpu->a[0] >> 1)
            );
            break;
        case 5: // RTR
            iocpu->a[0] = (
                ((iocpu->a[0] & 3) << 17)
                | (iocpu->a[0] >> 2)
            );
            break;
        case 6: // MSX
            iocpu->a[1] = iocpu->a[0] & MASK_18;
            break;
        case 7: // MDX
            iocpu->a[2] = iocpu->a[0] & MASK_18;
            break;
    }
    
    iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
}

void io_exec_opr_1(ist66_cu_t *iocpu, uint64_t inst) {
    int condition = 0;
    
    if ((inst & (1 << 6))) { // TGE
        condition |= !!(iocpu->a[0] & (1 << 17));
    }
    
    if ((inst & (1 << 5))) { // TNZ
        condition |= !(iocpu->a[0] & MASK_18);
    }
    
    if ((inst & (1 << 4))) { // TCZ
        condition |= !(iocpu->a[0] & (1 << 18));
    }
    
    if ((inst & (1 << 3))) { // And Group
        condition ^= 1;
    }
    
    if (condition) {
        iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
    }
    
    if ((inst & (1 << 7))) {
        iocpu->a[0] &= 1 << 18;
    }
    
    if ((inst & (1 << 1))) {
        pthread_mutex_lock(&(iocpu->lock));
        if (iocpu->min_pending > 1 || !iocpu->c[C_ION]) {
            iocpu->running = 0;
        }
        pthread_mutex_unlock(&(iocpu->lock));
    }
    
    if ((inst & (1 << 2))) {
        iocpu->a[0] |= iocpu->stop_code & MASK_18;
    }
    
    iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
}

void io_exec_opr_3(ist66_cu_t *iocpu, uint64_t inst) {
    if ((inst & (1 << 7))) { // CIE
        iocpu->c[C_ION] = 0;
    }
    
    if ((inst & (1 << 5))) { // CMI
        iocpu->c[C_ION] ^= 1;
    }
    
    if ((inst & (1 << 2))) { // SSR
        iocpu->stop_code = iocpu->a[0];
    }
    
    if ((inst & (1 << 3))) { // API
        pthread_mutex_lock(&(iocpu->lock));
        intr_assert(iocpu->host, iocpu->c[C_IRQ]);
        iocpu->c[C_API] = 1;
        pthread_mutex_unlock(&(iocpu->lock));
    }
    
    if ((inst & (1 << 1))) { // HLT
        pthread_mutex_lock(&(iocpu->lock));
        if (iocpu->min_pending > 1 || !iocpu->c[C_ION]) {
            iocpu->running = 0;
        }
        pthread_mutex_unlock(&(iocpu->lock));
    }
    
    int condition = 0;
    
    if ((inst & (1 << 4))) { // TIE
        if (iocpu->c[C_ION]) condition = 1;
    }
    
    if ((inst & (1 << 6))) { // TNP
        if (iocpu->pending[1] == 0) condition = 1;
    }
    
    if ((inst & (1 << 8))) { // And Group (TNE/TIP)
        condition ^= 1;
    }
    
    if (condition) {
        iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
    }
    
    iocpu->c[C_IOPC] = (iocpu->c[C_IOPC] + 1) & MASK_18;
}

void io_exec_all(ist66_cu_t *iocpu, uint64_t inst) {
    switch (inst >> 15) {
        case 6:
            io_exec_io(iocpu, inst);
            break;
        case 7:
            if ((inst & 1)) io_exec_opr_3(iocpu, inst);
            else if ((inst & (1 << 8))) io_exec_opr_1(iocpu, inst);
            else io_exec_opr_0(iocpu, inst);
            break;
        default:
            io_exec_mr(iocpu, inst);
    }
}