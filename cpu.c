/**
 * @file cpu.c
 * Implements core CPU functionality
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alu.h"
#include "fpu.h"
#include "cpu.h"
#include "ppt.h"
#include "pch.h"
#include "lpt.h"

#include "softfloat.h"

/**
 * @brief Assert a priority interrupt signal
 *
 * IST-66 supports 14 interrupt priority levels (1-14; smaller number = higher
 * priority). Multiple devices may assert a single IRQ at a time. The emulated
 * CPU tracks the lowest (highest priority) IRQ that is asserted and unmasked.
 * Therefore, increment the count of devices asserting the chosen IRQ, update
 * the lowest pending IRQ and poke the CPU thread to start it if the selected
 * IRQ is not masked.
 *
 * DO NOT ATTEMPT TO ASSERT IRQ 0 OR >=15
 *
 * @param cpu Emulated CPU context
 * @param irq IRQ priority level (1-14, smaller number = higher priority)
 */
void intr_assert(ist66_cu_t *cpu, int irq) {
    pthread_mutex_lock(&(cpu->lock));
    cpu->pending[irq]++;
    if (irq < cpu->min_pending && ((cpu->mask >> irq) & 1)) {
        cpu->min_pending = irq;
        cpu->running = 1;
    }
    pthread_cond_signal(&cpu->intr_cond);
    pthread_mutex_unlock(&(cpu->lock));
}

/**
 * @brief Release a priority interrupt signal
 *
 * Decrement the count of devices asserting the chosen IRQ and update the lowest
 * pending IRQ to reflect this.
 *
 * @param cpu Emulated CPU context
 * @param irq IRQ priority level (1-14, smaller number = higher priority)
 */
void intr_release(ist66_cu_t *cpu, int irq) {
    pthread_mutex_lock(&(cpu->lock));
    if (cpu->pending[irq] > 0) {
        cpu->pending[irq]--;
    }
    int new_min_pending = cpu->min_pending;
    while (new_min_pending < 15 
        && ((((cpu->mask >> new_min_pending) & 1) == 0)
            || (cpu->pending[new_min_pending] == 0))) {
        new_min_pending++;
    }
    cpu->min_pending = new_min_pending;
    pthread_mutex_unlock(&(cpu->lock));
}

/**
 * @brief Set the IRQ mask
 *
 * Update the interrupt mask. Less significant bits of the 16-bit mask
 * correspond to higher priority levels (rightmost bit is level 0, leftmost bit
 * is level 15). If a bit is set to 1, its corresponding IRQ priority level is
 * enabled. The lowest pending IRQ must be recalculated to account for any newly
 * masked or unmasked priority levels. (Remember, only IRQ 1-14 are usable!)
 *
 * @param cpu Emulated CPU context
 * @param mask New IRQ mask
 */
void intr_set_mask(ist66_cu_t *cpu, uint16_t mask) {
    pthread_mutex_lock(&(cpu->lock));
    cpu->mask = mask;
    int new_min_pending = 1;
    while (new_min_pending < 15 
        && ((((cpu->mask >> new_min_pending) & 1) == 0)
            || (cpu->pending[new_min_pending] == 0))) {
        new_min_pending++;
    }
    cpu->min_pending = new_min_pending;
    pthread_mutex_unlock(&(cpu->lock));
}

/**
 * @brief Read a 36-bit word from CPU memory or return an error value
 *
 * IST-66 assigns to each 512-word page of memory an 8-bit memory protection
 * key. This key may be 0x00 (supervisor only), 0x01-0xFD (protected), 0xFE
 * (readable to all) or 0xFF (readable/writable to all). To read memory, a
 * 27-bit address is first bounds-checked against the amount of available
 * memory; if this check fails, this function returns a MEM_FAULT error value.
 * Then the protection key (usually the current key from control register 1,
 * Control Word) is checked against the target page's key. A read will succeed
 * and return a 36-bit word if either the provided key is equal to 0, the key
 * matches the one in memory or the key in memory is 0xFE or 0xFF; otherwise
 * this function returns a KEY_FAULT error value.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to test
 * @param address Memory address
 * @return Contents of memory, MEM_FAULT or KEY_FAULT
 */
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
        return KEY_FAULT;
    }
    else return cpu->memory[address] & MASK_36;
}

/**
 * @brief Write a 36-bit word to CPU memory or return an error value
 *
 * To write memory, a 27-bit address is first bounds-checked against the amount
 * of available memory; if this check fails, this function returns a MEM_FAULT
 * error value. Then the protection key (usually the current key from control
 * register 1, Control Word) is checked against the target page's key. A write
 * will succeed and return a 36-bit word if either the provided key is equal to
 * 0, the key matches the one in memory or the key in memory is 0xFF; otherwise
 * this function returns a KEY_FAULT error value.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to test
 * @param address Memory address
 * @param data 36-bit word to write
 * @return Zero, MEM_FAULT or KEY_FAULT
 */
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
        return KEY_FAULT;
    }
    
    uint64_t old_tag = cpu->memory[address] & ~(MASK_36);
    cpu->memory[address] = old_tag | (data & MASK_36);
    return 0;
}

/**
 * @brief Set a page's memory protection key
 *
 * This emulator uses a 64-bit word to store each 36-bit word of memory; there
 * is some extra space left over. The memory protection key for a given page is
 * stored in the eight bits immediately to the left of the low 36 bits of the
 * first word in the page. Of course, bounds-check each key set operation and
 * return MEM_FAULT on failure.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to write
 * @param address Memory address
 * @return Zero or MEM_FAULT
 */
uint64_t set_key(ist66_cu_t *cpu, uint8_t key, uint32_t address) {
    if (address >= cpu->mem_size) {
        return MEM_FAULT;
    }
    
    address &= ~(0x1FF);
    uint64_t old_data = cpu->memory[address] & MASK_36;
    cpu->memory[address] = (((uint64_t) key) << 36) | old_data;
    return 0;
}

/**
 * @brief Compute an effective address
 *
 * Most (all) IST-66 memory reference instructions use a format similar to the
 * DEC PDP-10: one indirect bit, one four-bit index selector and one 18-bit
 * signed displacement. Unlike the PDP-10 however, several of the index selector
 * values have special significance:
 *    - 0: No index register
 *    - 1: Use "direct page" 18-bit base in CR1 (Control Word)
 *    - 2: PC-relative
 *    - 3-13: Index register is AC3-AC13 (X0-X8, LR, SP)
 *    - 14: Post-increment AC13 (SP) by displacement
 *    - 15: Pre-decrement AC13 (SP) by displacement
 *
 * The computed address is thus a 36-bit word; as of now only the low 27 bits
 * are used for addressing even though all 36 bits are generated by this
 * operation.
 *
 * If an indirect address was specified, we now must perform an additional step
 * or two - the real address must be fetched from memory and if this operation
 * fails, either MEM_FAULT or KEY_FAULT is the result. If the most significant
 * bit of the fetched word is not set, then that is the final address. If it is
 * set, the next eight bits indicate a more advanced addressing mode:
 *    - Octal 0xx: Post-increment address field by signed 6-bit immediate xx
 *    - Octal 1xx: Pre-decrement address field by signed 6-bit immediate xx
 *    - 2xx, 3xx: Reserved (MEM_FAULT)
 * 
 * The result of an increment/decrement indirect address is written back to
 * memory after the successful completion of the instruction that computed it.
 * Should the instruction fail, all internal state pertaining to this operation
 * is cleared so it may be retried.
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 * @return Address, MEM_FAULT or KEY_FAULT
 */
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
        uint64_t new_ea = 
            read_mem(cpu, cpu->c[C_PSW] >> 28, ea_l & MASK_ADDR);
            
        if (
            new_ea == MEM_FAULT
            || new_ea == KEY_FAULT
            || !(new_ea & (1L << 35))
        )
            return new_ea;
        
        else {
            uint64_t mode = (new_ea >> 33) & 3;
            uint64_t inc = (new_ea >> 27) & 63;
            inc = EXT6(inc);
            uint64_t disp = new_ea & MASK_ADDR;
            
            if (mode == 0) {
                cpu->do_inc = 1;
                cpu->inc_addr = ea_l;
                cpu->inc_data = (
                    ((disp + inc) & MASK_ADDR)
                    | (new_ea & ~(MASK_ADDR))
                );
                return disp;
            }
            
            else if (mode == 1) {
                cpu->do_inc = 1;
                cpu->inc_addr = ea_l;
                cpu->inc_data = (
                    ((disp - inc) & MASK_ADDR)
                    | (new_ea & ~(MASK_ADDR))
                );
                return ((disp - inc) & MASK_ADDR);
            }
            
            else return MEM_FAULT;
        }
    }
    
    else return ea_l;
}

/**
 * @brief Execute an instruction with a memory reference
 *
 * These are the basic type "MR" instructions from opcode 000; the 9-bit opcode
 * is followed by a 4-bit function selector
 *    - 0: @c JMP - set program counter to effective address
 *    - 1: @c JSR - save PC to AC12/LR, set program counter to effective address
 *    - 2: @c ISZ - increment contents of memory, skip next instruction if zero
 *    - 3: @c DSZ - decrement contents of memory, skip next instruction if zero
 *
 * Any other function selector will raise an unimplemented instruction trap.
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 */
void exec_mr(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    
    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }
    
    switch ((inst >> 23) & 0xF) {
        case 0: { // JMP
            set_pc(cpu, ea);
        } break;
        case 1: { // JSR
            cpu->a[12] = (get_pc(cpu) + 1) & MASK_ADDR;
            set_pc(cpu, ea);
        } break;
        case 2: { // ISZ
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(data, 1, 0, 6, 0, 4, 0, 0, 0, 0);
            uint64_t w_res = write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }
            
            if (SKIP(result)) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        case 3: { // DSZ
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(1, data, 0, 5, 0, 4, 0, 0, 0, 0);
            uint64_t w_res = write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }
            
            if (SKIP(result)) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        default: {
            // UMR
            do_except(cpu, X_USER);
        }
    }
}

/**
 * @brief Execute a multiplication or division
 *
 * These are the multiply/divide instructions from opcode 030; the 9-bit opcode
 * is followed by a 4-bit function selector
 *    - 0: @c MPY - multiply AC1/MQ by contents of memory for a 72-bit result in
 *      AC2:AC0 (XY:AC, most significant bits in AC2/XY)
 *    - 1: @c MPA - multiply AC1/MQ by contents of memory and add 72-bit result
 *      to AC2:AC0 (XY:AC) (complement carry flag on carry out of XY)
 *    - 1: @c MNA - multiply AC1/MQ by contents of memory and subtract 72-bit
 *      result from AC2:AC0 (XY:AC) (complement carry flag on carry out of XY)
 *    - 3: @c DIV - divide AC by contents of memory for a 36-bit result in MQ;
 *      store remainder (modulo) in XY
 *
 * Any other function selector will raise an unimplemented instruction trap.
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 */
void exec_md(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    
    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }
    
    switch ((inst >> 23) & 0xF) {
        case 0: { // MPY
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            xmul(cpu->a[1], data, &cpu->a[0], &cpu->a[2]);
            
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 1: { // MPA
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t high, low;
            
            xmul(cpu->a[1], data, &low, &high);
            
            uint64_t result_l = compute(
                low, cpu->a[0], 0, 6, 0, 0, 0, 0, 0, 0
            );
            
            uint64_t carry_l = result_l >> 36;
            
            uint64_t result_h = compute(
                high + carry_l, cpu->a[2], get_cf(cpu), 6, 0, 0, 0, 0, 0, 0
            );
            
            cpu->a[0] = result_l & MASK_36;
            cpu->a[2] = result_h & MASK_36;
            
            set_cf(cpu, (result_h >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 2: { // MNA
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data = ((~data) + 1) & MASK_36;
            
            uint64_t high, low;
            
            xmul(cpu->a[1], data, &low, &high);
            
            uint64_t result_l = compute(
                low, cpu->a[0], 0, 6, 0, 0, 0, 0, 0, 0
            );
            
            uint64_t carry_l = result_l >> 36;
            
            uint64_t result_h = compute(
                high + carry_l, cpu->a[2], get_cf(cpu), 6, 0, 0, 0, 0, 0, 0
            );
            
            cpu->a[0] = result_l & MASK_36;
            cpu->a[2] = result_h & MASK_36;
            
            set_cf(cpu, (result_h >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 3: { // DIV
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            } else if (data == 0) {
                do_except(cpu, X_DIVZ);
                return;
            }
            data &= MASK_36;
            
            int64_t data_s = (int64_t) (EXT36(data));
            uint64_t ac = cpu->a[0];
            int64_t ac_s = (int64_t) (EXT36(ac));
            
            cpu->a[1] = ((uint64_t) (ac_s / data_s)) & MASK_36;
            cpu->a[2] = ((uint64_t) (ac_s % data_s)) & MASK_36;
            
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        default: {
            // UMR
            do_except(cpu, X_USER);
        }
    }
}

/**
 * @brief Execute an instruction with a memory reference and accumulator
 *
 * These are the type "AM" instructions from opcode 000-026; the 9-bit opcode is
 * followed by a 4-bit accumulator selector \a n
 *    - 001: @c EDT - bitwise OR AC\a n with contents of memory and execute
 *      the resulting value as an instruction
 *    - 002: @c ESK - bitwise OR AC\a n with contents of memory and execute
 *      the resulting value as an instruction; skip the next instruction in
 *      series
 *       - Note: the program counter remains unchanged. Any PC-relative
 *         operations effected by @c EDT/ESK will occur relative to the
 *         location of the @c EDT/ESK instruction rather than the called
 *         instruction. The behavior of calling @c EDT/ESK with @c EDT/ESK is
 *         not well-defined but will not violate protection.
 *    - 003: @c LAD - load effective address to AC\a n
 *    - 004: @c AAD - add effective address to AC\a n, complement carry flag on
 *      carry out
 *    - 005: @c ISE - increment AC\a n, complement carry flag on carry out, skip
 *      next instruction if AC\a n = contents of memory
 *    - 006: @c DSE - decrement AC\a n, complement carry flag on carry out, skip
 *      next instruction if AC\a n = contents of memory
 *    - 007: @c LAS - load effective address << 17 to AC\a n
 *    - 010: @c LCO - load one's complement of contents of memory to AC\a n
 *    - 011: @c LNG - load two's complement of contents of memory to AC\a n
 *    - 012: @c LAC - load contents of memory to AC\a n
 *    - 013: @c DAC - store contents of AC\a n to memory
 *    - 014: @c ADC - add one's complement of contents of memory to AC\a n,
 *      complement carry flag on carry out
 *    - 015: @c SUB - add two's complement of contents of memory to AC\a n,
 *      complement carry flag on carry out
 *    - 016: @c ADD - add contents of memory to AC\a n, complement carry flag on
 *      carry out
 *    - 017: @c AND - bitwise AND contents of memory to AC\a n
 *    - 022: @c IOR - bitwise OR contents of memory to AC\a n
 *    - 026: @c XOR - bitwise XOR contents of memory to AC\a n
 *
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 */
void exec_am(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    uint64_t ac = (inst >> 23) & 0xF;
    
    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }
    
    switch ((inst >> 27) & 0x1FF) {
        case 001: { // EDIT
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0, 0
            );
            cpu->do_edit = 1;
            cpu->xeq_inst = result & MASK_36;
        } break;
        case 002: { // EDSK
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
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
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
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
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
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
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, 0, 0, 0, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 011: { // LDNEG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, 0, 0, 1, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 012: { // LDA
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            cpu->a[ac] = data & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 013: { // STA
            uint64_t w_res =
                write_mem(cpu, cpu->c[C_PSW] >> 28, ea, cpu->a[ac]);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }
            
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 014: { // ADCM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 4, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 015: { // SUBM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 5, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 016: { // ADDM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 017: { // ANDM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 7, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 022: { // ORM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 026: { // XORM
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t result = compute(
                data, cpu->a[ac], get_cf(cpu), 15, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        default: {
            // Illegal
            do_except(cpu, X_INST);
        }
    }
}

void exec_smi(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t key = (cpu->c[C_PSW] >> 28) & 0xFF;
    if (!key) {
        uint64_t ea = comp_mr(cpu, inst);
        if (ea == MEM_FAULT) {
            do_except(cpu, X_MEMX);
            return;
        }
        
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
                        uint64_t data = read_mem(cpu, 0, ea);
                        if (data == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        data &= MASK_36;
                        
                        intr_set_mask(cpu, data);
                        leave_intr(cpu);
                    } break;
                    case 2: { // LDMSK
                        uint64_t data = read_mem(cpu, 0, ea);
                        if (data == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        data &= MASK_36;
                        
                        intr_set_mask(cpu, data);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 3: { // STMSK
                        uint64_t w_res = write_mem(cpu, 0, ea, cpu->mask);
                        if (w_res == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    default: {
                        // Illegal
                        do_except(cpu, X_INST);
                    }
                }
            } break;
            case 0603: { // LDK
                ea &= ~(0x1FF);
                if (ea < cpu->mem_size) {
                    cpu->a[ac] = cpu->memory[ea] >> 36;
                } else {
                    do_except(cpu, X_MEMX);
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 0604: { // STK
                uint64_t w_res = set_key(cpu, cpu->a[ac], ea);
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 0605: { // LCT
                uint64_t data = read_mem(cpu, 0, ea);
                if (data == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                   return;
                }
                data &= MASK_36;
            
                cpu->c[ac] = data & MASK_36;
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 0606: { // STCTL
                uint64_t w_res = write_mem(cpu, 0, ea, cpu->c[ac & 0x7]);
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            default: {
                // Illegal
                do_except(cpu, X_INST);
            }
        }
    } else {
        // Privilege
        do_except(cpu, X_PPFS);
    }
}

void exec_io1(ist66_cu_t *cpu, uint64_t inst) {
    
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
            do_except(cpu, X_DEVX);
        }
    } else {
        // Privilege
        do_except(cpu, X_PPFS);
    }
}

/**
 * @brief Execute a call or return with register save mask
 *
 * These are the function call instructions from opcode 030; the 9-bit opcode is
 * followed by a 4-bit function selector
 *    - 0: @c CLM - call with mask
 *       - Load call mask from effective address
 *       - For bits \a n = 35 to 20, push accumulator (\a n - 20) to stack
 *       - Push mask then finally return address to stack
 *       - Set program counter to effective address + 1
 *    - 1: @c RTM - return with mask
 *       - Pop program counter from stack
 *       - Pop mask from stack
 *       - For bits \a n = 20 to 35, pop accumulator (\a n - 20) from stack
 *
 * Any other function selector will raise an unimplemented instruction trap. If
 * any portion of a mask call or return instruction (e.g. push, pop) fails, all
 * changes to CPU state will be rolled back to allow a retry.
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 */
void exec_call(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    
    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }
    
    switch ((inst >> 23) & 0xF) {
        case 0: { // CALL
            uint64_t mask = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (mask == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (mask == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            mask &= MASK_36;
            
            uint64_t temp_sp = cpu->a[13];
            
            for (int i = 0; i < 16; i++) {
                if ((mask << i) & 1) {
                    int reg = 15 - i;
                    uint64_t w_res =
                        write_mem(
                            cpu, cpu->c[C_PSW] >> 28, --temp_sp, cpu->a[reg]
                        );
                    if (w_res == MEM_FAULT) {
                        do_except(cpu, X_MEMX);
                        return;
                    } else if (w_res == KEY_FAULT) {
                        do_except(cpu, X_PPFW);
                        return;
                    }
                }
            }
            
            uint64_t last_two[2] = {mask, (get_pc(cpu) + 1) & MASK_ADDR};
            
            for (int i = 0; i < 2; i++) {
                uint64_t w_res =
                    write_mem(
                        cpu, cpu->c[C_PSW] >> 28, --temp_sp, last_two[i]
                    );
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                } else if (w_res == KEY_FAULT) {
                    do_except(cpu, X_PPFW);
                    return;
                }
            }
            
            cpu->a[13] = temp_sp;
            set_pc(cpu, ea + 1);
        } break;
        case 1: { // RET
            uint64_t temp_sp = cpu->a[13];
            uint64_t last_two[2]; // return addr, mask
            
            for (int i = 0; i < 2; i++) {
                uint64_t r_res =
                    read_mem(
                        cpu, cpu->c[C_PSW] >> 28, temp_sp++
                    );
                if (r_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                } else if (r_res == KEY_FAULT) {
                    do_except(cpu, X_PPFR);
                    return;
                }
                last_two[i] = r_res & MASK_36;
            }
            
            uint64_t mask = last_two[1];
            int restored_sp = 0;
            
            for (int i = 0; i < 16; i++) {
                if ((mask << (15 - i)) & 1) {
                    int reg = i;
                    uint64_t r_res =
                        read_mem(
                            cpu, cpu->c[C_PSW] >> 28, temp_sp++
                        );
                    if (r_res == MEM_FAULT) {
                        do_except(cpu, X_MEMX);
                        return;
                    } else if (r_res == KEY_FAULT) {
                        do_except(cpu, X_PPFR);
                        return;
                    }
                    
                    cpu->a[reg] = r_res & MASK_36;
                    if (reg == 13) restored_sp = 1;
                }
            }
            
            set_pc(cpu, last_two[0]);
            if (!restored_sp) cpu->a[13] = temp_sp;
        } break;
        default: {
            // UMR
            do_except(cpu, X_USER);
        }
    }
}

/**
 * @brief Execute an instruction with two (three) accumulator(s)
 *
 * These are the type "AA" instructions from opcode 700+; the format is as
 * follows:
 *    - Opcode: 4 bits
 *    - Rotate through carry: 1 bit
 *       - If this bit is set, the implicit rotate operation will include the
 *         carry flag for a 37-bit rotate
 *    - Source accumulator select \a m: 4 bits
 *    - Target accumulator select \a n: 4 bits
 *    - Function: 3 bits
 *       - Opcode 14, fn 0: @c OCA - result is one's complement of AC\a m
 *       - Opcode 14, fn 1: @c NEA - result is two's complement of AC\a m
 *       - Opcode 14, fn 2: @c DAA - result is AC\a m
 *       - Opcode 14, fn 3: @c ICA - result is AC\a m + 1, complement carry flag
 *         on carry out
 *       - Opcode 14, fn 4: @c ACA - result is AC\a m + one's complement of
 *         AC\a n, complement carry flag on carry out
 *       - Opcode 14, fn 5: @c SUA - result is AC\a m + two's complement of
 *         AC\a n, complement carry flag on carry out
 *       - Opcode 14, fn 6: @c ADA - result is AC\a m + AC\a n, complement carry
 *         flag on carry out
 *       - Opcode 14, fn 7: @c OCA - result is AC\a m bitwise AND AC\a n
 *       - Opcode 15, fn 2: @c IOA - result is AC\a m bitwise OR AC\a n
 *       - Opcode 15, fn 6: @c XOA - result is AC\a m bitwise OR AC\a n
 *       - Any other combination: result is 0
 *    - Carry flag mode: 2 bits; do this BEFORE arithmetic operation
 *       - 0: Preserve carry flag
 *       - 1: Clear carry flag
 *       - 2: Set carry flag
 *       - 3: Complement carry flag
 *    - Skip mode: 3 bits; do this AFTER instruction is complete
 *       - 0: Do not skip next instruction
 *       - 1: Always skip next instruction
 *       - 2: Skip if carry flag unset
 *       - 3: Skip if carry flag set
 *       - 4: Skip if result is zero
 *       - 5: Skip if result is nonzero
 *       - 6: Skip if carry flag and/or result is zero
 *       - 7: Skip if carry flag and result are nonzero
 *    - No load: 1 bit, do not save result if set
 *    - Bit mask: Signed 7 bits; AFTER rotate
 *       - After the implicit rotate operation, the carry flag will replace this
 *         many bits from the left (most significant) if the value of this field
 *         is positive, otherwise it will replace that many bits from the right
 *       - The result of using a bit mask width \a x such that \a |x| > 36 is
 *         not well defined UNLESS the first three bits are equal to 4 (1, 0, 0)
 *       - In that case, the remaining four bits are used to encode a third
 *         accumulator select \a d, the final result of the operation is stored
 *         in that accumulator and the bit mask width is set equal to the rotate
 *         field (i.e. a non-zero rotate value will effect a bit shift rather
 *         than rotate); otherwise the result is stored in AC\a n
 *    - Rotate: Signed 7 bits; AFTER arithmetic operation but BEFORE mask
 *       - The initial result of the arithmetic operation is rotated (including
 *         the new carry flag if the rotate-through-carry bit is set) this many
 *         bits to the left (right if value is negative)
 *       - The result of a rotation \a x such that \a |x| > 36 (37 if rotating
 *         through carry) is not well defined
 *       
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 * @param a First operand
 * @param b Second operand
 * @param c Current carry flag
 */
uint64_t exec_aa(
    uint64_t inst,
    uint64_t a, uint64_t b, int c
) {
    int op = (int) ((inst >> 20) & 0x7);
    op |= (int) ((inst >> 29) & 0x8);
    int ci = (int) ((inst >> 18) & 0x3);
    int cond = (int) ((inst >> 15) & 0x7);
    int nl = (int) ((inst >> 14) & 0x1);
    int rc = (int) ((inst >> 31) & 0x1);
    
    uint64_t mk_u = ((inst >> 7) & 0x3F);
    uint64_t rt_u = (inst & 0x3F);
    
    int mk = (int) (EXT7(mk_u));
    int rt = (int) (EXT7(rt_u));
    
    uint64_t result = compute(a, b, c, op, ci, cond, nl, rc, mk, rt);
    return result;
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
    else if (inst >> 27 == 030) {
        exec_md(cpu, inst);
    }
    else if (inst >> 27 == 0100) {
        exec_call(cpu, inst);
    }
    else if (inst >> 27 == 0670) {
        exec_io1(cpu, inst);
    }
    else if (inst >> 33 == 06) {
        exec_smi(cpu, inst);
    }
    else {
        // Illegal
        do_except(cpu, X_INST);
    }
}

void *run(void *vctx) {
    ist66_cu_t *cpu = (ist66_cu_t *) vctx;
    
    fprintf(stderr, "/CPU-I-STARTING\n");
    
    do {
        int done_edit = 0;
        if (cpu->do_edit) {
            exec_all(cpu, cpu->xeq_inst);
            cpu->do_edit = 0;
            if (cpu->do_edsk) {
                set_pc(cpu, get_pc(cpu) + 1);
                cpu->do_edsk = 0;
            }
            done_edit = 1;
        }
        
        uint64_t current_irql = (cpu->c[C_CW] >> 32) & 0xF;
        if (cpu->min_pending < current_irql) {
            // fprintf(stderr, "%ld -> %d\n", current_irql, cpu->min_pending);
            do_intr(cpu, cpu->min_pending);
        }
        
        if (cpu->running) {
            if (!done_edit) {
                uint64_t inst = read_mem(cpu, cpu->c[C_PSW] >> 28, get_pc(cpu));
                if (inst == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                } else if (inst == KEY_FAULT) {
                    do_except(cpu, X_PPFR);
                } else {
                    exec_all(cpu, inst);
                }
            }
        } else {
            pthread_mutex_lock(&cpu->lock);
            if (current_irql == 0x0 || cpu->mask == 0) {
                cpu->exit = 1;
            } else if (!cpu->exit) {
                while (!cpu->running) {
                    pthread_cond_wait(&cpu->intr_cond, &cpu->lock);
                }
            }
            pthread_mutex_unlock(&cpu->lock);
        }
        
        if (cpu->do_inc) {
            uint64_t w_res =
                write_mem
                    (cpu, cpu->c[C_PSW] >> 28, cpu->inc_addr, cpu->inc_data);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
            }
            cpu->do_inc = 0;
        }
    } while (!cpu->exit || cpu->do_edit);
    
    fprintf(stderr, "/CPU-I-STOP CODE %012lo\n", cpu->stop_code);
    return NULL;
}

void init_cpu(ist66_cu_t *cpu, uint64_t mem_size, int max_io) {
    memset(cpu, 0, sizeof(ist66_cu_t));
    
    cpu->memory = calloc(sizeof(uint64_t), mem_size);
    cpu->mem_size = mem_size;
    
    cpu->io_destroy = calloc(sizeof(ist66_io_dtor_t), max_io);
    cpu->io = calloc(sizeof(ist66_io_t), max_io);
    cpu->ioctx = calloc(sizeof(void *), max_io);
    cpu->max_io = max_io;
    cpu->mask = 0xFFFF;
    
    pthread_mutex_init(&cpu->lock, NULL);
    pthread_cond_init(&cpu->intr_cond, NULL);
    fprintf(stderr, "/CPU-I-INIT TYPE 66/10 %ldW %d MAXDEV\n", mem_size, max_io);
}

void start_cpu(ist66_cu_t *cpu, int do_step) {
    cpu->running = 1;
    cpu->exit = do_step;
    pthread_create(&cpu->thread, NULL, run, cpu);
}

void stop_cpu(ist66_cu_t *cpu) {
    cpu->running = 1;
    cpu->exit = 1;
    pthread_cond_signal(&cpu->intr_cond);
    pthread_join(cpu->thread, NULL);
    cpu->running = 0;
}

void wait_for_cpu(ist66_cu_t *cpu) {
    pthread_join(cpu->thread, NULL);
    cpu->running = 0;
}

void destroy_cpu(ist66_cu_t *cpu) {
    if (!cpu->exit && cpu->running) stop_cpu(cpu);
    
    for (int i = 0; i < cpu->max_io; i++) {
        if (cpu->io_destroy[i] != NULL) {
            cpu->io_destroy[i](cpu, i);
        }
    }
    
    free(cpu->memory);
    free(cpu->io_destroy);
    free(cpu->io);
    free(cpu->ioctx);
    pthread_mutex_destroy(&cpu->lock);
    pthread_cond_destroy(&cpu->intr_cond);
    
    fprintf(stderr, "/CPU-I-CLOSED\n");
}

int main(int argc, char *argv[]) {
    ist66_cu_t cpu;
    
    init_cpu(&cpu, 65536, 512);

    init_ppt(&cpu, 012, 4);
    init_lpt_ex(&cpu, 013, 5, "/dev/null");
    init_pch(&cpu, 014, 6);
    
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
    
    char cmd[512];
    int running = 1;
    uint64_t ptr = 0;
    
    while (running) {
        printf("> ");
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;
        cmd[strcspn(cmd, "\n")] = 0;
        cmd[sizeof(cmd) - 1] = 0;
        
        int i = 0;
        while ((cmd[i] == ' ' || cmd[i] == '\t') && i < sizeof(cmd) - 1) i++;
        
        if (cmd[i]) {
            if (cmd[i] == '/') {
                char *end;
                uint64_t new_ptr = strtol(cmd + i + 1, &end, 8);
                if (end > cmd + i + 1 && new_ptr <= 0777777777) {
                    ptr = new_ptr;
                    i += end - (cmd + i);
                }
                else {
                    printf("? Bad address\n");
                    continue;
                }
            }
            
            while ((cmd[i] == ' ' || cmd[i] == '\t') && i < sizeof(cmd) - 1)
                i++;
            
            if (cmd[i] == '?') {
                printf("%09lo\n", ptr & MASK_ADDR);
            }
            
            else if (cmd[i] == '.') {
                i++;
                while (
                    (cmd[i] == ' ' || cmd[i] == '\t')
                    && i < sizeof(cmd) - 1
                ) i++;
                
                uint64_t to_print;
                if (cmd[i] == '\0') to_print = 1;
                else {
                    char *end;
                    to_print = strtol(cmd + i, &end, 8);
                    if (!(end > cmd + i && to_print <= 0777777777)) {
                        printf("? Bad count\n");
                        continue;
                    }
                }
                
                for (int j = 0; j < to_print; j++) {
                    if (j == 0) printf("%09lo: ", ptr & MASK_ADDR);
                    else if (j % 4 == 0) printf("\n%09lo: ", ptr & MASK_ADDR);
                    uint64_t data = read_mem
                        (&cpu, 0, ptr++ & MASK_ADDR);
                    if (data & MEM_FAULT) {
                        printf("? Bad address\n");
                        break;
                    } else {
                        printf("%012lo ", data & MASK_36);
                    }
                }
                printf("\n");
            }
            
            else if (cmd[i] == '=') {
                char *saveptr = NULL;
                char *rest = cmd + i + 1;
                char *tok = NULL;
                
                while ((tok = strtok_r(rest, " \t", &saveptr))) {
                    rest = NULL;
                    char *end = NULL;
                    uint64_t data = strtol(tok, &end, 8);
                    if (!(end > tok && data <= 0777777777777)) {
                        printf("? Bad data\n");
                        break;
                    }
                    
                    uint64_t result = write_mem
                        (&cpu, 0, ptr++ & MASK_ADDR, data);
                    if (result & MEM_FAULT) {
                        printf("? Bad address\n");
                        break;
                    }
                }
            }
            
            else if (cmd[i] == 'W') {
                start_cpu(&cpu, 0);
                wait_for_cpu(&cpu);
                int c;
                while ((c = getchar()) != '\n' && c != EOF) { }
            }
            
            else if (cmd[i] == 'S') {
                start_cpu(&cpu, 0);
            }
            
            else if (cmd[i] == 'P') {
                if (cpu.running)
                    stop_cpu(&cpu);
                ptr = get_pc(&cpu);
            }
            
            else if (cmd[i] == 'G') {
                set_pc(&cpu, ptr);
                if (cmd[i + 1] == 'W') {
                    start_cpu(&cpu, 0);
                    wait_for_cpu(&cpu);
                    int c;
                    while ((c = getchar()) != '\n' && c != EOF) { }
                } else if (cmd[i + 1] == 'S') {
                    start_cpu(&cpu, 0);
                }
            }
            
            else if (cmd[i] == 'X') {
                running = 0;
            }
        }
    }
    
    destroy_cpu(&cpu);
    return 0;
}

