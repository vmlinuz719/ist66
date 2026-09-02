/*
 * cpu.c - ACR 7000 central processor: instruction execution, memory
 * protection and translation, priority interrupts, and the interactive
 * monitor (main).
 *
 * Machine model:
 *  - 36-bit words, word addressed; 27-bit physical/virtual addresses.
 *  - 16 general accumulators a[0..15], named by the assembler as AC(0),
 *    MQ(1), XY(2), X0-X7(3-10), AP(11), LR(12), SP(13), R14, R15.
 *    AC/MQ/XY have fixed roles in multiply/divide (exec_md), LR is the
 *    subroutine linkage register (callr/retr), SP is the stack pointer
 *    (stacks grow downward), R14/R15 are clobbered by local traps.
 *  - 8 control registers c[0..7] (asm2 names in parens):
 *      0 C_PSW (psw0): bits 0-26 PC, bit 27 carry flag, bits 28-35
 *        storage protection key (0 = supervisor).
 *      1 C_CW  (psw1): bits 0-17 direct page base (x512 words), bits
 *        24-27 exception code, bits 28-31 previous IRQL, bits 32-35
 *        current IRQL (interrupt priority ceiling).
 *      2 C_FCW (fpc):  bit 2 FPU enable, bits 0-1 FP register bank.
 *      3 C_PLT (plt):  problem local trap table, bit 27 enable, low 27
 *        bits table base.
 *      4 C_SLT (slt):  supervisor local trap table, same layout.
 *      5 C_SDR (sdr):  segment descriptor table, high 9 bits highest
 *        valid selector, low 27 bits table base. Nonzero enables
 *        virtual memory (see read_vmem).
 *      6 C_SF  (sflt): segment fault status (see read_vmem).
 *  - 16 floating point registers f[0..15], visible four at a time
 *    (F0-F3) through the bank selected by FCW bits 0-1.
 *
 * Instructions are decoded in exec_all; the major opcode is the top nine
 * bits (first three octal digits) of the word. See exec_all for the
 * dispatch map and the exec_* functions for the individual formats.
 *
 * Mnemonics in the comments below are those of the current assembler,
 * not_vibe_code/asm2.c (tools/assembler.py and its mnemonics are
 * deprecated).
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <SDL2/SDL.h>

#include "alu.h"
#include "fpu.h"
#include "cpu.h"
#include "include/cpu.h"
#include "ppt.h"
#include "pch.h"
#include "lpt.h"
#include "tty.h"
#include "msc.h"
#include "panel.h"
#include "bishop.h"

/**
 * @brief Look up a segment descriptor through the segment cache
 *
 * With virtual memory enabled (nonzero C_SDR), the low 27 bits of SDR
 * address a segment descriptor table and its high 9 bits hold the highest
 * valid segment selector. Each descriptor is a pair of words:
 *   word 0: base - physical base address of the segment, or of its page
 *           table if the segment is paged
 *   word 1: tag  - bits 28-35 storage key, bit 27 present, bit 26
 *           writable, bit 25 sticky (entry survives seg_invalidate_all),
 *           bit 24 paged, bits 0-17 limit (highest valid offset; only
 *           checked for unpaged segments)
 * Descriptors are cached in the 32-row direct-mapped seg_cache indexed by
 * the low five selector bits; a miss walks the table in physical memory.
 * Returns NULL for selectors beyond the SDR limit, descriptors outside
 * physical memory, or descriptors without the present bit; the caller
 * reports the specific fault.
 *
 * @param cpu Emulated CPU context
 * @param selector 9-bit segment selector (virtual address bits 18-26)
 * @return Cached descriptor or NULL
 */
seg_cache_t *seg_lookup(acr7k_cu_t *cpu, int selector) {
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
        
        cpu->mem_accesses += 2;

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

/**
 * @brief Look up a page table entry through the TLB
 *
 * A paged segment (tag bit 24) uses its descriptor base as a page table
 * with one word per 512-word page. Entry format: bits 9-35 physical page
 * base (512-word aligned), bits 5-8 access rights - present, writable,
 * global, nocache (the TLB_* bits in cpu.h). Entries are cached in the
 * 32-row direct-mapped TLB indexed by the low five bits of the page
 * number; global entries survive tlb_invalidate_all (e.g. on SDR reload).
 * Returns NULL if the entry lies outside physical memory or the page is
 * not present.
 *
 * @param cpu Emulated CPU context
 * @param selector Virtual address bits 9-26 (segment + page number)
 * @param pg_table Segment descriptor whose base addresses the page table
 * @return Cached TLB entry or NULL
 */
tlb_entry_t *tlb_lookup(acr7k_cu_t *cpu, int selector, seg_cache_t *pg_table) {
    uint8_t cache_row = selector & 0x1F;
    uint16_t cache_key = selector >> 5;
    uint16_t page_select = selector & 0x1FF;
    
    if (
        (cpu->tlb[cache_row].key != cache_key) || // not cached
        (!(cpu->tlb[cache_row].rights & TLB_PRESENT)) // not present
    ) { // go fish
        uint64_t descriptor_addr = pg_table->base + page_select;
        
        if (descriptor_addr >= cpu->mem_size) return NULL;
        
        cpu->mem_accesses++;

        uint8_t rights = (cpu->memory[descriptor_addr] & 0x1E0) >> 5;
        if (!(rights & TLB_PRESENT)) return NULL; // still not present
        
        cpu->tlb[cache_row].pg_base =
            cpu->memory[descriptor_addr] & 0777777777000;
        cpu->tlb[cache_row].rights = rights;
        cpu->tlb[cache_row].key = cache_key;
    }
    
    return &(cpu->tlb[cache_row]);
}

void tlb_invalidate(acr7k_cu_t *cpu, int selector) {
    cpu->tlb[selector & 0x1F].rights = 0;
}

void tlb_invalidate_all(acr7k_cu_t *cpu) {
    for (int i = 0; i < 32; i++) {
        if (!(cpu->tlb[i].rights & TLB_GLOBAL)) {
            cpu->tlb[i].rights = 0;
        }
    }
}

void seg_invalidate(acr7k_cu_t *cpu, int selector) {
    cpu->seg_cache[selector & 0x1F].tag = 0;
    // tlb_invalidate_all(cpu);
}

void seg_invalidate_all(acr7k_cu_t *cpu) {
    for (int i = 0; i < 32; i++) {
        if (!(cpu->seg_cache[i].tag & (1 << 25))) {
            cpu->seg_cache[i].tag = 0;
        }
    }
}

/**
 * @brief Assert a priority interrupt signal
 *
 * The ACR 7000 has 14 interrupt priority levels (1-14; smaller number = higher
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
void intr_assert(acr7k_cu_t *cpu, int irq) {
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
void intr_release(acr7k_cu_t *cpu, int irq) {
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
void intr_set_mask(acr7k_cu_t *cpu, uint16_t mask) {
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
 * @brief Read a word through virtual address translation
 *
 * A 27-bit virtual address splits into segment selector (bits 18-26),
 * page number (bits 9-17) and word offset (bits 0-8). The segment's
 * storage key must match the access key unless the access key is 0
 * (supervisor) or the segment key is 0xFE/0xFF (public read / public
 * read-write). An unpaged segment adds the full 18-bit in-segment offset
 * to its base after checking it against the descriptor limit; a paged
 * segment translates the page number through its page table instead
 * (see tlb_lookup) with no limit check.
 *
 * On a translation failure C_SF (sflt) receives the failing virtual
 * address plus a fault code: bits 27-28 = 0 not present, 1 key mismatch,
 * 2 limit exceeded, 3 access rights; bit 29 set for writes; bit 30 set
 * when the fault came from the page level rather than the segment level.
 * The function then returns KEY_FAULT so the caller raises a protection
 * fault exception. MEM_FAULT is returned only when the translated
 * physical address falls outside real memory.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to test (usually PSW bits 28-35)
 * @param vaddress Virtual address
 * @return Contents of memory, MEM_FAULT or KEY_FAULT
 */
uint64_t read_vmem(acr7k_cu_t *cpu, uint8_t key, uint32_t vaddress) {
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
 * If virtual memory is enabled (nonzero C_SDR) the access is translated
 * by read_vmem instead; the rules below apply to real mode only.
 *
 * The ACR 7000 assigns to each 512-word page of memory an 8-bit storage
 * protection key. This key may be 0x00 (supervisor only), 0x01-0xFD
 * (protected), 0xFE (readable to all) or 0xFF (readable/writable to all).
 * To read memory, a 27-bit address is first bounds-checked against the
 * amount of available memory; if this check fails, this function returns a
 * MEM_FAULT error value. Then the access key (usually the current key from
 * PSW bits 28-35) is checked against the target page's key. A read will
 * succeed and return a 36-bit word if either the provided key is equal to
 * 0, the key matches the one in memory or the key in memory is 0xFE or
 * 0xFF; otherwise this function returns a KEY_FAULT error value.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to test
 * @param address Memory address
 * @return Contents of memory, MEM_FAULT or KEY_FAULT
 */
uint64_t read_mem(acr7k_cu_t *cpu, uint8_t key, uint32_t address) {
    cpu->mem_accesses++;
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

/**
 * @brief Write a word through virtual address translation
 *
 * Write counterpart of read_vmem (see there for the address split and
 * fault reporting). Stores additionally require the segment's writable
 * bit, and the TLB_WRITE right on the page for paged segments; segment
 * key 0xFE (public read) does not authorize stores, only 0xFF does.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to test (usually PSW bits 28-35)
 * @param vaddress Virtual address
 * @param data 36-bit word to write
 * @return Zero, MEM_FAULT or KEY_FAULT
 */
uint64_t write_vmem(
    acr7k_cu_t *cpu,
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
 * If virtual memory is enabled (nonzero C_SDR) the access is translated by
 * write_vmem instead; the rules below apply to real mode only.
 *
 * To write memory, a 27-bit address is first bounds-checked against the
 * amount of available memory; if this check fails, this function returns a
 * MEM_FAULT error value. Then the access key (usually the current key from
 * PSW bits 28-35) is checked against the target page's key. A write will
 * succeed if either the provided key is equal to 0, the key matches the one
 * in memory or the key in memory is 0xFF (public read/write); otherwise
 * this function returns a KEY_FAULT error value.
 *
 * @param cpu Emulated CPU context
 * @param key Storage key to test
 * @param address Memory address
 * @param data 36-bit word to write
 * @return Zero, MEM_FAULT or KEY_FAULT
 */
uint64_t write_mem(
    acr7k_cu_t *cpu,
    uint8_t key,
    uint32_t address,
    uint64_t data
) {
    cpu->mem_accesses++;
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
uint64_t set_key(acr7k_cu_t *cpu, uint8_t key, uint32_t address) {
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
 * ACR 7000 memory reference instructions share a format similar to the DEC
 * PDP-10: one indirect bit (22), one four-bit index selector (bits 18-21)
 * and one 18-bit signed displacement (bits 0-17). Unlike the PDP-10,
 * several index selector values have special significance (asm2 address
 * syntax in parens):
 *    - 0: no index, absolute address (disp)
 *    - 1: offset from the direct page base, CW bits 0-17 x 512 (_disp)
 *    - 2: PC-relative (.disp)
 *    - 3-13: index register X0-X7, AP, LR or SP (disp(xn))
 *    - 14: address is SP, then SP += disp afterwards (+disp) - the "pop"
 *          direction; pop/popim/popcr are aliases using +1
 *    - 15: SP -= disp first, address is the new SP (=disp) - the "push"
 *          direction; push/pushim/pushcr are aliases using =1
 * The SP update of modes 14/15 is staged in next_stack/do_stack and only
 * committed once the instruction completes, so a faulting instruction can
 * be retried.
 *
 * The computed address is a 36-bit word; as of now only the low 27 bits
 * are used for addressing even though all 36 bits are generated by this
 * operation.
 *
 * If the indirect bit is set (@ in asm2), the address word must first be
 * fetched from memory, and if that fetch fails, MEM_FAULT or KEY_FAULT is
 * the result. If the most significant bit (35) of the fetched word is
 * clear, its low 27 bits are the final address. If it is set, bits 33-34
 * select an auto-modify mode using the signed 6-bit increment in bits
 * 27-32 (so, by leading octal digits of the pointer word):
 *    - 4ii: post-increment - use the address, then add increment ii
 *    - 5ii: pre-decrement - subtract increment ii, then use the address
 *    - 6xx, 7xx: reserved (MEM_FAULT)
 *
 * The modified pointer is written back to memory (via do_inc/inc_addr/
 * inc_data) after the successful completion of the instruction that
 * computed it. Should the instruction fail, all internal state pertaining
 * to this operation is cleared so it may be retried.
 *
 * @param cpu Emulated CPU context
 * @param inst Instruction
 * @return Address, MEM_FAULT or KEY_FAULT
 */
uint64_t comp_mr(acr7k_cu_t *cpu, uint64_t inst) {
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

/**
 * @brief Execute a jump, memory test/skip or stack call instruction
 *
 * Major opcode 000; the accumulator field (bits 23-26) selects the
 * operation:
 *   0  jmp    - jump to EA (nop = jmp .+1, retr = jmp 0(lr))
 *   1  callr  - jump to EA leaving the return address in LR
 *   2  inctnz - increment memory word, skip next if the result is zero
 *   3  dectnz - decrement memory word, skip next if the result is zero
 *   4  tstmnz - skip next if the memory word is zero
 *   5  tstmz  - skip next if the memory word is nonzero
 *   14 calls  - call with save mask: the word at EA (asm2 "save"
 *               directive; bit i = register 15-i) selects registers to
 *               push, followed by the mask itself and the return address,
 *               leaving SP at the return address; execution continues at
 *               EA+1
 *   15 rets/retsd - return from calls: SP += EA first (retsd n discards
 *               n words of locals), pop the return address and mask, then
 *               the saved registers; if SP itself was restored from the
 *               frame, the popped value wins
 * Other values raise the unimplemented instruction exception.
 *
 * (The tst/inc/dec skip mnemonics follow the asm2 convention of naming
 * the condition under which the FOLLOWING instruction executes, e.g.
 * "tstmnz x / jmp .y" jumps while the word at x is nonzero.)
 */
void exec_mr(acr7k_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    
    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }
    
    switch ((inst >> 23) & 0xF) {
        case 0: { // jmp
            set_pc(cpu, ea);
        } break;
        case 1: { // callr - return address in LR
            cpu->a[12] = (get_pc(cpu) + 1) & MASK_ADDR;
            set_pc(cpu, ea);
        } break;
        case 2: { // inctnz - increment memory, skip next if zero
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
        case 3: { // dectnz - decrement memory, skip next if zero
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
        case 4: { // tstmnz - skip next if memory word is zero
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
        case 5: { // tstmz - skip next if memory word is nonzero
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
        case 14: { // calls - call with register save mask at EA
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
        case 15: { // rets/retsd - return from calls, SP += EA first
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
            // unimplemented
            do_except(cpu, X_USER);
        }
    }
}

/**
 * @brief Execute a fixed point multiply/divide instruction
 *
 * Major opcode 001; the accumulator field (bits 23-26) selects the
 * operation. These use the fixed register roles AC (a0), MQ (a1) and XY
 * (a2); products are 72 bits with the high half in XY and the low half
 * in AC:
 *   0 mul    - XY:AC = MQ * mem (signed)
 *   1 fmadd  - XY:AC += MQ * mem (multiply-accumulate; a carry out of XY
 *              inverts the carry flag, Nova style)
 *   2 fmsub  - XY:AC += MQ * -mem
 *   3 div    - MQ = AC / mem, XY = AC % mem (signed, 36-bit dividend);
 *              a zero divisor raises X_DIVZ
 *   4 umul, 5 ufmadd, 6 ufmsub, 7 udiv - unsigned variants
 * Other values raise the unimplemented instruction exception.
 */
void exec_md(acr7k_cu_t *cpu, uint64_t inst) {
    uint64_t ea = comp_mr(cpu, inst);
    
    if (ea == MEM_FAULT) {
        do_except(cpu, X_MEMX);
        return;
    } else if (ea == KEY_FAULT) {
        do_except(cpu, X_PPFR);
        return;
    }
    
    switch ((inst >> 23) & 0xF) {
        case 0: { // mul - XY:AC = MQ * mem, signed
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
        case 1: { // fmadd - XY:AC += MQ * mem, signed
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
        case 2: { // fmsub - XY:AC += MQ * -mem, signed
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
        case 3: { // div - MQ = AC / mem, XY = AC % mem, signed
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
        case 4: { // umul - XY:AC = MQ * mem, unsigned
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
        case 5: { // ufmadd - XY:AC += MQ * mem, unsigned
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
        case 6: { // ufmsub - XY:AC += MQ * -mem, unsigned
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
        case 7: { // udiv - MQ = AC / mem, XY = AC % mem, unsigned
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
            // unimplemented
            do_except(cpu, X_USER);
        }
    }
}

/**
 * @brief Execute an accumulator-memory instruction
 *
 * Major opcodes 041-066, any accumulator in bits 23-26, standard address
 * field (comp_mr):
 *   041 edit   - execute the word (mem | AC) as an instruction; it runs
 *                in place of the next fetch (see do_edit in run)
 *   042 edits  - as edit, but also skip the instruction following the
 *                edits after the target has executed
 *   043 ldea   - AC = effective address
 *   044 addea  - AC += effective address
 *   045 inctne - AC += 1, then skip next if AC == mem
 *   046 dectne - AC -= 1, then skip next if AC == mem
 *                (i.e. the following instruction runs until the count
 *                reaches the memory word - "next if not equal")
 *   047 ldeas  - AC = EA << 17
 *   050 ldcom  - AC = ~mem
 *   051 ldneg  - AC = -mem
 *   052 ld     - AC = mem (pop = ld ac, +1)
 *   053 st     - mem = AC (push = st ac, =1)
 *   054 addcom - AC = ~mem + AC
 *   055 sub    - AC = AC - mem
 *   056 add    - AC = AC + mem
 *   057 and    - AC &= mem
 *   062 or     - AC |= mem
 *   066 xor    - AC ^= mem
 * The arithmetic forms invert the carry flag on carry/borrow out (Nova
 * style; the carry is never added into the sum). Unassigned opcodes in
 * the range raise the illegal instruction exception.
 */
void exec_am(acr7k_cu_t *cpu, uint64_t inst) {
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
        case 041: { // edit - execute (mem | AC) as an instruction
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
        case 042: { // edits - edit, then skip the following instruction
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
        case 043: { // ldea - AC = effective address
            cpu->a[ac] = ea;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 044: { // addea - AC += effective address
            uint64_t result = compute(
                ea, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0, 0, 0
            );
            cpu->a[ac] = result & MASK_36;
            set_cf(cpu, (result >> 36) & 1);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 045: { // inctne - AC += 1, skip next if AC == mem
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
        case 046: { // dectne - AC -= 1, skip next if AC == mem
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
        case 047: { // ldeas - AC = EA << 17
            cpu->a[ac] = (ea << 17) & MASK_36;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 050: { // ldcom - AC = ~mem
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
        case 051: { // ldneg - AC = -mem
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
        case 052: { // ld - AC = mem (pop = ld ac, +1)
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
        case 053: { // st - mem = AC (push = st ac, =1)
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
        case 054: { // addcom - AC = ~mem + AC
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
        case 055: { // sub - AC = AC - mem
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
        case 056: { // add - AC = AC + mem
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
        case 057: { // and - AC &= mem
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
        case 062: { // or - AC |= mem
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
        case 066: { // xor - AC ^= mem
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

/**
 * @brief Execute a floating point memory instruction
 *
 * Major opcodes 0400-0417. Raises X_NFPU unless FCW bit 2 (FPU enable)
 * is set. The two-bit register field in bits 23-24 selects F0-F3 within
 * the bank chosen by FCW bits 0-1 (4 banks x 4 = 16 registers). Bit 26
 * ("n" mnemonic suffix) normalizes the result and bit 25 ("r" suffix)
 * rounds it to the storage precision. Arithmetic status flags (F_OVRF /
 * F_UNDF / F_INSG / F_ILGL, see fpu.h) are ORed into XY (a2).
 *
 * Single precision (one 36-bit word at EA):
 *   0400 ldf, 0401 stf, 0402 adf, 0403 sbf, 0404 mlf, 0405 dvf
 * Double precision (two words at EA, EA+1):
 *   0406 ldg, 0407 stg, 0410 adg, 0411 sbg, 0412 mlg, 0413 dvg
 * Component access (no n/r suffixes):
 *   0414 ldexp - set the register's exponent from a signed 36-bit word
 *                (unbiased; out of range sets F_OVRF/F_UNDF)
 *   0415 stexp - store the (biased minus 16383) exponent
 *   0416 ldsig - set sign and significand from a 72-bit two's complement
 *                doubleword at EA, EA+1
 *   0417 stsig - store sign and significand as a 72-bit two's complement
 *                doubleword
 */
void exec_fm(acr7k_cu_t *cpu, uint64_t inst) {
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
        case 0400: { // ldf - load 36-bit float
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
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0401: { // stf - store 36-bit float
            int status = 0;
            uint64_t result;
            acr7k_float_t temp = {
                .sign_exp = cpu->f[ac].sign_exp,
                .signif = cpu->f[ac].signif
            };
            if (normalize) {
                acr7k_fnorm(&temp, &temp);
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

        case 0402: { // adf - add 36-bit float
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            acr7k_float_t temp;
            set_f36(&data, &temp);
            int status = acr7k_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0403: { // sbf - subtract 36-bit float
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            acr7k_float_t temp;
            set_f36(&data, &temp);
            acr7k_fneg(&temp, &temp);
            int status = acr7k_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0404: { // mlf - multiply by 36-bit float
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            acr7k_float_t temp;
            set_f36(&data, &temp);
            int status = acr7k_fmul(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0405: { // dvf - divide by 36-bit float
            uint64_t data = read_mem(cpu, cpu->c[C_PSW] >> 28, ea);
            if (data == MEM_FAULT) {
                do_except(cpu, X_MEMX);
                return;
            } else if (data == KEY_FAULT) {
                do_except(cpu, X_PPFR);
                return;
            }
            data &= MASK_36;

            acr7k_float_t temp;
            set_f36(&data, &temp);
            // acr7k_fnorm(&temp, &temp);
            
            int status = acr7k_fdiv(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            
            if (round) {
                status |= f80_round_to_f36(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0406: { // ldg - load 72-bit float
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
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }

            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0407: { // stg - store 72-bit float
            int status = 0;
            uint64_t result, result_l;
            acr7k_float_t temp = {
                .sign_exp = cpu->f[ac].sign_exp,
                .signif = cpu->f[ac].signif
            };
            if (normalize) {
                acr7k_fnorm(&temp, &temp);
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

        case 0410: { // adg - add 72-bit float
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

            acr7k_float_t temp;
            set_f72(&data, &data_l, &temp);
            int status = acr7k_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0411: { // sbg - subtract 72-bit float
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

            acr7k_float_t temp;
            set_f72(&data, &data_l, &temp);
            acr7k_fneg(&temp, &temp);
            int status = acr7k_fadd(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0412: { // mlg - multiply by 72-bit float
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

            acr7k_float_t temp;
            set_f72(&data, &data_l, &temp);
            int status = acr7k_fmul(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0413: { // dvg - divide by 72-bit float
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

            acr7k_float_t temp;
            set_f72(&data, &data_l, &temp);
            // acr7k_fnorm(&temp, &temp);
            
            int status = acr7k_fdiv(&cpu->f[ac], &temp, &cpu->f[ac]);

            if (normalize) {
                acr7k_fnorm(&cpu->f[ac], &cpu->f[ac]);
            }
            if (round) {
                status |= f80_round_to_f72(&cpu->f[ac], &cpu->f[ac]);
            }

            cpu->a[2] |= status;
            set_pc(cpu, get_pc(cpu) + 1);
        } break;

        case 0414: { // ldexp - load exponent
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

        case 0415: { // stexp - store exponent
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

        case 0416: { // ldsig - load significand
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
        
        case 0417: { // stsig - store significand
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

/**
 * @brief Execute a floating point register-register instruction
 *
 * Major opcodes 0440-0445; FPU enable is checked as in exec_fm. Three
 * two-bit register fields select F0-F3 within the FCW bank: TGT in bits
 * 23-24, SRC in bits 20-21, DST in bits 18-19. asm2 writes
 * "op src, tgt[, dst]" and makes DST = TGT when only two are given:
 *   0440 mvl - DST = SRC
 *   0441 ngl - DST = -SRC
 *   0442 adl - DST = SRC + TGT
 *   0443 sbl - DST = SRC - TGT
 *   0444 mll - DST = SRC * TGT
 *   0445 dvl - DST = SRC / TGT
 * Flag bits (mnemonic suffixes): 26 normalize ("n"); 25 round, to 36-bit
 * precision ("f") or, with bit 14 also set, to 72-bit ("g"); 22 discard
 * the result ("k") - flags and the skip test still apply. Status flags
 * are ORed into XY (a2) as in exec_fm.
 *
 * Skip condition in bits 15-17, applied to the (possibly discarded)
 * result. The ".xx" suffix names the condition under which the FOLLOWING
 * instruction executes; the hardware skips on its complement:
 *   1 .sk always skip          2 .lz skip unless negative
 *   3 .zg skip unless >= 0     4 .rn skip if zero
 *   5 .rz skip if nonzero      6 .if skip unless infinite
 *   7 .nn skip unless NaN
 */
void exec_fr(acr7k_cu_t *cpu, uint64_t inst) {
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

    acr7k_float_t temp;
    int status = 0;

    switch ((inst >> 27) & 0x1FF) {
        case 0440: { // mvl - DST = SRC
            temp.sign_exp = cpu->f[src].sign_exp;
            temp.signif = cpu->f[src].signif;
        } break;
        
        case 0441: { // ngl - DST = -SRC
            temp.sign_exp = cpu->f[src].sign_exp;
            temp.signif = cpu->f[src].signif;
            acr7k_fneg(&temp, &temp);
        } break;
        
        case 0442: { // adl - DST = SRC + TGT
            status = acr7k_fadd(&cpu->f[src], &cpu->f[tgt], &temp);
        } break;
        
        case 0443: { // sbl - DST = SRC - TGT
            temp.sign_exp = cpu->f[tgt].sign_exp;
            temp.signif = cpu->f[tgt].signif;
            acr7k_fneg(&temp, &temp);
            status = acr7k_fadd(&cpu->f[src], &temp, &temp);
        } break;
        
        case 0444: { // mll - DST = SRC * TGT
            status = acr7k_fmul(&cpu->f[src], &cpu->f[tgt], &temp);
        } break;
        
        case 0445: { // dvl - DST = SRC / TGT
            status = acr7k_fdiv(&cpu->f[src], &cpu->f[tgt], &temp);
        } break;

        default: {
            // Illegal
            do_except(cpu, X_INST);
            return;
        }
    }
    
    if (normalize) {
        acr7k_fnorm(&temp, &temp);
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

/**
 * @brief Execute a byte instruction
 *
 * Major opcodes 0100-0104. Bytes are arbitrary-width bit fields of 1-36
 * bits, PDP-10 style. Fields: data accumulator in bits 23-26, byte
 * pointer register in bits 18-21, byte size in bits 0-5. A byte pointer
 * packs a word address in bits 0-26 and the bit position of the byte
 * (shift of its LSB from bit 0) in bits 27-35.
 *   0100 ldb    - AC = byte at the pointer
 *   0101 stb    - deposit AC into the byte at the pointer
 *   0102 incbx  - advance the pointer; result to the AC-field register
 *   0103 incldb - advance the pointer in place, then load the byte
 *   0104 incstb - advance the pointer in place, then store the byte
 * Advancing subtracts the byte size from the shift; when it runs out the
 * shift wraps to 36-size and the word address is incremented, so
 * consecutive bytes fill each word from the most significant end down
 * (the asm2 ds/dsn directives pack five 7-bit characters per word at
 * shifts 29, 22, 15, 8, 1 to match). A pointer with shift 36 points just
 * before the first byte of its word, so a pre-incrementing loop with
 * incldb/incstb starts there.
 */
void exec_bx(acr7k_cu_t *cpu, uint64_t inst) {
    uint64_t ac = (inst >> 23) & 0xF;
    uint64_t ix = (inst >> 18) & 0xF;
    uint64_t bs = inst & 0x3F;
    
    uint64_t ea = cpu->a[ix] & MASK_ADDR;
    uint64_t sh = cpu->a[ix] >> 27;
    
    switch ((inst >> 27) & 0x1FF) {
        case 0100: { // ldb - load byte
            if (sh > 36) sh = 36;

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
        case 0101: { // stb - store byte
            if (sh > 36) sh = 36;

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
        case 0102: { // incbx - advance byte pointer
            sh -= bs;
            if (sh > 36) {
                sh = (36 - bs) & 0x3F;
                ea = (ea + 1) & MASK_ADDR;
            }
            cpu->a[ac] = ea | (sh << 27);
            set_pc(cpu, get_pc(cpu) + 1);
        } break;
        case 0103: { // incldb - advance pointer, then load byte
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
        case 0104: { // incstb - advance pointer, then store byte
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

/**
 * @brief Execute a local trap (one-word call gate) instruction
 *
 * Major opcodes 0200-0377 form 128 call gates dispatched through two
 * 64-entry trap tables: opcodes 02xx use C_PLT (problem local trap) and
 * 03xx use C_SLT (supervisor local trap). Bit 27 of the table register
 * enables the group; a disabled trap raises X_USER. The low six bits of
 * the opcode select the entry: PC = table base + entry.
 *
 * PLT traps save only the return PC in R15 and keep the current key.
 * SLT traps save the entire PSW in R15 and clear the key to 0, entering
 * supervisor state (return with retsv, which restores PSW from R15).
 * Both leave the full instruction word in R14, so its unused low 27 bits
 * can carry an operand such as a service call number.
 */
void exec_local_trap(acr7k_cu_t *cpu, uint64_t inst) {
    uint64_t opcode = ((inst >> 27) & 0x1FF);
    if (opcode >= 0300) { // SLT: set key to 0 and save full PSW
        if (!((cpu->c[C_SLT] >> 27) & 1)) {
            do_except(cpu, X_USER);
            return;
        }
        cpu->a[15] = make_pc(cpu, get_pc(cpu) + 1);
        cpu->c[C_PSW] &= (1 << 27);
        set_pc(cpu, (cpu->c[C_SLT] + (opcode & 077)) & MASK_ADDR);
    } else { // PLT: preserve key and save only the program counter
        if (!((cpu->c[C_PLT] >> 27) & 1)) {
            do_except(cpu, X_USER);
            return;
        }
        cpu->a[15] = (get_pc(cpu) + 1) & MASK_ADDR;
        set_pc(cpu, (cpu->c[C_PLT] + (opcode & 077)) & MASK_ADDR);
    }
    cpu->a[14] = inst;
}

/**
 * @brief Execute a system management instruction
 *
 * Major opcodes 010 and 070-076. Privileged: any nonzero key raises
 * X_PPFS. The accumulator field (bits 23-26) is an operand register for
 * most forms and a sub-opcode for the 010 group.
 *
 *   070 wait   - stop the CPU with stop code AC, resume PC = EA
 *                (hlt = wait ac, .+1). The stop only takes effect if no
 *                deliverable interrupt is pending; see run() for how a
 *                stopped CPU sleeps or exits to the monitor.
 *   071 intr   - programmed interrupt: PC = EA is recorded as the resume
 *                point, then control enters interrupt level AC
 *   010 sub-opcodes (in the AC field):
 *     0 reti/retid - return from interrupt: restore the previous level's
 *                    PSW/CW from the save area, then add EA to the
 *                    restored PC (retid n resumes n words later)
 *     1 retlmi     - load the interrupt mask from mem[EA], then return
 *                    from interrupt
 *     2 ldmask     - load the interrupt mask (popim = ldmask +1)
 *     3 lmwait     - load the mask and wait for an interrupt; execution
 *                    resumes at the next instruction
 *     4 stmask     - store the interrupt mask (pushim = stmask =1)
 *     5 invlsg     - invalidate the cached segment descriptor for EA
 *     6 invlpg     - invalidate the TLB row for the page containing EA
 *     7 retsv      - restore the full PSW from R15 (SLT trap return;
 *                    retsvd additionally takes an address field, e.g. to
 *                    pop with +n)
 *   072 ldkey  - AC = storage key of the page containing EA
 *   073 stkey  - set the storage key of the page at EA from AC
 *   074 ldctl  - load control register (AC field) from mem[EA]; loading
 *                SDR flushes non-sticky segment cache and TLB entries
 *                (popcr = ldctl cr, +1)
 *   075 stctl  - store control register to mem[EA] (pushcr = stctl =1)
 *   076 ldtrt  - translate virtual address EA: on success the real
 *                address is loaded into the accumulator selected by the
 *                AC field and the next instruction is skipped; on
 *                failure C_SF describes the fault and no skip occurs
 */
void exec_smi(acr7k_cu_t *cpu, uint64_t inst) {
    uint64_t key = (cpu->c[C_PSW] >> 28) & 0xFF;
    if (!key) {
        uint64_t ea = comp_mr(cpu, inst);
        if (ea == MEM_FAULT) {
            do_except(cpu, X_MEMX);
            return;
        }
        
        uint64_t ac = (inst >> 23) & 0xF;
        switch ((inst >> 27) & 0x1FF) {
            case 070: { // wait (hlt = wait ac, .+1)
                halt(cpu);
                cpu->stop_code = cpu->a[ac];
                set_pc(cpu, ea);
            } break;
            case 071: { // intr - programmed interrupt to level AC
                set_pc(cpu, ea);
                do_intr(cpu, ac);
            } break;
            case 010: { // sub-opcode in the AC field
                switch (ac) {
                    case 0: { // reti/retid - return from interrupt
                        leave_intr(cpu);
                        set_pc(cpu, get_pc(cpu) + ea);
                    } break;
                    case 1: { // retlmi - load mask, return from interrupt
                        uint64_t data = read_mem(cpu, 0, ea);
                        if (data == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        data &= MASK_36;
                        
                        intr_set_mask(cpu, data);
                        leave_intr(cpu);
                    } break;
                    case 2: { // ldmask (popim = ldmask +1)
                        uint64_t data = read_mem(cpu, 0, ea);
                        if (data == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        data &= MASK_36;
                        
                        intr_set_mask(cpu, data);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 3: { // lmwait - load mask and wait for interrupt
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
                    case 4: { // stmask (pushim = stmask =1)
                        uint64_t w_res = write_mem(cpu, 0, ea, cpu->mask);
                        if (w_res == MEM_FAULT) {
                            do_except(cpu, X_MEMX);
                            return;
                        }
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 5: { // invlsg - invalidate segment cache row
                        seg_invalidate(cpu, ea >> 18);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 6: { // invlpg - invalidate TLB row
                        tlb_invalidate(cpu, (ea >> 9) & 0x1F);
                        set_pc(cpu, get_pc(cpu) + 1);
                    } break;
                    case 7: { // retsv - restore PSW from R15
                        uint64_t new_pc = (cpu->a[15] + ea) & MASK_ADDR;
                        cpu->c[C_PSW] = (cpu->a[15] & ~MASK_ADDR) | new_pc;
                    } break;
                    default: {
                        // Illegal
                        do_except(cpu, X_INST);
                    }
                }
            } break;
            case 072: { // ldkey - AC = page storage key
                ea &= ~(0x1FF);
                if (ea < cpu->mem_size) {
                    cpu->a[ac] = cpu->memory[ea] >> 36;
                } else {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 073: { // stkey - set page storage key from AC
                uint64_t w_res = set_key(cpu, cpu->a[ac], ea);
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 074: { // ldctl (popcr = ldctl cr, +1)
                uint64_t data = read_mem(cpu, 0, ea);
                if (data == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                   return;
                }
                data &= MASK_36;
            
                cpu->c[(ac & 0x7)] = data & MASK_36;
                if ((ac & 0x7) == C_SDR) {
                    seg_invalidate_all(cpu);
                    tlb_invalidate_all(cpu);
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 075: { // stctl (pushcr = stctl cr, =1)
                uint64_t w_res = write_mem(cpu, 0, ea, cpu->c[ac & 0x7]);
                if (w_res == MEM_FAULT) {
                    do_except(cpu, X_MEMX);
                    return;
                }
                set_pc(cpu, get_pc(cpu) + 1);
            } break;
            case 076: { // ldtrt - translate virtual address, skip if OK
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
                
                cpu->a[ac] = address;
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

/**
 * @brief Execute a programmed I/O instruction
 *
 * Major opcode 0640, privileged (nonzero key raises X_PPFS). Fields:
 * accumulator bits 23-26, control pulse bits 16-17, transfer code bits
 * 12-15, device id bits 0-11. The handler registered in cpu->io[device]
 * receives the accumulator value plus the ctl and transfer codes; a
 * missing device raises X_DEVX. What each transfer code addresses is up
 * to the device, but the direction is fixed: even codes below 14 store
 * the handler's result into the accumulator (device-to-CPU), odd codes
 * are CPU-to-device only.
 *
 * asm2 encodes transfer = 2 x device register + direction:
 *   rio ac, r, dev  - read device register r  (transfer 2r)
 *   wio ac, r, dev  - write device register r (transfer 2r+1)
 *   nio dev         - no transfer             (transfer 15)
 * each optionally suffixed with a control pulse: -s(tart) ctl 1,
 * -c(lear) ctl 2, -p(ulse) ctl 3 (e.g. nios, wioc).
 *
 * Transfer 14 is the status skip test: the handler returns Busy in bit 0
 * and Done in bit 1, and ctl selects the condition as commented below:
 *   tionb - skip if busy       tiobz - skip if not busy
 *   tiond - skip if done       tiodn - skip if not done
 * (so "tiond dev / jmp .-1" spins until the device is done).
 */
void exec_io1(acr7k_cu_t *cpu, uint64_t inst) {

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
 * @brief Evaluate an ALU (register-register) operation
 *
 * All words whose top three bits are 111 (leading octal digit 7) are ALU
 * operations. Common fields:
 *   bit  32     high bit of the ALU opcode
 *   bit  31     no-load ("n" suffix): update carry/skip, discard result
 *               (used by the canned cmp* mnemonics)
 *   bits 27-30  ACS, source accumulator (operand a)
 *   bits 23-26  ACD, destination accumulator (operand b)
 *   bits 20-22  low three bits of the ALU opcode
 *   bits 18-19  carry init: 0 keep, 1 zero ("z"), 2 one ("s"), 3
 *               complement ("c")
 *   bits 15-17  skip condition (below)
 *   bit  14     mode, bit 13 submode - select one of three encodings:
 *
 * mode 0, "r"/"m" mnemonic forms - rotate and mask:
 *   bit 13    rotate through carry, 37 bits ("t" suffix); otherwise
 *             rotate 36 bits leaving the carry alone
 *   bit 12    negate the mask count ("r" flag after an "m" form)
 *   bits 6-11 mask count: after rotating, fill the top mk bits (bottom
 *             -mk bits if negated) of the result with the carry value
 *   bits 0-5  left rotate count (0-63, effectively mod 36/37)
 *   asm2: "xxxr acs, acd[, rotate[, mask]]" with rotate first, or
 *         "xxxm acs, acd[, mask[, rotate]]" with mask first
 *
 * mode 1 submode 0, "s" form - shift, with alternate destination:
 *   bits 6-9  destination register, written instead of ACD (see the ADR
 *             handling in exec_all)
 *   bit 12    shift right instead of left ("r" flag)
 *   bits 0-5  shift count; the vacated bit positions are filled with the
 *             initialized carry, so "z" gives a logical shift
 *   asm2: "xxxs acs, acd, dst[, count]"
 *
 * mode 1 submode 1, "i" form - immediate:
 *   bits 0-12 signed 13-bit immediate used in place of operand b
 *   asm2: "xxxi acs, acd, imm"
 *
 * ALU opcodes (bit 32, bits 20-22 - asm2 three-letter base mnemonics):
 *   00 com (~a), 01 ngt (-a), 02 mov (a), 03 inc (a+1), 04 adc (~a+b),
 *   05 sub (b-a), 06 add (a+b), 07 and, 12 bis (a|b), 16 xor,
 *   17 pct (population count of a)
 * As on the DG Nova the carry is never added into a sum; arithmetic ops
 * merely invert the initialized carry when they carry/borrow out.
 *
 * Skip conditions (see skip() in alu.c). The asm2 ".xx" suffix names the
 * condition under which the FOLLOWING instruction executes; the hardware
 * skips on its complement:
 *   0 never (no suffix)        1 .sk always skip
 *   2 .cn skip if carry zero   3 .cz skip if carry one
 *   4 .rn skip if result zero  5 .rz skip if result nonzero
 *   6 .bn skip if result zero or carry zero
 *   7 .bz skip if result nonzero and carry one
 * The canned compares are no-load subtracts using these tests: after
 * "cmp/cmpne/cmplt/cmple/cmpgt/cmpge a, b" the next instruction executes
 * if a ==/!=/</<=/>/>= b (unsigned).
 *
 * @param inst Instruction
 * @param a Value of ACS
 * @param b Value of ACD (ignored for the immediate form)
 * @param c Current carry flag
 * @return ALU result: bits 0-35 value, bit 36 carry, bit 37 skip
 */
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

/**
 * @brief Decode and execute one instruction
 *
 * Dispatch on the major opcode, the top nine bits (first three octal
 * digits) of the instruction word:
 *   7xx        ALU register-register group     (exec_aa, handled here)
 *   000        jumps, memory test/skips, calls (exec_mr)
 *   001        fixed point multiply/divide     (exec_md)
 *   010, 070-076  system management            (exec_smi)
 *   041-066    accumulator-memory group        (exec_am)
 *   0100-0104  byte load/store                 (exec_bx)
 *   0200-0377  local traps, PLT/SLT            (exec_local_trap)
 *   0400-0417  floating point memory ops       (exec_fm)
 *   0440-0445  floating point register ops     (exec_fr)
 *   0640       programmed I/O                  (exec_io1)
 * Anything else raises the illegal instruction exception.
 *
 * For the ALU group this function also applies the result: it is written
 * to ACD unless the no-load bit 31 is set - or, for the "s" (shift/ADR)
 * encoding recognized by bits 13-14 == 10, to the alternate destination
 * register in bits 6-9. Carry comes from bit 36 of the ALU result and a
 * skip (PC += 2) is taken if the selected condition put a 1 in bit 37.
 */
void exec_all(acr7k_cu_t *cpu, uint64_t inst) {
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
 
/*
 * Pace the CPU thread to at most cpu->throttle read_mem/write_mem calls per
 * millisecond. We track how many accesses happened since the window anchor
 * (throttle_t0/throttle_n0) and how long that took; if we are running ahead of
 * the allowed rate we sleep off the surplus. Sub-quarter-millisecond surpluses
 * are left to accumulate so we sleep in usefully large (and accurate) chunks
 * rather than fighting the kernel's timer granularity on every instruction.
 */
static void cpu_throttle(acr7k_cu_t *cpu) {
    if (!cpu->throttle) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int64_t elapsed_ns =
        (int64_t) (now.tv_sec - cpu->throttle_t0.tv_sec) * 1000000000LL
        + (now.tv_nsec - cpu->throttle_t0.tv_nsec);

    uint64_t did = cpu->mem_accesses - cpu->throttle_n0;

    // Time these accesses are permitted to take at <throttle> accesses/ms,
    // i.e. <throttle> accesses per 1e6 ns.
    int64_t allowed_ns = (int64_t) (did * 1000000ULL / (uint64_t) cpu->throttle);

    int64_t surplus_ns = allowed_ns - elapsed_ns;
    if (surplus_ns > 250000) {
        struct timespec ts;
        ts.tv_sec = surplus_ns / 1000000000LL;
        ts.tv_nsec = surplus_ns % 1000000000LL;
        nanosleep(&ts, NULL);
    }

    // Re-anchor the window periodically (and after long idles) so accumulated
    // credit or debt can't run away.
    if (elapsed_ns > 100000000LL) {
        clock_gettime(CLOCK_MONOTONIC, &cpu->throttle_t0);
        cpu->throttle_n0 = cpu->mem_accesses;
    }
}

/**
 * @brief CPU thread main loop
 *
 * Each iteration executes a pending edit target if one is staged (see
 * exec_am: an edit target runs in place of the next fetch, and edits
 * then advances PC once more to skip the following instruction), takes
 * the best pending interrupt if its level beats the current IRQL (CW
 * bits 32-35), fetches and executes one instruction, and finally commits
 * any deferred indirect-pointer writeback (do_inc, see comp_mr) and SP
 * update (do_stack) - deferred so that a faulting instruction can be
 * retried after e.g. a page-in.
 *
 * Interrupt entry (do_intr in cpu.h) vectors through low memory: words
 * 2n and 2n+1 hold the new PSW and the CW direct-page bits for level n,
 * and the interrupted PSW/CW pair is stashed at words 32+2*irql and
 * 33+2*irql for leave_intr (reti) to restore. Level 0 is the exception
 * level; do_except additionally deposits the X_* code in CW bits 24-27.
 *
 * When the CPU stops (wait/hlt/lmwait): at IRQL 0, or with an all-zero
 * interrupt mask, nothing could ever wake it, so the thread exits back
 * to the monitor; otherwise it sleeps on intr_cond until a device
 * asserts an unmasked interrupt.
 */
void *run(void *vctx) {
    acr7k_cu_t *cpu = (acr7k_cu_t *) vctx;

    fprintf(stderr, "CPU: starting\n");

    do {
        cpu_throttle(cpu);
        
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
    fprintf(stderr, "CPU: halted, code %012lo after %ld instructions\n", 
        cpu->stop_code, cpu->cycles);
    cpu->cycles = 0;
    return NULL;
}

void init_cpu(acr7k_cu_t *cpu, uint64_t mem_size, int max_io) {
    memset(cpu, 0, sizeof(acr7k_cu_t));
    
    cpu->memory = calloc(sizeof(uint64_t), mem_size);
    cpu->mem_size = mem_size;
    
    cpu->io_destroy = calloc(sizeof(acr7k_io_dtor_t), max_io);
    cpu->io = calloc(sizeof(acr7k_io_t), max_io);
    cpu->media = calloc(sizeof(acr7k_media_cmd_t), max_io);
    cpu->ioctx = calloc(sizeof(void *), max_io);
    cpu->max_io = max_io;
    cpu->mask = 0xFFFF;
    cpu->min_pending = 0xFFFF;
    cpu->exit = 1;
    
    pthread_mutex_init(&cpu->lock, NULL);
    pthread_cond_init(&cpu->intr_cond, NULL);
    fprintf(stderr, "CPU: ACR 7000 %ldW memory, %d devices\n", mem_size, max_io);
}

void start_cpu(acr7k_cu_t *cpu, int do_step) {
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

void stop_cpu(acr7k_cu_t *cpu) {
    if (!(cpu->exit)) {
        cpu->running = 1;
        cpu->exit = 1;
        pthread_cond_signal(&cpu->intr_cond);
        pthread_join(cpu->thread, NULL);
        cpu->running = 0;
    }
}

void wait_for_cpu(acr7k_cu_t *cpu) {
    if (!(cpu->exit)) {
        pthread_join(cpu->thread, NULL);
        cpu->running = 0;
    }
}

void destroy_cpu(acr7k_cu_t *cpu) {
    stop_cpu(cpu);
    
    for (int i = 0; i < cpu->max_io; i++) {
        if (cpu->io_destroy[i] != NULL) {
            cpu->io_destroy[i](cpu, i);
        }
    }
    
    free(cpu->memory);
    free(cpu->io_destroy);
    free(cpu->io);
    free(cpu->media);
    free(cpu->ioctx);
    pthread_mutex_destroy(&cpu->lock);
    pthread_cond_destroy(&cpu->intr_cond);
    
    fprintf(stderr, "CPU: deinitialized\n");
}

/*
 * Interactive monitor. Commands operate on a current octal pointer:
 *   /addr   set the pointer         ?       print the pointer
 *   .n      dump n words            =v ...  deposit octal words
 *   G[W|S]  set PC to the pointer (GW: run until halt, GS: run detached)
 *   W       run until halt          S       single step
 *   P       pause the CPU           F       print the FP registers
 *   Tn      throttle to n memory accesses/ms (bare T disables)
 *   X       exit
 * The device set is hardcoded below. The words preloaded at 01000 are
 * the assembled not_vibe_code/rimldr.a700, a relocatable loader for the
 * paper tape (RIM) format emitted by asm2; monitor.ppt is mounted in the
 * tape reader for it.
 */
int main(int argc, char *argv[]) {
    acr7k_cu_t cpu;



    init_cpu(&cpu, 262144, 512);

    init_panel(&cpu, 16);
    init_bishop(&cpu, 32);


    init_ppt_ex(&cpu, 012, 9, "monitor.ppt");
    init_lpt(&cpu, 013, 8, stdout);
    init_tty(&cpu, 060, 10, 8080);
    
    init_msch(&cpu, 034, 6);
    init_7310(&cpu, 034, 014);
    
    init_msch(&cpu, 054, 6);
    
    char cmd[512];
    int running = 1;
    uint64_t ptr = 0;
    
    start_render(&(cpu.render_ctx));
    
    printf("Ready. Relocatable loader at 01000:\n");
    
    printf("    640000,370012\n");
    printf("    704212,004400\n");
    printf("    740032,120023\n");
    printf("    740032,020001\n");
    printf("    640000,560012\n");
    printf("    000002,777777\n");
    printf("    640100,200012\n");
    printf("    722130,577600\n");
    printf("    000002,777771\n");
    printf("    700011,020006\n");
    printf("    742010,300000\n");
    printf("    000002,777771\n");
    printf("    704214,500000\n");
    printf("    700650,100000\n");
    printf("    053016,000001\n");
    printf("    000002,777764\n");
        
    cpu.memory[512] = 0640000370012;
    cpu.memory[513] = 0704212004400;
    cpu.memory[514] = 0740032120023;
    cpu.memory[515] = 0740032020001;
    cpu.memory[516] = 0640000560012;
    cpu.memory[517] = 0000002777777;
    cpu.memory[518] = 0640100200012;
    cpu.memory[519] = 0722130577600;
    cpu.memory[520] = 0000002777771;
    cpu.memory[521] = 0700011020006;
    cpu.memory[522] = 0742010300000;
    cpu.memory[523] = 0000002777771;
    cpu.memory[524] = 0704214500000;
    cpu.memory[525] = 0700650100000;
    cpu.memory[526] = 0053016000001;
    cpu.memory[527] = 0000002777764;
    
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
                start_cpu(&cpu, 1);
                ptr = get_pc(&cpu);
            }

            else if (cmd[i] == 'F') {
                for (int i = 0; i < 16; i++) {
                    printf("F%02d = ", i);
                    print_rdc_float(&cpu.f[i]);
                    printf("\n");
                }
            }
            
            else if (cmd[i] == 'T') {
                char *end;
                long val = strtol(cmd + i + 1, &end, 10);
                if (end == cmd + i + 1) {
                    cpu.throttle = 0;
                    printf("Throttle disabled\n");
                } else if (val <= 0) {
                    printf("? Bad throttle\n");
                } else {
                    cpu.throttle = (int) val;
                    cpu.throttle_n0 = cpu.mem_accesses;
                    clock_gettime(CLOCK_MONOTONIC, &cpu.throttle_t0);
                    printf("Throttle set to %d accesses/ms\n", cpu.throttle);
                }
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
            
            else if (cmd[i] == 'M') {
                if (ptr >= cpu.max_io || cpu.media[ptr] == NULL) {
                    printf("? Unsupported\n");
                    continue;
                }
                
                i++;
                while (
                    (cmd[i] == ' ' || cmd[i] == '\t')
                    && i < sizeof(cmd) - 1
                ) i++;
                
                if (cmd[i] == '\0') {
                    cpu.media[ptr](cpu.ioctx[ptr], MEDIA_UNSET, NULL);
                } else {
                    cpu.media[ptr](cpu.ioctx[ptr], MEDIA_SET, &cmd[i]);
                }
                
            }
        }
    }
    
    kill_render(&(cpu.render_ctx));
    destroy_cpu(&cpu);
    
    return 0;
}

