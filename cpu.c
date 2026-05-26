#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "alu.h"
#include "fpu.h"
#include "cpu.h"
#include "tty.h"

seg_cache_t *seg_lookup(ist66_cu_t *cpu, int selector) {
    uint8_t cache_row = selector & 0x1F;
    uint16_t cache_key = selector >> 5;
    
    if (
        (cpu->seg_cache[cache_row].key != cache_key) || // not cached
        (!(cpu->seg_cache[cache_row].tag & (1 << 27))) // not present
    ) { // go fish
        if (selector > cpu->c[C_SDR] >> 27) return NULL; // no such segment
        
        uint64_t descriptor_addr = (cpu->c[C_SDR] & MASK_ADDR)
            + (selector << 1);
        
        if (descriptor_addr >= cpu->mem_size - 1) return NULL;
        
        uint64_t tag = cpu->memory[descriptor_addr + 1] & MASK_36;
        if (!(tag & (1 << 27))) return NULL; // still not present
        
        cpu->seg_cache[cache_row].base =
            cpu->memory[descriptor_addr] & MASK_36;
        cpu->seg_cache[cache_row].tag = tag;
        cpu->seg_cache[cache_row].key = cache_key;
    }
    // printf("segment lookup success\n");
    return &(cpu->seg_cache[cache_row]);
}

tlb_entry_t *tlb_lookup(ist66_cu_t *cpu, int selector, seg_cache_t *pg_table) {
    uint8_t cache_row = selector & 0x1F;
    uint16_t cache_key = selector >> 5;
    uint16_t page_select = selector & 0x1FF;
    
    if (
        (cpu->tlb[cache_row].key != cache_key) || // not cached
        (!(cpu->tlb[cache_row].rights & TLB_PRESENT)) // not present
    ) { // go fish
        uint64_t descriptor_addr = pg_table->base + page_select;
        
        if (descriptor_addr >= cpu->mem_size) return NULL;
        
        uint8_t rights = (cpu->memory[descriptor_addr] & 0x1E0) >> 5;
        if (!(rights & TLB_PRESENT)) return NULL; // still not present
        
        cpu->tlb[cache_row].pg_base =
            cpu->memory[descriptor_addr] & 0777777777000;
        cpu->tlb[cache_row].rights = rights;
        cpu->tlb[cache_row].key = cache_key;
    }
    
    return &(cpu->tlb[cache_row]);
}

void tlb_invalidate(ist66_cu_t *cpu, int selector) {
    cpu->tlb[selector & 0x1F].rights = 0;
}

void tlb_invalidate_all(ist66_cu_t *cpu) {
    for (int i = 0; i < 32; i++) {
        if (!(cpu->tlb[i].rights & TLB_GLOBAL)) {
            cpu->tlb[i].rights = 0;
        }
    }
}

void seg_invalidate(ist66_cu_t *cpu, int selector) {
    cpu->seg_cache[selector & 0x1F].tag = 0;
    // tlb_invalidate_all(cpu);
}

void seg_invalidate_all(ist66_cu_t *cpu) {
    for (int i = 0; i < 32; i++) {
        if (!(cpu->seg_cache[i].tag & (1 << 25))) {
            cpu->seg_cache[i].tag = 0;
        }
    }
}

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
 * IOCPU's only have interrupt 1, control register 1 nonzero to enable
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

uint64_t read_vmem(ist66_cu_t *cpu, uint8_t key, uint32_t vaddress) {
    vaddress &= MASK_ADDR;
    
    seg_cache_t *seg = seg_lookup(cpu, vaddress >> 18);
    if (seg == NULL) {
        // printf("Segment not present\n");
        cpu->c[C_SF] = vaddress | SEG_FAULT_PRESENT;
        return KEY_FAULT;
    }
    
    uint8_t seg_key = seg->tag >> 28;
    
    if (
        key &&
        seg_key != 0xFE &&
        seg_key != 0xFF &&
        seg_key != key
    ) {
        cpu->c[C_SF] = vaddress | SEG_FAULT_KEY;
        // printf("Segment key error\n");
        return KEY_FAULT;
    }
    
    uint64_t offset = vaddress & 0x3FFFF;
    if (!((seg->tag >> 24) & 1) && offset > (seg->tag & 0x3FFFF)) {
        // printf("Segment bounds error\n");
        cpu->c[C_SF] = vaddress | SEG_FAULT_BOUNDS;
        return KEY_FAULT;
    }
    
    uint64_t address = (seg->base + offset) & MASK_36;
    
    if (((seg->tag >> 24) & 1)) {
        tlb_entry_t *entry = tlb_lookup(cpu, vaddress >> 9, seg);
        if (entry == NULL) {
            // printf("Segment page fault\n");
            cpu->c[C_SF] = vaddress | SEG_FAULT_PRESENT | SEG_FAULT_PAGE;
            return KEY_FAULT;
        }
        address = entry->pg_base + (vaddress & 0x1FF);
    }
    
    
    if (address >= cpu->mem_size) return MEM_FAULT;
    
    return cpu->memory[address] & MASK_36;
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
    if (cpu->c[C_SDR] != 0) return read_vmem(cpu, key, address);
    
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

uint64_t write_vmem(
    ist66_cu_t *cpu,
    uint8_t key,
    uint32_t vaddress,
    uint64_t data
) {
    vaddress &= MASK_ADDR;
    
    seg_cache_t *seg = seg_lookup(cpu, vaddress >> 18);
    if (seg == NULL) {
        cpu->c[C_SF] = vaddress | SEG_FAULT_PRESENT | SEG_FAULT_WRITE;
        return KEY_FAULT;
    }
    
    uint8_t seg_key = seg->tag >> 28;
    
    if (
        key &&
        seg_key != 0xFF &&
        seg_key != key
    ) {
        cpu->c[C_SF] = vaddress | SEG_FAULT_KEY | SEG_FAULT_WRITE;
        return KEY_FAULT;
    }
    
    if (!((seg->tag >> 26) & 1)) {
        cpu->c[C_SF] = vaddress | SEG_FAULT_RIGHTS | SEG_FAULT_WRITE;
        return KEY_FAULT;
    }
    
    uint64_t offset = vaddress & 0x3FFFF;
    if (!((seg->tag >> 24) & 1) && offset > (seg->tag & 0x3FFFF)) {
        cpu->c[C_SF] = vaddress | SEG_FAULT_BOUNDS | SEG_FAULT_WRITE;
        return KEY_FAULT;
    }
    
    uint64_t address = (seg->base + offset) & MASK_36;
    
    if (((seg->tag >> 24) & 1)) {
        tlb_entry_t *entry = tlb_lookup(cpu, vaddress >> 9, seg);
        if (entry == NULL) {
            cpu->c[C_SF] = vaddress | SEG_FAULT_PRESENT | SEG_FAULT_PAGE;
            return KEY_FAULT;
        }
        if (!(entry->rights & TLB_WRITE)) {
            cpu->c[C_SF] = vaddress | SEG_FAULT_RIGHTS | SEG_FAULT_WRITE | SEG_FAULT_PAGE;
            return KEY_FAULT;
        }
        address = entry->pg_base + (vaddress & 0x1FF);
    }
    
    if (address >= cpu->mem_size) return MEM_FAULT;
    
    uint64_t old_tag = cpu->memory[address] & ~(MASK_36);
    cpu->memory[address] = old_tag | (data & MASK_36);
    return 0;
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
    if (cpu->c[C_SDR] != 0) return write_vmem(cpu, key, address, data);
    
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
            cpu->do_stack = 1;
            ea_l = cpu->a[13];
            cpu->next_stack = (cpu->a[13] + disp) & MASK_36;
        } break;
        case 15: {
            cpu->do_stack = 1;
            cpu->next_stack = (cpu->a[13] - disp) & MASK_36;
            ea_l = cpu->next_stack;
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
            
            uint64_t result = compute(data, 1, 0, 6, 0, 4, 0, 0, 0);
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
            
            uint64_t result = compute(1, data, 0, 5, 0, 4, 0, 0, 0);
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
        case 4: { // SZR
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            if (data == 0) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        case 5: { // SNZ
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            if (data != 0) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
        } break;
        case 14: { // CALL
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
                if ((mask >> i) & 1) {
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
        case 15: { // RET
            uint64_t temp_sp = cpu->a[13] + ea;
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
                if ((mask >> (15 - i)) & 1) {
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
                low, cpu->a[0], 0, 6, 0, 0, 0, 0, 0
            );
            
            uint64_t carry_l = result_l >> 36;
            
            uint64_t result_h = compute(
                high + carry_l, cpu->a[2], get_cf(cpu), 6, 0, 0, 0, 0, 0
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
                low, cpu->a[0], 0, 6, 0, 0, 0, 0, 0
            );
            
            uint64_t carry_l = result_l >> 36;
            
            uint64_t result_h = compute(
                high + carry_l, cpu->a[2], get_cf(cpu), 6, 0, 0, 0, 0, 0
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
        case 4: { // MU
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            xmulu(cpu->a[1], data, &cpu->a[0], &cpu->a[2]);

            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 5: { // MAU
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

            xmulu(cpu->a[1], data, &low, &high);

            uint64_t result_l = compute(
                low, cpu->a[0], 0, 6, 0, 0, 0, 0, 0
            );

            uint64_t carry_l = result_l >> 36;

            uint64_t result_h = compute(
                high + carry_l, cpu->a[2], get_cf(cpu), 6, 0, 0, 0, 0, 0
            );

            cpu->a[0] = result_l & MASK_36;
            cpu->a[2] = result_h & MASK_36;

            set_cf(cpu, (result_h >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 6: { // MNAU
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

            xmulu(cpu->a[1], data, &low, &high);

            uint64_t result_l = compute(
                low, cpu->a[0], 0, 6, 0, 0, 0, 0, 0
            );

            uint64_t carry_l = result_l >> 36;

            uint64_t result_h = compute(
                high + carry_l, cpu->a[2], get_cf(cpu), 6, 0, 0, 0, 0, 0
            );

            cpu->a[0] = result_l & MASK_36;
            cpu->a[2] = result_h & MASK_36;

            set_cf(cpu, (result_h >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 7: { // DU
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

            uint64_t ac = cpu->a[0];

            cpu->a[1] = ((uint64_t) (ac / data)) & MASK_36;
            cpu->a[2] = ((uint64_t) (ac % data)) & MASK_36;

            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        default: {
            // UMR
            do_except(cpu, X_USER);
        }
    }
}

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
        case 041: { // EDIT
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
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0
            );
            cpu->do_edit = 1;
            cpu->xeq_inst = result & MASK_36;
        } break;
        case 042: { // EDSK
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
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0
            );
            cpu->do_edit = 1;
            cpu->do_edsk = 1;
            cpu->xeq_inst = result & MASK_36;
        } break;
        case 043: { // MOVEA
            cpu->a[ac] = ea;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 044: { // ADDEA
            uint64_t result = compute(
                ea, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 045: { // ISE
            uint64_t result = compute(
                1, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0
            );
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            if (data == (result & MASK_36)) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
            
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
        } break;
        case 046: { // DSE
            uint64_t result = compute(
                1, cpu->a[ac], get_cf(cpu), 5, 0, 0, 0, 0, 0
            );
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            if (data == (result & MASK_36)) {
                set_pc(cpu, get_pc(cpu) + 2);
            } else {
                set_pc(cpu, get_pc(cpu) + 1);
            }
            
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
        } break;
        case 047: { // MOVEAS
            cpu->a[ac] = (ea << 17) & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 050: { // LDCOM
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
                data, 0, 0, 0, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 051: { // LDNEG
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
                data, 0, 0, 1, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 052: { // LDA
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
        case 053: { // STA
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
        case 054: { // ADCM
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
                data, cpu->a[ac], get_cf(cpu), 4, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 055: { // SUBM
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
                data, cpu->a[ac], get_cf(cpu), 5, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 056: { // ADDM
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
                data, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 057: { // ANDM
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
                data, cpu->a[ac], get_cf(cpu), 7, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 062: { // ORM
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
                data, cpu->a[ac], get_cf(cpu), 10, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 066: { // XORM
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
                data, cpu->a[ac], get_cf(cpu), 14, 0, 0, 0, 0, 0
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

void exec_fm(ist66_cu_t *cpu, uint64_t inst) {
    if ((cpu->c[C_FCW] & 4) == 0) {
        do_except(cpu, X_NFPU);
        return;
    }

    uint64_t ea = comp_mr(cpu, inst);
    uint64_t ac = ((inst >> 23) & 0x3) | ((cpu->c[C_FCW] & 3) << 2);

    int normalize = !!(inst & (1 << 26));
    int round = !!(inst & (1 << 25));

    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }

    switch ((inst >> 27) & 0x1FF) {
        case 0400: { // LF
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            set_f36(&data, &cpu->f[ac]);
            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0401: { // STF
            int status = 0;
            uint64_t result;
            rdc700_float_t temp = {
                .sign_exp = cpu->f[ac].sign_exp,
                .signif = cpu->f[ac].signif
            };
            if (normalize) {
                rdc700_fnorm(&temp, &temp);
            }
            if (round) {
                status |= f80_round_to_f36(&temp, &temp);
            }
            status |= get_f36(&temp, &result);

            uint64_t w_res =
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0402: { // AF
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            rdc700_float_t temp;
            set_f36(&data, &temp);
            int status = rdc700_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0403: { // SF
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            rdc700_float_t temp;
            set_f36(&data, &temp);
            rdc700_fneg(&temp, &temp);
            int status = rdc700_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0404: { // MF
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            rdc700_float_t temp;
            set_f36(&data, &temp);
            int status = rdc700_fmul(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0405: { // DF
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            rdc700_float_t temp;
            set_f36(&data, &temp);
            // rdc700_fnorm(&temp, &temp);
            
            int status = rdc700_fdiv(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0406: { // LG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            uint64_t data_l = read_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1);
            if (data_l == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data_l == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data_l &= MASK_36;

            set_f72(&data, &data_l, &cpu->f[ac]);
            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0407: { // STG
            int status = 0;
            uint64_t result, result_l;
            rdc700_float_t temp = {
                .sign_exp = cpu->f[ac].sign_exp,
                .signif = cpu->f[ac].signif
            };
            if (normalize) {
                rdc700_fnorm(&temp, &temp);
            }
            if (round) {
                status |= f80_round_to_f72(&temp, &temp);
            }
            status |= get_f72(&temp, &result, &result_l);

            uint64_t w_res =
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }

            w_res = write_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1, result_l);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0410: { // AG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            uint64_t data_l = read_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1);
            if (data_l == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data_l == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data_l &= MASK_36;

            rdc700_float_t temp;
            set_f72(&data, &data_l, &temp);
            int status = rdc700_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0411: { // SG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            uint64_t data_l = read_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1);
            if (data_l == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data_l == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data_l &= MASK_36;

            rdc700_float_t temp;
            set_f72(&data, &data_l, &temp);
            rdc700_fneg(&temp, &temp);
            int status = rdc700_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0412: { // MG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            uint64_t data_l = read_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1);
            if (data_l == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data_l == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data_l &= MASK_36;

            rdc700_float_t temp;
            set_f72(&data, &data_l, &temp);
            int status = rdc700_fmul(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0413: { // DG
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            uint64_t data_l = read_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1);
            if (data_l == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data_l == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data_l &= MASK_36;

            rdc700_float_t temp;
            set_f72(&data, &data_l, &temp);
            // rdc700_fnorm(&temp, &temp);
            
            int status = rdc700_fdiv(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                rdc700_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0414: { // LE
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            int64_t exp = (int64_t) (EXT36(data));
            if (exp < -16383) {
                cpu->a[2] |= F_UNDF;
                cpu->f[ac].sign_exp &= 0x8000;
            }
            else if (exp > 16384) {
                cpu->a[2] |= F_OVRF;
                cpu->f[ac].sign_exp |= 0x7FFF;
            }
            else {
                cpu->f[ac].sign_exp &= 0x8000;
                cpu->f[ac].sign_exp |= (exp + 16383) & 0x7FFF;
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0415: { // STE
            uint64_t result = cpu->f[ac].sign_exp;
            result -= 16383;
            result &= MASK_36;

            uint64_t w_res =
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0416: { // LS
            uint64_t data_h = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data_h == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data_h == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data_h &= MASK_36;

            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            data |= data_h << 36;
            data_h >>= 28;

            int new_sign = !!(data_h & (1 << 7));
            if (new_sign) {
                data_h = (~data_h) & 0xFF;
                data = ~data;
                if (data + 1 < data) {
                    data_h = (data_h + 1) & 0xFF;
                }
                data++;
            }
            
            if (data_h) {
                cpu->a[2] |= F_OVRF;
                cpu->f[ac].signif = 0;
            } else {
                cpu->f[ac].sign_exp = cpu->f[ac].sign_exp & 0x7FFF;
                cpu->f[ac].sign_exp |= 0x8000 * new_sign;
                cpu->f[ac].signif = data;
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        
        case 0417: { // STS
            uint64_t result = 0, result_l = cpu->f[ac].signif;
            if ((cpu->f[ac].sign_exp & 0x8000)) {
                result = (~result) & 0xFF;
                result_l = ~result_l;
                if (result_l + 1 < result_l) {
                    result = (result + 1) & 0xFF;
                }
                result_l++;
            }
            
            result = (result << 28) | (result_l >> 36);
            result_l &= MASK_36;

            uint64_t w_res =
            write_mem(cpu, cpu->c[C_PSW] >> 28, ea, result);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }

            w_res = write_mem(cpu, cpu->c[C_PSW] >> 28, ea + 1, result_l);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }
            
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        default: {
            // Illegal
            do_except(cpu, X_INST);
        }
    }
}

void exec_fr(ist66_cu_t *cpu, uint64_t inst) {
    if ((cpu->c[C_FCW] & 4) == 0) {
        do_except(cpu, X_NFPU);
        return;
    }

    uint64_t tgt = ((inst >> 23) & 0x3) | ((cpu->c[C_FCW] & 3) << 2);
    uint64_t src = ((inst >> 20) & 0x3) | ((cpu->c[C_FCW] & 3) << 2);
    uint64_t dst = ((inst >> 18) & 0x3) | ((cpu->c[C_FCW] & 3) << 2);

    int normalize = !!(inst & (1 << 26));
    int round = !!(inst & (1 << 25));
    int round_size = !!(inst & (1 << 14));

    rdc700_float_t temp;
    int status = 0;

    switch ((inst >> 27) & 0x1FF) {
        case 0440: { // LL
            temp.sign_exp = cpu->f[src].sign_exp;
            temp.signif = cpu->f[src].signif;
        } break;
        
        case 0441: { // NL
            temp.sign_exp = cpu->f[src].sign_exp;
            temp.signif = cpu->f[src].signif;
            rdc700_fneg(&temp, &temp);
        } break;
        
        case 0442: { // AL
            status = rdc700_fadd(&cpu->f[src], &cpu->f[tgt], &temp);
        } break;
        
        case 0443: { // SL
            temp.sign_exp = cpu->f[tgt].sign_exp;
            temp.signif = cpu->f[tgt].signif;
            rdc700_fneg(&temp, &temp);
            status = rdc700_fadd(&cpu->f[src], &temp, &temp);
        } break;
        
        case 0444: { // ML
            status = rdc700_fmul(&cpu->f[src], &cpu->f[tgt], &temp);
        } break;
        
        case 0445: { // DL
            status = rdc700_fdiv(&cpu->f[src], &cpu->f[tgt], &temp);
        } break;

        default: {
            // Illegal
            do_except(cpu, X_INST);
        }
    }
    
    if (normalize) {
        rdc700_fnorm(&temp, &temp);
    }
    if (round) {
        if (round_size) {
            status |= f80_round_to_f72(&temp, &temp);
        } else {
            status |= f80_round_to_f36(&temp, &temp);
        }
    }
    
    if (!(inst & (1L << 22))) {
        cpu->f[dst].signif = temp.signif;
        cpu->f[dst].sign_exp = temp.sign_exp;
    }
    
    cpu->a[2] |= status;
    
    int skip = 0;
    switch ((inst >> 15) & 0x7) {
        case 1: skip = 1;                               break;
        case 2: skip = !(temp.sign_exp & 0x8000);       break;
        case 3: skip = (temp.sign_exp & 0x8000);        break;
        case 4: skip = is_zero(&temp);                  break;
        case 5: skip = !is_zero(&temp);                 break;
        case 6: skip = !is_inf(&temp);                  break;
        case 7: skip = !is_nan(&temp);                  break;
    }
    
    if (skip) {
        set_pc(cpu, get_pc(cpu) + 2);
    } else {
        set_pc(cpu, get_pc(cpu) + 1);
    }
}

void exec_bx(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t ac = (inst >> 23) & 0xF;
    uint64_t ix = (inst >> 18) & 0xF;
    uint64_t bs = inst & 0x3F;
    
    uint64_t ea = cpu->a[ix] & MASK_ADDR;
    uint64_t sh = cpu->a[ix] >> 27;
    
    switch ((inst >> 27) & 0x1FF) {
        case 0100: { // LCH
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            data >>= sh;
            data &= (1L << bs) - 1;
            
            cpu->a[ac] = data;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 0101: { // DCH
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;
            
            uint64_t mask = ((1L << bs) - 1) << sh;
            uint64_t wr_data = (cpu->a[ac] << sh) & mask;
            data &= ~mask;
            data |= wr_data;
            
            uint64_t w_res =
                write_mem(cpu, cpu->c[C_PSW] >> 28, ea, data);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }
            
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 0102: { // ICX
            sh -= bs;
            if (sh > 36) {
                sh = (36 - bs) & 0x3F;
                ea = (ea + 1) & MASK_ADDR;
            }
            cpu->a[ac] = ea | (sh << 27);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 0103: { // ILC
            sh -= bs;
            if (sh > 36) {
                sh = (36 - bs) & 0x3F;
                ea = (ea + 1) & MASK_ADDR;
            }
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            
            cpu->a[ix] = ea | (sh << 27);
            
            data &= MASK_36;
            
            data >>= sh;
            data &= (1L << bs) - 1;
            
            cpu->a[ac] = data;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 0104: { // IDC
            sh -= bs;
            if (sh > 36) {
                sh = (36 - bs) & 0x3F;
                ea = (ea + 1) & MASK_ADDR;
            }
            
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            
            data &= MASK_36;
            // fprintf(stderr, "Write char %ld -> %ld\n", cpu->a[ac], ea);
            
            uint64_t mask = ((1L << bs) - 1) << sh;
            uint64_t wr_data = (cpu->a[ac] << sh) & mask;
            data &= ~mask;
            data |= wr_data;
            
            uint64_t w_res =
                write_mem(cpu, cpu->c[C_PSW] >> 28, ea, data);
            if (w_res == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (w_res == KEY_FAULT) {
                do_except(cpu, X_PPFW);
                return;
            }
            
            cpu->a[ix] = ea | (sh << 27);
            
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        default: {
            // Illegal
            do_except(cpu, X_INST);
        }
    }
}

void exec_local_trap(ist66_cu_t *cpu, uint64_t inst) {
    uint64_t opcode = ((inst >> 27) & 0x1FF);
    if (opcode >= 0300) { // SLT, set key to 0 and save full PSW
        if (!((cpu->c[C_SLT] >> 27) & 1)) {
            do_except(cpu, X_USER);
            return;
        }
        cpu->a[15] = cpu->c[C_PSW];
        cpu->c[C_PSW] &= (1 << 27);
        set_pc(cpu, (cpu->c[C_SLT] + (opcode & 077)) & MASK_ADDR);
    } else { // PLT, preserve key and save only program counter
        if (!((cpu->c[C_PLT] >> 27) & 1)) {
            do_except(cpu, X_USER);
            return;
        }
        cpu->a[15] = get_pc(cpu);
        set_pc(cpu, (cpu->c[C_PLT] + (opcode & 077)) & MASK_ADDR);
    }
    cpu->a[14] = inst;
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
            case 070: { // HLT
                halt(cpu);
                cpu->stop_code = cpu->a[ac];
                set_pc(cpu, ea);
            } break;
            case 071: { // INT
                set_pc(cpu, ea);
                do_intr(cpu, ac);
            } break;
            case 010: { // various
                switch (ac) {
                    case 0: { // RFI
                        leave_intr(cpu);
                        set_pc(cpu, get_pc(cpu) + ea);
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
                    case 3: { // MWAIT
                        uint64_t data = read_mem(cpu, 0, ea);
                        if (data == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        data &= MASK_36;

                        intr_set_mask(cpu, data);
                        halt(cpu);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 4: { // STMSK
                        uint64_t w_res = write_mem(cpu, 0, ea, cpu->mask);
                        if (w_res == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 5: { // INVSM
                        seg_invalidate(cpu, ea >> 18);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 6: { // INVPG
                        tlb_invalidate(cpu, (ea >> 9) & 0x1F);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 7: { // SLR
                        cpu->c[C_PSW] = cpu->a[15];
                    } break;
                    default: {
                        // Illegal
                        do_except(cpu, X_INST);
                    }
                }
            } break;
            case 072: { // LDK
                ea &= ~(0x1FF);
                if (ea < cpu->mem_size) {
                    cpu->a[ac] = cpu->memory[ea] >> 36;
                } else {
                    do_except(cpu, X_MEMX);
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 073: { // STK
                uint64_t w_res = set_key(cpu, cpu->a[ac], ea);
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 074: { // LCT
                uint64_t data = read_mem(cpu, 0, ea);
                if (data == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                   return;
                }
                data &= MASK_36;
            
                cpu->c[ac] = data & MASK_36;
                if (ac == C_SDR) {
                    seg_invalidate_all(cpu);
                    tlb_invalidate_all(cpu);
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 075: { // STCTL
                uint64_t w_res = write_mem(cpu, 0, ea, cpu->c[ac & 0x7]);
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 076: { // LXRT
                uint64_t vaddress = ea & MASK_ADDR;
        
                seg_cache_t *seg = seg_lookup(cpu, vaddress >> 18);
                if (seg == NULL) {
                    cpu->c[C_SF] = vaddress | SEG_FAULT_PRESENT;
                    set_pc(cpu, get_pc(cpu) + 1);
                    return;
                }
                
                uint64_t offset = vaddress & 0x3FFFF;
                if (!((seg->tag >> 24) & 1) && offset > (seg->tag & 0x3FFFF)) {
                    cpu->c[C_SF] = vaddress | SEG_FAULT_BOUNDS;
                    set_pc(cpu, get_pc(cpu) + 1);
                    return;
                }
                
                uint64_t address = (seg->base + offset) & MASK_36;
                
                if (((seg->tag >> 24) & 1)) {
                    tlb_entry_t *entry = tlb_lookup(cpu, vaddress >> 9, seg);
                    if (entry == NULL) {
                        cpu->c[C_SF] = vaddress | SEG_FAULT_PRESENT | SEG_FAULT_PAGE;
                        set_pc(cpu, get_pc(cpu) + 1);
                        return;
                    }
                    address = entry->pg_base + (vaddress & 0x1FF);
                }
                
                cpu->c[ac] = address;
                set_pc(cpu, get_pc(cpu) + 2);
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

uint64_t exec_aa(
    uint64_t inst,
    uint64_t a, uint64_t b, int c
) {
    uint64_t result;
    int op = (int) ((inst >> 20) & 0x7);
    op |= (int) ((inst >> 29) & 0x8);
    int ci = (int) ((inst >> 18) & 0x3);
    int cond = (int) ((inst >> 15) & 0x7);
    
    int mode = (int) ((inst >> 14) & 0x1);
    int submode = (int) ((inst >> 13) & 0x1);
    
    if (mode == 0) {
        
        int mr = (int) ((inst >> 12) & 0x1);
        
        int mk = (int) ((inst >> 6) & 0x3F);
        if (mr) {
            mk = -mk;
        }
        
        int rt = (int) (inst & 0x3F);
        
        result = compute(a, b, c, op, ci, cond, submode, mk, rt);
    }
    
    else if (mode == 1 && submode == 0) {
        int mr = (int) ((inst >> 12) & 0x1);
        int rt = (int) (inst & 0x3F);
        if (mr) rt = -rt;
        int mk = -rt;
        
        result = compute(a, b, c, op, ci, cond, 0, mk, rt);
    }
    
    else {
        b = inst & 0x1FFF;
        b = EXT13(b);
        result = compute(a, b, c, op, ci, cond, 0, 0, 0);
    }
    
    return result;
}

void exec_all(ist66_cu_t *cpu, uint64_t inst) {
    cpu->inst = inst;
    
    if (inst >> 33 == 0x7) { // ALU operation
        uint64_t acs = (inst >> 27) & 0xF;
        uint64_t acd = (inst >> 23) & 0xF;
        uint64_t result = exec_aa(inst, cpu->a[acs], cpu->a[acd], get_cf(cpu));
        
        if (((inst >> 13) & 0x3) == 0x2) {
            // ADR encoding; save to alternate register
            acd = (inst >> 6) & 0xF;
        }
        
        if (!((inst >> 31) & 0x1)) cpu->a[acd] = result & MASK_36;
        
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
    else if (inst >> 27 == 1) {
        exec_md(cpu, inst);
    }
    else if (inst >> 27 >= 040 && inst >> 27 <= 067) {
        exec_am(cpu, inst);
    }
    
    else if (inst >> 27 >= 0200 && inst >> 27 < 0400) {
        exec_local_trap(cpu, inst);
    }
    
    else if (inst >> 27 >= 0400 && inst >> 27 < 0420) {
        exec_fm(cpu, inst);
    }
    
    else if (inst >> 27 >= 0440 && inst >> 27 < 0450) {
        exec_fr(cpu, inst);
    }
    
    else if (inst >> 27 == 0640) {
        exec_io1(cpu, inst);
    }
    else if (inst >> 27 >= 0100 && inst >> 27 <= 0104) {
        exec_bx(cpu, inst);
    }
    else if ((inst >> 27 >= 070 && inst >> 27 <= 076) || inst >> 27 == 010) {
        exec_smi(cpu, inst);
    }
    else {
        // Illegal
        do_except(cpu, X_INST);
    }
}
 
void *run(void *vctx) {
    ist66_cu_t *cpu = (ist66_cu_t *) vctx;
    
    // fprintf(stderr, "CPU: starting\n");
    
    do {
        if (cpu->throttle) {
            struct timespec millisecond;
            millisecond.tv_nsec = 33333333;
            millisecond.tv_sec = 0;
            nanosleep(&millisecond, NULL);
        }
        
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
        
        // NOTE: this already gets cancelled on exception
        // see cpu.h
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
        if (cpu->do_stack) {
            cpu->a[13] = cpu->next_stack;
            cpu->do_stack = 0;
        }
        cpu->cycles++;
    } while (!cpu->exit || cpu->do_edit);
    
    cpu->running = 0;
    //fprintf(stderr, "CPU: halted, code %012lo after %ld instructions\n", 
    //    cpu->stop_code, cpu->cycles);
    cpu->cycles = 0;
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
    cpu->min_pending = 0xFFFF;
    cpu->exit = 1;
    cpu->c[0] = 1024;
    
    pthread_mutex_init(&cpu->lock, NULL);
    pthread_cond_init(&cpu->intr_cond, NULL);
    // fprintf(stderr, "CPU: RDC700 %ldW memory, %d devices\n", mem_size, max_io);
}

void start_cpu(ist66_cu_t *cpu, int do_step) {
    if (cpu->exit) {
        cpu->running = 1;
        cpu->exit = do_step;
        pthread_create(&cpu->thread, NULL, run, cpu);
        if (do_step) {
            pthread_join(cpu->thread, NULL);
        }
    } else if (!(cpu->running)) {
        cpu->running = 1;
        pthread_cond_signal(&cpu->intr_cond);
    }
}

void stop_cpu(ist66_cu_t *cpu) {
    if (!(cpu->exit)) {
        cpu->running = 1;
        cpu->exit = 1;
        pthread_cond_signal(&cpu->intr_cond);
        pthread_join(cpu->thread, NULL);
        cpu->running = 0;
    }
}

void wait_for_cpu(ist66_cu_t *cpu) {
    if (!(cpu->exit)) {
        pthread_join(cpu->thread, NULL);
        cpu->running = 0;
    }
}

void destroy_cpu(ist66_cu_t *cpu) {
    stop_cpu(cpu);
    
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
    
    // fprintf(stderr, "CPU: deinitialized\n");
}

int main(int argc, char *argv[]) {
    ist66_cu_t cpu;
    
    
    
    init_cpu(&cpu, 32768, 512);

    // init_panel(&cpu, 0);
    // init_bishop(&cpu, 32);


    // init_ppt_ex(&cpu, 012, 9, "monitor.ppt");
    // init_lpt(&cpu, 013, 8, stdout);
    // init_pch(&cpu, 014, 6);
    init_tty(&cpu, 060, 10, 8080);
    
    // char cmd[512];
    // int running = 1;
    // uint64_t ptr = 0;
    
    // start_render(&(cpu.render_ctx));
        
    cpu.memory[20] = 0000000002014;
    cpu.memory[21] = 0000000000000;
    cpu.memory[1024] = 0043142000010;
    cpu.memory[1025] = 0043202776075;
    cpu.memory[1026] = 0052003000000;
    cpu.memory[1027] = 0053004000000;
    cpu.memory[1028] = 0052003000001;
    cpu.memory[1029] = 0053004000001;
    cpu.memory[1030] = 0074042000004;
    cpu.memory[1031] = 0010042000004;
    cpu.memory[1032] = 0000000002653;
    cpu.memory[1033] = 0740000000000;
    cpu.memory[1034] = 0036000000000;
    cpu.memory[1035] = 0000000002000;
    cpu.memory[1036] = 0640000570060;
    cpu.memory[1037] = 0053002000006;
    cpu.memory[1038] = 0740032020001;
    cpu.memory[1039] = 0053002000003;
    cpu.memory[1040] = 0052002000003;
    cpu.memory[1041] = 0010000000000;
    cpu.memory[1044] = 0000000000002;
    cpu.memory[1045] = 0064240000000;
    cpu.memory[1046] = 0000000000136;
    cpu.memory[1047] = 0466405533540;
    cpu.memory[1048] = 0301012642644;
    cpu.memory[1049] = 0516231747100;
    cpu.memory[1050] = 0305012242630;
    cpu.memory[1051] = 0426032342500;
    cpu.memory[1052] = 0300321224206;
    cpu.memory[1053] = 0245006230144;
    cpu.memory[1054] = 0331016666730;
    cpu.memory[1055] = 0647356575156;
    cpu.memory[1056] = 0305625620202;
    cpu.memory[1057] = 0663304071322;
    cpu.memory[1058] = 0637216471500;
    cpu.memory[1059] = 0713136362744;
    cpu.memory[1060] = 0733134427100;
    cpu.memory[1061] = 0423364067336;
    cpu.memory[1062] = 0721016262710;
    cpu.memory[1063] = 0647476471322;
    cpu.memory[1064] = 0613536462502;
    cpu.memory[1065] = 0064241505000;
    cpu.memory[1066] = 0000000000021;
    cpu.memory[1067] = 0447355172100;
    cpu.memory[1068] = 0512330442650;
    cpu.memory[1069] = 0426072427134;
    cpu.memory[1070] = 0271000000000;
    cpu.memory[1071] = 0000000000015;
    cpu.memory[1072] = 0447355172100;
    cpu.memory[1073] = 0512130146240;
    cpu.memory[1074] = 0476371400000;
    cpu.memory[1075] = 0000000000020;
    cpu.memory[1076] = 0447355172100;
    cpu.memory[1077] = 0516130746612;
    cpu.memory[1078] = 0472512327134;
    cpu.memory[1079] = 0270000000000;
    cpu.memory[1080] = 0000000000020;
    cpu.memory[1081] = 0447355172100;
    cpu.memory[1082] = 0426610342640;
    cpu.memory[1083] = 0522210427134;
    cpu.memory[1084] = 0270000000000;
    cpu.memory[1085] = 0000000000027;
    cpu.memory[1086] = 0447355172100;
    cpu.memory[1087] = 0526470551232;
    cpu.memory[1088] = 0476110520120;
    cpu.memory[1089] = 0723136372122;
    cpu.memory[1090] = 0271345600000;
    cpu.memory[1091] = 0000000000001;
    cpu.memory[1092] = 0270000000000;
    cpu.memory[1093] = 0000000000001;
    cpu.memory[1094] = 0204000000000;
    cpu.memory[1095] = 0000000000010;
    cpu.memory[1096] = 0202115767312;
    cpu.memory[1097] = 0270321200000;
    cpu.memory[1098] = 0000000000022;
    cpu.memory[1099] = 0535012242602;
    cpu.memory[1100] = 0461011542632;
    cpu.memory[1101] = 0476453120236;
    cpu.memory[1102] = 0454321200000;
    cpu.memory[1103] = 0000000000136;
    cpu.memory[1104] = 0064244020100;
    cpu.memory[1105] = 0201004020100;
    cpu.memory[1106] = 0251245220206;
    cpu.memory[1107] = 0476413151222;
    cpu.memory[1108] = 0436212420254;
    cpu.memory[1109] = 0446371440650;
    cpu.memory[1110] = 0446371620210;
    cpu.memory[1111] = 0426510541650;
    cpu.memory[1112] = 0426104025124;
    cpu.memory[1113] = 0250321220100;
    cpu.memory[1114] = 0201004020100;
    cpu.memory[1115] = 0201245225100;
    cpu.memory[1116] = 0202472440644;
    cpu.memory[1117] = 0522532020256;
    cpu.memory[1118] = 0446311420234;
    cpu.memory[1119] = 0476504041636;
    cpu.memory[1120] = 0472511147252;
    cpu.memory[1121] = 0425024020124;
    cpu.memory[1122] = 0251241505000;
    cpu.memory[1123] = 0000000000666;
    cpu.memory[1124] = 0000000000000;
    cpu.memory[1125] = 0401000000000;
    cpu.memory[1126] = 0001600777777;
    cpu.memory[1127] = 0001600001777;
    cpu.memory[1128] = 0003500001777;
    cpu.memory[1129] = 0003400001777;
    cpu.memory[1130] = 0003500000000;
    cpu.memory[1131] = 0377000000000;
    cpu.memory[1132] = 0400000000374;
    cpu.memory[1133] = 0355510514236;
    cpu.memory[1134] = 0153466602421;
    cpu.memory[1135] = 0000000000002;
    cpu.memory[1136] = 0455740000000;
    cpu.memory[1137] = 0000000000007;
    cpu.memory[1138] = 0352410151646;
    cpu.memory[1139] = 0064240000000;
    cpu.memory[1140] = 0000000000007;
    cpu.memory[1141] = 0352150144630;
    cpu.memory[1142] = 0064240000000;
    cpu.memory[1143] = 0000002001000;
    cpu.memory[1144] = 0053642775760;
    cpu.memory[1145] = 0000202775710;
    cpu.memory[1146] = 0000002000006;
    cpu.memory[1147] = 0000102775706;
    cpu.memory[1148] = 0053142775755;
    cpu.memory[1149] = 0743172020023;
    cpu.memory[1150] = 0052643001006;
    cpu.memory[1151] = 0052142775752;
    cpu.memory[1152] = 0715670077776;
    cpu.memory[1153] = 0000702000001;
    cpu.memory[1154] = 0000000177777;
    cpu.memory[1155] = 0052002775745;
    cpu.memory[1156] = 0053015000017;
    cpu.memory[1157] = 0043002000027;
    cpu.memory[1158] = 0053015000000;
    cpu.memory[1159] = 0740030000000;
    cpu.memory[1160] = 0053015000001;
    cpu.memory[1161] = 0075042775737;
    cpu.memory[1162] = 0052002775736;
    cpu.memory[1163] = 0700151004010;
    cpu.memory[1164] = 0703170000000;
    cpu.memory[1165] = 0052103000040;
    cpu.memory[1166] = 0053115000022;
    cpu.memory[1167] = 0052103000041;
    cpu.memory[1168] = 0053115000023;
    cpu.memory[1169] = 0052002775727;
    cpu.memory[1170] = 0700151004014;
    cpu.memory[1171] = 0740032020001;
    cpu.memory[1172] = 0703170040400;
    cpu.memory[1173] = 0056202775662;
    cpu.memory[1174] = 0052244000000;
    cpu.memory[1175] = 0705250500000;
    cpu.memory[1176] = 0000740000000;
    cpu.memory[1177] = 0074044000001;
    cpu.memory[1178] = 0740030000000;
    cpu.memory[1179] = 0000005000000;
    cpu.memory[1180] = 0074042777710;
    cpu.memory[1181] = 0043100177777;
    cpu.memory[1182] = 0053117000001;
    cpu.memory[1183] = 0043102000011;
    cpu.memory[1184] = 0700010400000;
    cpu.memory[1185] = 0043102000012;
    cpu.memory[1186] = 0053117000001;
    cpu.memory[1187] = 0052015000022;
    cpu.memory[1188] = 0053002775574;
    cpu.memory[1189] = 0052015000023;
    cpu.memory[1190] = 0053002775573;
    cpu.memory[1191] = 0000740000000;
    cpu.memory[1192] = 0000142775631;
    cpu.memory[1193] = 0000002000001;
    cpu.memory[1194] = 0010000000000;
    cpu.memory[1195] = 0000142775626;
    cpu.memory[1196] = 0000002000001;
    cpu.memory[1197] = 0010000000001;
    cpu.memory[1198] = 0000000001000;
    cpu.memory[1199] = 0715310000000;
    cpu.memory[1200] = 0703650000000;
    cpu.memory[1201] = 0053256000001;
    cpu.memory[1202] = 0704230477777;
    cpu.memory[1203] = 0000002777776;
    cpu.memory[1204] = 0706650000000;
    cpu.memory[1205] = 0000740000000;
    cpu.memory[1206] = 0000000000000;
    cpu.memory[1207] = 0062202777656;
    cpu.memory[1208] = 0053217000001;
    cpu.memory[1209] = 0062142777654;
    cpu.memory[1210] = 0053157000001;
    cpu.memory[1211] = 0052035000000;
    cpu.memory[1212] = 0053035000001;
    cpu.memory[1213] = 0705270477777;
    cpu.memory[1214] = 0000002777775;
    cpu.memory[1215] = 0000740000002;
    cpu.memory[1216] = 0000000000000;
    cpu.memory[1217] = 0052004000000;
    cpu.memory[1218] = 0055003000000;
    cpu.memory[1219] = 0720011404301;
    cpu.memory[1220] = 0000002000020;
    cpu.memory[1221] = 0700010500000;
    cpu.memory[1222] = 0000002000005;
    cpu.memory[1223] = 0700004000000;
    cpu.memory[1224] = 0104244000007;
    cpu.memory[1225] = 0700014400000;
    cpu.memory[1226] = 0000002777776;
    cpu.memory[1227] = 0051003000000;
    cpu.memory[1228] = 0700010500000;
    cpu.memory[1229] = 0000740000000;
    cpu.memory[1230] = 0103243000007;
    cpu.memory[1231] = 0104244000007;
    cpu.memory[1232] = 0700014400000;
    cpu.memory[1233] = 0000002777775;
    cpu.memory[1234] = 0740030000000;
    cpu.memory[1235] = 0000740000000;
    cpu.memory[1236] = 0043000777777;
    cpu.memory[1237] = 0000740000000;
    cpu.memory[1238] = 0000000060000;
    cpu.memory[1239] = 0053257000001;
    cpu.memory[1240] = 0715250000000;
    cpu.memory[1241] = 0740030000000;
    cpu.memory[1242] = 0053003000000;
    cpu.memory[1243] = 0704010000000;
    cpu.memory[1244] = 0001345000000;
    cpu.memory[1245] = 0053117000001;
    cpu.memory[1246] = 0000103000000;
    cpu.memory[1247] = 0701010400000;
    cpu.memory[1248] = 0000002777774;
    cpu.memory[1249] = 0703250000000;
    cpu.memory[1250] = 0043040000072;
    cpu.memory[1251] = 0052216000001;
    cpu.memory[1252] = 0704230060060;
    cpu.memory[1253] = 0724065700000;
    cpu.memory[1254] = 0704230060007;
    cpu.memory[1255] = 0104203000007;
    cpu.memory[1256] = 0045005000000;
    cpu.memory[1257] = 0000002777772;
    cpu.memory[1258] = 0715654000000;
    cpu.memory[1259] = 0000740000000;
    cpu.memory[1260] = 0000000000600;
    cpu.memory[1261] = 0703350000000;
    cpu.memory[1262] = 0740030000000;
    cpu.memory[1263] = 0053007000000;
    cpu.memory[1264] = 0707350500000;
    cpu.memory[1265] = 0000740000000;
    cpu.memory[1266] = 0704404000000;
    cpu.memory[1267] = 0053357000001;
    cpu.memory[1268] = 0000002000004;
    cpu.memory[1269] = 0043142000045;
    cpu.memory[1270] = 0043202777434;
    cpu.memory[1271] = 0000702000030;
    cpu.memory[1272] = 0640000040060;
    cpu.memory[1273] = 0720011503400;
    cpu.memory[1274] = 0000002777773;
    cpu.memory[1275] = 0000135000000;
    cpu.memory[1276] = 0640000000060;
    cpu.memory[1277] = 0104007000007;
    cpu.memory[1278] = 0710414400000;
    cpu.memory[1279] = 0720024560012;
    cpu.memory[1280] = 0000002000002;
    cpu.memory[1281] = 0000002777767;
    cpu.memory[1282] = 0052036000001;
    cpu.memory[1283] = 0000740000000;
    cpu.memory[1284] = 0000000000000;
    cpu.memory[1285] = 0051203000000;
    cpu.memory[1286] = 0704210500000;
    cpu.memory[1287] = 0000740000000;
    cpu.memory[1288] = 0640000360060;
    cpu.memory[1289] = 0000002777777;
    cpu.memory[1290] = 0103003000007;
    cpu.memory[1291] = 0640000210060;
    cpu.memory[1292] = 0704214400000;
    cpu.memory[1293] = 0000002777773;
    cpu.memory[1294] = 0000740000000;
    cpu.memory[1295] = 0000000000010;
    cpu.memory[1296] = 0010217000001;
    cpu.memory[1297] = 0010102000010;
    cpu.memory[1298] = 0000043000000;
    cpu.memory[1299] = 0700010400000;
    cpu.memory[1300] = 0000002000003;
    cpu.memory[1301] = 0010155000000;
    cpu.memory[1302] = 0000002777773;
    cpu.memory[1303] = 0010116000001;
    cpu.memory[1304] = 0000740000000;
    cpu.memory[1305] = 0000000000000;
    cpu.memory[1306] = 0052004000000;
    cpu.memory[1307] = 0700010500000;
    cpu.memory[1308] = 0000002000005;
    cpu.memory[1309] = 0053117000001;
    cpu.memory[1310] = 0742130000000;
    cpu.memory[1311] = 0053104000000;
    cpu.memory[1312] = 0052116000001;
    cpu.memory[1313] = 0000014000000;
    cpu.memory[1314] = 0000243000000;
    cpu.memory[1315] = 0000002000010;
    cpu.memory[1316] = 0052243000000;
    cpu.memory[1317] = 0053203000000;
    cpu.memory[1318] = 0053244000000;
    cpu.memory[1319] = 0053205000001;
    cpu.memory[1320] = 0740030000000;
    cpu.memory[1321] = 0053004000001;
    cpu.memory[1322] = 0000014000000;
    cpu.memory[1323] = 0740030000000;
    cpu.memory[1324] = 0053004000000;
    cpu.memory[1325] = 0053004000001;
    cpu.memory[1326] = 0053203000000;
    cpu.memory[1327] = 0053203000001;
    cpu.memory[1328] = 0000014000000;
    cpu.memory[1329] = 0052003000000;
    cpu.memory[1330] = 0700210500000;
    cpu.memory[1331] = 0000014000000;
    cpu.memory[1332] = 0000002000004;
    cpu.memory[1333] = 0052003000001;
    cpu.memory[1334] = 0700210500000;
    cpu.memory[1335] = 0000014000000;
    cpu.memory[1336] = 0052244000000;
    cpu.memory[1337] = 0052304000001;
    cpu.memory[1338] = 0705250400000;
    cpu.memory[1339] = 0053305000001;
    cpu.memory[1340] = 0706310400000;
    cpu.memory[1341] = 0053246000000;
    cpu.memory[1342] = 0052103000000;
    cpu.memory[1343] = 0722224500000;
    cpu.memory[1344] = 0053243000000;
    cpu.memory[1345] = 0052103000001;
    cpu.memory[1346] = 0722224500000;
    cpu.memory[1347] = 0053303000001;
    cpu.memory[1348] = 0000014000000;
    cpu.memory[1349] = 0000000000050;
    cpu.memory[1350] = 0703511001034;
    cpu.memory[1351] = 0044500004000;
    cpu.memory[1352] = 0052152000002;
    cpu.memory[1353] = 0723224400000;
    cpu.memory[1354] = 0000002000015;
    cpu.memory[1355] = 0075057000001;
    cpu.memory[1356] = 0074042777430;
    cpu.memory[1357] = 0712210000000;
    cpu.memory[1358] = 0703150400000;
    cpu.memory[1359] = 0000042777751;
    cpu.memory[1360] = 0043142775362;
    cpu.memory[1361] = 0712210000000;
    cpu.memory[1362] = 0053144000002;
    cpu.memory[1363] = 0000042777717;
    cpu.memory[1364] = 0074056000001;
    cpu.memory[1365] = 0740032020001;
    cpu.memory[1366] = 0000740000000;
    cpu.memory[1367] = 0740030000000;
    cpu.memory[1368] = 0000740000000;
    cpu.memory[1369] = 0000000000050;
    cpu.memory[1370] = 0703510000000;
    cpu.memory[1371] = 0043142775347;
    cpu.memory[1372] = 0075057000001;
    cpu.memory[1373] = 0074042777407;
    cpu.memory[1374] = 0000042777727;
    cpu.memory[1375] = 0700010500000;
    cpu.memory[1376] = 0000002000012;
    cpu.memory[1377] = 0700210000000;
    cpu.memory[1378] = 0712250000000;
    cpu.memory[1379] = 0053504000002;
    cpu.memory[1380] = 0700510000000;
    cpu.memory[1381] = 0705150400000;
    cpu.memory[1382] = 0000042777674;
    cpu.memory[1383] = 0043102001231;
    cpu.memory[1384] = 0702526011010;
    cpu.memory[1385] = 0712010000000;
    cpu.memory[1386] = 0074056000001;
    cpu.memory[1387] = 0000740000000;
    cpu.memory[1388] = 0000000000150;
    cpu.memory[1389] = 0703511001034;
    cpu.memory[1390] = 0044500004000;
    cpu.memory[1391] = 0704450000000;
    cpu.memory[1392] = 0075057000001;
    cpu.memory[1393] = 0074042777363;
    cpu.memory[1394] = 0052152000002;
    cpu.memory[1395] = 0712210000000;
    cpu.memory[1396] = 0703150400000;
    cpu.memory[1397] = 0000042777703;
    cpu.memory[1398] = 0711150000000;
    cpu.memory[1399] = 0712210000000;
    cpu.memory[1400] = 0053144000002;
    cpu.memory[1401] = 0000042777651;
    cpu.memory[1402] = 0074056000001;
    cpu.memory[1403] = 0000740000000;
    cpu.memory[1404] = 0000000000400;
    cpu.memory[1405] = 0043142775310;
    cpu.memory[1406] = 0000702777733;
    cpu.memory[1407] = 0700150500000;
    cpu.memory[1408] = 0000740000000;
    cpu.memory[1409] = 0700350000000;
    cpu.memory[1410] = 0043200002000;
    cpu.memory[1411] = 0745270000000;
    cpu.memory[1412] = 0000702777452;
    cpu.memory[1413] = 0052002777341;
    cpu.memory[1414] = 0053007000001;
    cpu.memory[1415] = 0053347000002;
    cpu.memory[1416] = 0052002777337;
    cpu.memory[1417] = 0053007000003;
    cpu.memory[1418] = 0743172020023;
    cpu.memory[1419] = 0043003002000;
    cpu.memory[1420] = 0053007001006;
    cpu.memory[1421] = 0707010000000;
    cpu.memory[1422] = 0000740000000;
    cpu.memory[1423] = 0000000000000;
    cpu.memory[1424] = 0741070000000;
    cpu.memory[1425] = 0701010000000;
    cpu.memory[1426] = 0043100000007;
    cpu.memory[1427] = 0700011220044;
    cpu.memory[1428] = 0744030000000;
    cpu.memory[1429] = 0702130477777;
    cpu.memory[1430] = 0000002777775;
    cpu.memory[1431] = 0701170040500;
    cpu.memory[1432] = 0053005000000;
    cpu.memory[1433] = 0701054000000;
    cpu.memory[1434] = 0721070477600;
    cpu.memory[1435] = 0000002777766;
    cpu.memory[1436] = 0000740000000;
    cpu.memory[1437] = 0000000000000;
    cpu.memory[1438] = 0740032004400;
    cpu.memory[1439] = 0051103000000;
    cpu.memory[1440] = 0103243000007;
    cpu.memory[1441] = 0740271003500;
    cpu.memory[1442] = 0700011000735;
    cpu.memory[1443] = 0704270000000;
    cpu.memory[1444] = 0066005000000;
    cpu.memory[1445] = 0702114400000;
    cpu.memory[1446] = 0000002777772;
    cpu.memory[1447] = 0700000000000;
    cpu.memory[1448] = 0000740000000;
    cpu.memory[1449] = 0740032020001;
    cpu.memory[1450] = 0010000000001;
    cpu.memory[1451] = 0043640004000;
    cpu.memory[1452] = 0043240004000;
    cpu.memory[1453] = 0062242777270;
    cpu.memory[1454] = 0053257000001;
    cpu.memory[1455] = 0043002777772;
    cpu.memory[1456] = 0053002775120;
    cpu.memory[1457] = 0740030000000;
    cpu.memory[1458] = 0052135000000;
    cpu.memory[1459] = 0700010500000;
    cpu.memory[1460] = 0000002777775;
    cpu.memory[1461] = 0740030000000;
    cpu.memory[1462] = 0053002775112;
    cpu.memory[1463] = 0052016000001;
    cpu.memory[1464] = 0700211001100;
    cpu.memory[1465] = 0053202775207;
    cpu.memory[1466] = 0715670077774;
    cpu.memory[1467] = 0043000000017;
    cpu.memory[1468] = 0715150000000;
    cpu.memory[1469] = 0043240000012;
    cpu.memory[1470] = 0000702777430;
    cpu.memory[1471] = 0715670060004;
    cpu.memory[1472] = 0052002775200;
    cpu.memory[1473] = 0700011001232;
    cpu.memory[1474] = 0752530000000;
    cpu.memory[1475] = 0053017000001;
    cpu.memory[1476] = 0712150000012;
    cpu.memory[1477] = 0744230000000;
    cpu.memory[1478] = 0000702777577;
    cpu.memory[1479] = 0045515000000;
    cpu.memory[1480] = 0000002777774;
    cpu.memory[1481] = 0052016000001;
    cpu.memory[1482] = 0752532020003;
    cpu.memory[1483] = 0056502775170;
    cpu.memory[1484] = 0712511001232;
    cpu.memory[1485] = 0712514000000;
    cpu.memory[1486] = 0743170000000;
    cpu.memory[1487] = 0000702777612;
    cpu.memory[1488] = 0712530477777;
    cpu.memory[1489] = 0000002777775;
    cpu.memory[1490] = 0000702777652;
    cpu.memory[1491] = 0053002775161;
    cpu.memory[1492] = 0062002777227;
    cpu.memory[1493] = 0053017000001;
    cpu.memory[1494] = 0074256000001;
    cpu.memory[1495] = 0043002777241;
    cpu.memory[1496] = 0053002775050;
    cpu.memory[1497] = 0043142775157;
    cpu.memory[1498] = 0053142775155;
    cpu.memory[1499] = 0752532020023;
    cpu.memory[1500] = 0043152001000;
    cpu.memory[1501] = 0000702777574;
    cpu.memory[1502] = 0053012000004;
    cpu.memory[1503] = 0700030061000;
    cpu.memory[1504] = 0053012000006;
    cpu.memory[1505] = 0052002777210;
    cpu.memory[1506] = 0053012000005;
    cpu.memory[1507] = 0052002777207;
    cpu.memory[1508] = 0053012000007;
    cpu.memory[1509] = 0751472020024;
    cpu.memory[1510] = 0010251000000;
    cpu.memory[1511] = 0740032020023;
    cpu.memory[1512] = 0700470040400;
    cpu.memory[1513] = 0010244000000;
    cpu.memory[1514] = 0043152001002;
    cpu.memory[1515] = 0000702777556;
    cpu.memory[1516] = 0700410000000;
    cpu.memory[1517] = 0700350000000;
    cpu.memory[1518] = 0700150000000;
    cpu.memory[1519] = 0043200002000;
    cpu.memory[1520] = 0052242777163;
    cpu.memory[1521] = 0000702777275;
    cpu.memory[1522] = 0750410060600;
    cpu.memory[1523] = 0053411000000;
    cpu.memory[1524] = 0710030061000;
    cpu.memory[1525] = 0053011000001;
    cpu.memory[1526] = 0052002777162;
    cpu.memory[1527] = 0053012000005;
    cpu.memory[1528] = 0010251000000;
    cpu.memory[1529] = 0743172020024;
    cpu.memory[1530] = 0052202777162;
    cpu.memory[1531] = 0000702777624;
    cpu.memory[1532] = 0744232020024;
    cpu.memory[1533] = 0043142777031;
    cpu.memory[1534] = 0000702777637;
    cpu.memory[1535] = 0700410000000;
    cpu.memory[1536] = 0055002777155;
    cpu.memory[1537] = 0700010400000;
    cpu.memory[1538] = 0000002000025;
    cpu.memory[1539] = 0043000001400;
    cpu.memory[1540] = 0640000030060;
    cpu.memory[1541] = 0043142777152;
    cpu.memory[1542] = 0000702777376;
    cpu.memory[1543] = 0052142777160;
    cpu.memory[1544] = 0043200000030;
    cpu.memory[1545] = 0000702777343;
    cpu.memory[1546] = 0052142777155;
    cpu.memory[1547] = 0744232020024;
    cpu.memory[1548] = 0000702777621;
    cpu.memory[1549] = 0055002777141;
    cpu.memory[1550] = 0700010400000;
    cpu.memory[1551] = 0000002000004;
    cpu.memory[1552] = 0043142777141;
    cpu.memory[1553] = 0000702777363;
    cpu.memory[1554] = 0000002777763;
    cpu.memory[1555] = 0043142777141;
    cpu.memory[1556] = 0000702777360;
    cpu.memory[1557] = 0000002777760;
    cpu.memory[1558] = 0070002000001;
    cpu.memory[1559] = 0043142777070;
    cpu.memory[1560] = 0000702777354;
    cpu.memory[1561] = 0070002000001;
    
    start_cpu(&cpu, 0);
    wait_for_cpu(&cpu);
    
    
    
    // kill_render(&(cpu.render_ctx));
    destroy_cpu(&cpu);
    
    return 0;
}

