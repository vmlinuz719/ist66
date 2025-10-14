#ifndef _CPU_
#define _CPU_

#include <stdint.h>
#include <pthread.h>

#include "softfloat.h"

#define MASK_ADDR 0x7FFFFFFL
#define C_PSW 0
#define C_CW 1
#define C_FCW 2
#define MEM_FAULT (1L << 36)
#define KEY_FAULT (1L << 37)

typedef struct ist66_cu ist66_cu_t;

typedef uint64_t (*ist66_io_t) (
    void * /* ctx */,
    uint64_t /* accumulator */,
    int /* ctl */,
    int /* transfer */
);

typedef void (*ist66_io_dtor_t) (
    ist66_cu_t * /* cpu */,
    int /* id */
);

struct ist66_cu {
    struct ist66_cu *host;
    
    uint64_t a[16]; // accumulators
    uint64_t c[8];  // control registers - 0: PSW, 1: CW
    extFloat80_t f[16];
    uint64_t stop_code;
    
    uint64_t xeq_inst, inc_addr, inc_data;
    int do_edit, do_edsk, do_inc;
    
    uint64_t *memory;
    uint32_t mem_size;
    
    ist66_io_dtor_t *io_destroy;
    ist66_io_t *io;
    void **ioctx;
    int max_io;
    
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t intr_cond;
    int pending[16];
    int min_pending;
    uint16_t mask;
    int running, exit;
};

static inline void halt(ist66_cu_t *cpu) {
    pthread_mutex_lock(&(cpu->lock));
    uint64_t current_irql = (cpu->c[C_CW] >> 32) & 0xF;
    if (cpu->min_pending >= current_irql) {
        cpu->running = 0;
    }
    pthread_mutex_unlock(&(cpu->lock));
}

static inline void do_intr(ist66_cu_t *cpu, int irq) {
    uint64_t current_irql = (cpu->c[C_CW] >> 32) & 0xF;
    cpu->memory[32 + 2 * current_irql] = cpu->c[C_PSW];
    cpu->memory[33 + 2 * current_irql] = cpu->c[C_CW];
    cpu->c[C_CW] = (((uint64_t) irq) << 32) | (current_irql << 28);
    cpu->c[C_CW] |= cpu->memory[1 + 2 * irq] & 0x3FFFF;
    cpu->c[C_PSW] = cpu->memory[2 * irq] & 0xFF7FFFFFF;
    cpu->do_inc = 0;
    cpu->do_edit = 0;
    cpu->do_edsk = 0;
}

#define X_USER      0   // unimplemented instruction
#define X_INST      1   // illegal instruction
#define X_MEMX      2   // no such memory
#define X_DEVX      3   // no such device
#define X_PPFR      4   // problem protection fault - read/exec
#define X_PPFW      5   // problem protection fault - write
#define X_PPFS      6   // problem protection fault - system management
#define X_TIME      7   // timer
#define X_DIVZ      8   // divide by zero
#define X_NFPU      9   // no FPU
#define X_MCHK      14  // machine check
#define X_PWRF      15  // power failure

static inline void do_except(ist66_cu_t *cpu, int exc) {
    do_intr(cpu, 0);
    cpu->c[C_CW] |= (((uint64_t) exc) & 0xF) << 24;
}

static inline void leave_intr(ist66_cu_t *cpu) {
    uint64_t old_irql = (cpu->c[C_CW] >> 28) & 0xF;
    cpu->c[C_PSW] = cpu->memory[32 + 2 * old_irql];
    cpu->c[C_CW] = cpu->memory[33 + 2 * old_irql];
}

static inline uint32_t get_pc(ist66_cu_t *cpu) {
    return cpu->c[C_PSW] & MASK_ADDR;
}

static inline void set_pc(ist66_cu_t *cpu, uint32_t new) {
    cpu->c[C_PSW] = (cpu->c[C_PSW] & ~MASK_ADDR) | (new & MASK_ADDR);
}

static inline int get_cf(ist66_cu_t *cpu) {
    return !!(cpu->c[C_PSW] & (MASK_ADDR + 1));
}

static inline void set_cf(ist66_cu_t *cpu, int state) {
    if (state) {
        cpu->c[C_PSW] |= (MASK_ADDR + 1);
    } else {
        cpu->c[C_PSW] &= ~(MASK_ADDR + 1);
    }
}

void intr_assert(ist66_cu_t *cpu, int irq);

void intr_release(ist66_cu_t *cpu, int irq);

void intr_set_mask(ist66_cu_t *cpu, uint16_t mask);

uint64_t read_mem(ist66_cu_t *cpu, uint8_t key, uint32_t address);

uint64_t write_mem(
    ist66_cu_t *cpu,
    uint8_t key,
    uint32_t address,
    uint64_t data
);

void init_iocpu(
    ist66_cu_t *cpu,
    int id,
    int irq,
    uint64_t mem_size,
    int max_io
);

#endif
