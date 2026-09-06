#ifndef _CPU_
#define _CPU_

#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "fpu.h"
#include "sdlctx.h"

#define MASK_ADDR 0x7FFFFFFL
#define C_PSW 0
#define C_CW 1
#define C_FCW 2
#define C_PLT 3
#define C_SLT 4
#define C_SDR 5
#define C_SF 6
#define MEM_FAULT (1L << 36)
#define KEY_FAULT (1L << 37)
#define SEG_FAULT_PRESENT 0
#define SEG_FAULT_KEY (1 << 27)
#define SEG_FAULT_BOUNDS (2 << 27)
#define SEG_FAULT_RIGHTS (3 << 27)
#define SEG_FAULT_WRITE (1 << 29)
#define SEG_FAULT_PAGE (1 << 30)

typedef struct acr7k_cu acr7k_cu_t;

/* Bits of acr7k_cu.deferred - work staged by an instruction and committed
 * by the run loop after that instruction retires. Kept in one word so the
 * run loop's fast path can test all of them with a single load. */
#define DEF_EDIT  1     // xeq_inst runs in place of the next fetch
#define DEF_EDSK  2     // ...and then the following instruction is skipped
#define DEF_INC   4     // inc_data must be written back to inc_addr
#define DEF_STACK 8     // next_stack must be committed to SP

typedef uint64_t (*acr7k_io_t) (
    void * /* ctx */,
    uint64_t /* accumulator */,
    int /* ctl */,
    int /* transfer */
);

enum media_cmd {
    MEDIA_SET,
    MEDIA_UNSET,
    MEDIA_GET
};

typedef int (*acr7k_media_cmd_t) (
    void * /* ctx */,
    enum media_cmd /* command */,
    char * /* argument */
);

typedef void (*acr7k_io_dtor_t) (
    acr7k_cu_t * /* cpu */,
    int /* id */
);

typedef struct {
    uint64_t base;
    uint64_t tag;
    uint16_t key;
} seg_cache_t;

#define TLB_PRESENT 8
#define TLB_WRITE   4
#define TLB_GLOBAL  2
#define TLB_NOCACHE 1

typedef struct {
    uint64_t pg_base;   // 512W aligned
    uint16_t key;       // high 9+4=13 bits of virtual address
                        // segment selector + high 4 bits of page selector
    uint8_t rights;     // present, writable, global, nocache
} tlb_entry_t;      // indexed by low 5 bits of page selector

struct acr7k_cu {
    struct acr7k_cu *host;
    
    uint64_t a[16]; // accumulators
    uint64_t c[8];  // control registers - 0: PSW, 1: CW
    uint64_t inst;
    acr7k_float_t f[16];
    seg_cache_t seg_cache[32];
    tlb_entry_t tlb[32];
    uint64_t stop_code, cycles;
    
    uint64_t xeq_inst, inc_addr, inc_data, next_stack;
    int deferred;   // DEF_* bits, see above

    /* Nonzero when the run loop must leave its fast path before the next
     * fetch: a deferred edit, a pending interrupt, a halt or stop request,
     * or an active throttle. Set by intr_assert/intr_release/
     * intr_set_mask/halt/leave_intr/stop_cpu/start_cpu/exec_smi and
     * recomputed by cpu_event_state at the end of every slow-path pass. */
    int event;
    
    uint64_t *memory;
    uint32_t mem_size;
    
    acr7k_io_dtor_t *io_destroy;
    acr7k_io_t *io;
    acr7k_media_cmd_t *media;
    void **ioctx;
    int max_io;
    
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t intr_cond;
    int pending[16];
    int min_pending;
    uint16_t mask;
    int running, exit;

    // Throttle: cap read_mem/write_mem calls per millisecond. 0 disables it.
    // mem_accesses counts those calls; throttle_t0/throttle_n0 anchor the
    // current rate-measurement window (see cpu_throttle in cpu.c).
    int throttle;
    uint64_t mem_accesses;
    uint64_t throttle_n0;
    struct timespec throttle_t0;
    
    render_loop_ctx_t render_ctx;
};

static inline void halt(acr7k_cu_t *cpu) {
    pthread_mutex_lock(&(cpu->lock));
    uint64_t current_irql = (cpu->c[C_CW] >> 32) & 0xF;
    if (cpu->min_pending >= current_irql) {
        cpu->running = 0;
        cpu->event = 1;
    }
    pthread_mutex_unlock(&(cpu->lock));
}

static inline void do_intr(acr7k_cu_t *cpu, int irq) {
    uint64_t current_irql = (cpu->c[C_CW] >> 32) & 0xF;
    cpu->memory[32 + 2 * current_irql] = cpu->c[C_PSW];
    cpu->memory[33 + 2 * current_irql] = cpu->c[C_CW];
    cpu->c[C_CW] = (((uint64_t) irq) << 32) | (current_irql << 28);
    cpu->c[C_CW] |= cpu->memory[1 + 2 * irq] & 0x3FFFF;
    cpu->c[C_PSW] = cpu->memory[2 * irq] & 0xFF7FFFFFF;
    cpu->deferred = 0;
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

static inline void do_except(acr7k_cu_t *cpu, int exc) {
    do_intr(cpu, 0);
    cpu->c[C_CW] |= (((uint64_t) exc) & 0xF) << 24;
}

static inline void leave_intr(acr7k_cu_t *cpu) {
    cpu->event = 1;     // IRQL drops; re-check for pending interrupts
    uint64_t old_irql = (cpu->c[C_CW] >> 28) & 0xF;
    cpu->c[C_PSW] = cpu->memory[32 + 2 * old_irql];
    cpu->c[C_CW] = cpu->memory[33 + 2 * old_irql];
}

static inline uint32_t get_pc(acr7k_cu_t *cpu) {
    return cpu->c[C_PSW] & MASK_ADDR;
}

static inline void set_pc(acr7k_cu_t *cpu, uint32_t new) {
    cpu->c[C_PSW] = (cpu->c[C_PSW] & ~MASK_ADDR) | (new & MASK_ADDR);
}

static inline uint64_t make_pc(acr7k_cu_t *cpu, uint32_t new) {
    return (cpu->c[C_PSW] & ~MASK_ADDR) | (new & MASK_ADDR);
}

static inline int get_cf(acr7k_cu_t *cpu) {
    return !!(cpu->c[C_PSW] & (MASK_ADDR + 1));
}

static inline void set_cf(acr7k_cu_t *cpu, int state) {
    if (state) {
        cpu->c[C_PSW] |= (MASK_ADDR + 1);
    } else {
        cpu->c[C_PSW] &= ~(MASK_ADDR + 1);
    }
}

void intr_assert(acr7k_cu_t *cpu, int irq);

void intr_release(acr7k_cu_t *cpu, int irq);

void intr_set_mask(acr7k_cu_t *cpu, uint16_t mask);

uint64_t read_mem(acr7k_cu_t *cpu, uint8_t key, uint32_t address);

uint64_t write_mem(
    acr7k_cu_t *cpu,
    uint8_t key,
    uint32_t address,
    uint64_t data
);

void init_iocpu(
    acr7k_cu_t *cpu,
    int id,
    int irq,
    uint64_t mem_size,
    int max_io
);

void start_cpu(acr7k_cu_t *cpu, int do_step);

void stop_cpu(acr7k_cu_t *cpu);

#endif
