#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alu.h"
#include "cpu.h"
#include "ppt.h"
#include "pch.h"

void intr_assert(ist66_cu_t *cpu, int irq) {
    /* Max hardware IRQ is 14 */
    pthread_mutex_lock(&(cpu->lock));
    cpu->pending[irq]++;
    if (irq > cpu->max_pending && ((cpu->mask >> (15 - irq)) & 1)) {
        cpu->max_pending = irq;
        cpu->running = 1;
    }
    pthread_mutex_unlock(&(cpu->lock));
}

void intr_release(ist66_cu_t *cpu, int irq) {
    pthread_mutex_lock(&(cpu->lock));
    if (cpu->pending[irq] > 0) {
        cpu->pending[irq]--;
    }
    int new_max_pending = cpu->max_pending;
    while (new_max_pending > 0 
        && ((((cpu->mask >> (15 - new_max_pending)) & 1) == 0)
            || (cpu->pending[new_max_pending] == 0))) {
        new_max_pending--;
    }
    cpu->max_pending = new_max_pending;
    pthread_mutex_unlock(&(cpu->lock));
}

void intr_set_mask(ist66_cu_t *cpu, uint16_t mask) {
    pthread_mutex_lock(&(cpu->lock));
    cpu->mask = mask;
    int new_max_pending = 15;
    while (new_max_pending > 0 
        && ((((cpu->mask >> (15 - new_max_pending)) & 1) == 0)
            || (cpu->pending[new_max_pending] == 0))) {
        new_max_pending--;
    }
    cpu->max_pending = new_max_pending;
    pthread_mutex_unlock(&(cpu->lock));
}

uint64_t read_mem(ist66_cu_t *cpu, uint8_t key, uint32_t address) {
    address &= MASK_ADDR;
    
    if (address >= cpu->mem_size) {
        return MEM_FAULT;
    }
    else if ((uint8_t) (cpu->memory[address & ~(0x1FF)] >> 36) == 0xFE) {
        return cpu->memory[address] & MASK_36; // public read
    }
    else if ((uint8_t) (cpu->memory[address & ~(0x1FF)] >> 36) == 0xFF) {
        return cpu->memory[address] & MASK_36; // public read/write
    }
    else if (key && (uint8_t) (cpu->memory[address & ~(0x1FF)] >> 36) != key) {
        return MEM_FAULT;
    }
    else return cpu->memory[address] & MASK_36;
}

uint64_t write_mem(
    ist66_cu_t *cpu,
    uint8_t key,
    uint32_t address,
    uint64_t data
) {
    address &= MASK_ADDR;
    
    if (address >= cpu->mem_size) {
        return MEM_FAULT;
    }
    else if ((uint8_t) (cpu->memory[address & ~(0x1FF)] >> 36) == 0xFF) {
        uint64_t old_tag = cpu->memory[address] & ~(MASK_36); // public r/w
        cpu->memory[address] = old_tag | (data & MASK_36);
        return 0;
    }
    else if (key && (uint8_t) (cpu->memory[address & ~(0x1FF)] >> 36) != key) {
        return MEM_FAULT;
    }
    
    uint64_t old_tag = cpu->memory[address] & ~(MASK_36);
    cpu->memory[address] = old_tag | (data & MASK_36);
    return 0;
}

uint64_t set_key(ist66_cu_t *cpu, uint8_t key, uint32_t address) {
    if (address >= cpu->mem_size) {
        return MEM_FAULT;
    }
    
    address &= ~(0x1FF);
    uint64_t old_data = cpu->memory[address] & MASK_36;
    cpu->memory[address] = (((uint64_t) key) << 36) | old_data;
    return 0;
}

uint64_t comp_mr(ist66_cu_t *cpu, uint64_t inst) {
    int indirect = (inst >> 22) & 1;
    uint64_t index = (inst >> 18) & 0xF;
    uint64_t disp_u = inst & 0x3FFFF;
    uint64_t disp = EXT18(disp_u);
    uint64_t ea_l;
    
    switch (index) {
        case 0: {
            ea_l = disp;
        } break;
        case 1: {
            ea_l = ((cpu->c[C_CW] & 0x3FFFF) << 9) + disp;
        } break;
        case 2: {
            ea_l = (cpu->c[C_PSW] & MASK_ADDR) + disp;
        } break;
        case 14: {
            ea_l = cpu->a[13];
            cpu->a[13] = (cpu->a[13] + disp) & MASK_36;
        } break;
        case 15: {
            cpu->a[13] = (cpu->a[13] - disp) & MASK_36;
            ea_l = cpu->a[13];
        } break;
        default: {
            ea_l = cpu->a[index] + disp;
        }
    }
    ea_l &= MASK_36;
    
    if (indirect) {
        ea_l = read_mem(cpu, cpu->c[C_PSW] >> 28, ea_l & MASK_ADDR);
    }
    
    return ea_l;
}

void exec_mr(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    
    // TODO: decide how to handle faults
    
    switch ((inst >> 23) & 0xF) {
        case 0: { // JMP
            set_pc(cpu, ea);
        } break;
        case 1: { // CALL
            cpu->a[12] = (get_pc(cpu) + 1) & MASK_ADDR;
            set_pc(cpu, ea);
        } break;
        case 2: { // ISZ
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(data, 1, 0, 6, 0, 4, 0, 0, 0, 0);
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (SKIP(result)) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        case 3: { // DSZ
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(1, data, 0, 5, 0, 4, 0, 0, 0, 0);
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (SKIP(result)) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        default: {
            // UMR
            set_pc(cpu, get_pc(cpu) + 1);
        }
    }
}

void exec_am(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    uint64_t ac = (inst >> 23) & 0xF;
    
    // TODO: decide how to handle faults
    
    switch ((inst >> 27) & 0x1FF) {
        case 001: { // EDIT
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0, 0
            );
            cpu->do_edit = 1;
            cpu->xeq_inst = result & MASK_36;
        } break;
        case 002: { // EDSK
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0, 0
            );
            cpu->do_edit = 1;
            cpu->do_edsk = 1;
            cpu->xeq_inst = result & MASK_36;
        } break;
        case 003: { // MOVEA
            cpu->a[ac] = ea;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 004: { // ADDEA
            uint64_t result = compute(
                ea, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 005: { // ISE
            uint64_t result = compute(
                1, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            if (data == cpu->a[ac]) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        case 006: { // DSE
            uint64_t result = compute(
                1, cpu->a[ac], get_cf(cpu), 5, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            if (data == cpu->a[ac]) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        case 007: { // MOVEAS
            cpu->a[ac] = (ea << 17) & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 010: { // LDCOM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, 0, 0, 0, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 011: { // LDNEG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, 0, 0, 1, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 012: { // LDA
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            cpu->a[ac] = data & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 013: { // STA
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, cpu->a[ac]);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 014: { // ADCM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 4, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 015: { // SUBM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 5, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 016: { // ADDM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 017: { // ANDM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 7, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 022: { // ORM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 026: { // XORM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea) & MASK_36;
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 15, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        default: {
            // Illegal
            set_pc(cpu, get_pc(cpu) + 1);
        }
    }
}

void exec_smi(ist66_cu_t *cpu, uint64_t inst) {
    
    // TODO: decide how to handle faults
    
    uint64_t key = (cpu->c[C_PSW] >> 28) & 0xFF;
    if (!key) {
        uint64_t ea = comp_mr(cpu, inst);
        uint64_t ac = (inst >> 23) & 0xF;
        switch ((inst >> 27) & 0x1FF) {
            case 0600: { // HLT
                halt(cpu);
                cpu->stop_code = cpu->a[ac];
                set_pc(cpu, ea);
            } break;
            case 0601: { // INT
                set_pc(cpu, ea);
                do_intr(cpu, ac);
            } break;
            case 0602: { // various
                switch (ac) {
                    case 0: { // RFI
                        leave_intr(cpu);
                    } break;
                    case 1: { // RMSK
                        uint64_t data = read_mem(cpu, 0, ea) & MASK_36;
                        intr_set_mask(cpu, data);
                        leave_intr(cpu);
                    } break;
                    case 2: { // LDMSK
                        uint64_t data = read_mem(cpu, 0, ea) & MASK_36;
                        intr_set_mask(cpu, data);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 3: { // STMSK
                        write_mem(cpu, 0, ea, cpu->mask);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    default: {
                        // Illegal
                        set_pc(cpu, get_pc(cpu) + 1);
                    }
                }
            } break;
            case 0603: { // LDK
                ea &= ~(0x1FF);
                if (ea < cpu->mem_size) {
                    cpu->a[ac] = cpu->memory[ea] >> 36;
                } else {
                    cpu->a[ac] = 0;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 0604: { // STK
                set_key(cpu, cpu->a[ac], ea);
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 0605: { // STCTL
                write_mem(cpu, 0, ea, cpu->c[ac & 0x7]);
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            default: {
                // Illegal
                set_pc(cpu, get_pc(cpu) + 1);
            }
        }
    } else {
        // Privilege
        set_pc(cpu, get_pc(cpu) + 1);
    }
}

void exec_io1(ist66_cu_t *cpu, uint64_t inst) {

    // TODO: decide how to handle faults
    
    uint64_t key = (cpu->c[C_PSW] >> 28) & 0xFF;
    if (!key) {
        uint64_t device = inst & 0xFFF;
        uint64_t ctl = (inst >> 16) & 0x3;
        uint64_t transfer = (inst >> 12) & 0xF;
        uint64_t ac = (inst >> 23) & 0xF;
        uint64_t data = cpu->a[ac];
        
        if (device < cpu->max_io && cpu->io[device] != NULL) {
            uint64_t result = cpu->io[device](
                cpu->ioctx[device],
                data,
                ctl,
                transfer
            );
            
            if (transfer < 14 && !(transfer & 1)) {
                cpu->a[ac] = result;
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
                    set_pc(cpu, get_pc(cpu) + 1);
                }
            }
            
            set_pc(cpu, get_pc(cpu) + 1);
        } else {
            // I/O not present
            set_pc(cpu, get_pc(cpu) + 1);
        }
    } else {
        // Privilege
        set_pc(cpu, get_pc(cpu) + 1);
    }
}

void exec_all(ist66_cu_t *cpu, uint64_t inst) {
    if (inst >> 33 == 0x7) { // ALU operation
        uint64_t acs = (inst >> 27) & 0xF;
        uint64_t acd = (inst >> 23) & 0xF;
        uint64_t result = exec_aa(inst, cpu->a[acs], cpu->a[acd], get_cf(cpu));
        if (((inst >> 11) & 0x7) == 0x4) {
            // ADR encoding; save to alternate register
            acd = (inst >> 7) & 0xF;
        }
        cpu->a[acd] = result & MASK_36;
        set_cf(cpu, (result >> 36) & 1);
        if (SKIP(result)) {
            set_pc(cpu, get_pc(cpu) + 2);
        } else {
            set_pc(cpu, get_pc(cpu) + 1); 
        }
    }
    else if (inst >> 27 == 0) {
        exec_mr(cpu, inst);
    }
    else if (inst >> 27 <= 027) {
        exec_am(cpu, inst);
    }
    else if (inst >> 27 == 0670) {
        exec_io1(cpu, inst);
    }
    else if (inst >> 33 == 06) {
        exec_smi(cpu, inst);
    }
}

uint64_t run(ist66_cu_t *cpu, uint64_t init_pc) {
    cpu->running = 1;
    set_pc(cpu, init_pc);
    
    while (cpu->running) {
        if (cpu->do_edit) {
            exec_all(cpu, cpu->xeq_inst);
            cpu->do_edit = 0;
            if (cpu->do_edsk) {
                set_pc(cpu, get_pc(cpu) + 1);
                cpu->do_edsk = 0;
            }
        }
        
        uint64_t current_irql = (cpu->c[C_CW] >> 32) & 0xF;
        if (cpu->max_pending > current_irql) {
            do_intr(cpu, cpu->max_pending);
        }
        
        uint64_t inst = read_mem(cpu, cpu->c[C_PSW] >> 28, get_pc(cpu));
        // TODO: decide how to handle faults
        exec_all(cpu, inst);
    }
    
    return cpu->stop_code;
}

int main(int argc, char *argv[]) {
    ist66_cu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    
    cpu.memory = calloc(sizeof(uint64_t), 65536);
    cpu.mem_size = 65536;
    pthread_mutex_init(&(cpu.lock), NULL);
    
    cpu.io_init = calloc(sizeof(ist66_io_init_t), 512);
    cpu.io_destroy = calloc(sizeof(ist66_io_init_t), 512);
    cpu.io = calloc(sizeof(ist66_io_t), 512);
    cpu.ioctx = calloc(sizeof(void *), 512);
    cpu.max_io = 512;
    
    /* set I/O initializers here */
    cpu.io_init[012] = init_ppt;
    cpu.io_init[013] = init_pch;
    
    for (int i = 0; i < cpu.max_io; i++) {
        if (cpu.io_init[i] != NULL) {
            cpu.io_init[i](&cpu, i);
        }
    }
    
    cpu.memory[512] = 0xF08E00000;      // XOR    1,1
    cpu.memory[513] = 0xF11608000;      // XOR    2,2,SKP
    cpu.memory[514] = 0x00000000C;      // DW     12
    
    cpu.memory[515] = 0xDC001F00A;      // NTS    10
    
    cpu.memory[516] = 0xDC002E00A;      // SKPDN  10
    cpu.memory[517] = 0x0000BFFFF;      // JMP    .-1
    cpu.memory[518] = 0xDC001000A;      // INS    0,10,0
    cpu.memory[519] = 0xE0022C000;      // MOV#   0,0,SNZ
    cpu.memory[520] = 0x0000BFFFC;      // JMP    .-4
    
    cpu.memory[521] = 0xE00201080;      // MOVM   0,0,33
    cpu.memory[522] = 0xE08A00003;      // MOVR   1,1,3
    cpu.memory[523] = 0xF00A00000;      // OR     0,1
    cpu.memory[524] = 0x0290BFFF6;      // ISE    2,.-10
    cpu.memory[525] = 0x0000BFFF7;      // JMP    .-9
    
    cpu.memory[526] = 0xC00800000;      // HLT    1
    
    fprintf(stderr, "HALT: stop code %012lo\n", run(&cpu, 512));
    
    for (int i = 0; i < cpu.max_io; i++) {
        if (cpu.io_destroy[i] != NULL) {
            cpu.io_destroy[i](&cpu, i);
        }
    }
    
    free(cpu.memory);
    free(cpu.io_init);
    free(cpu.io_destroy);
    free(cpu.io);
    free(cpu.ioctx);
    pthread_mutex_destroy(&(cpu.lock));
    return 0;
}
