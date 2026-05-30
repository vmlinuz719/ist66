/* Headless CPU harness for VIBASIC.
 *
 * Loads a RIM tape, wires a fake TTY (device 48 = octal 060) on IRQ 10 that
 * captures output and can inject a scripted input line, and runs the CPU with
 * faithful interrupt dispatch.  Prints captured console output.
 *
 *   ./harness tape.ppt ["input line to type"]
 */
#define main cpu_real_main
#include "../cpu.c"
#undef main

/* stubs so the (unused) renamed original main links */
void init_panel(ist66_cu_t *c, int i){(void)c;(void)i;}
void init_bishop(ist66_cu_t *c, int i){(void)c;(void)i;}
void init_ppt_ex(ist66_cu_t *c,int i,int j,char*f){(void)c;(void)i;(void)j;(void)f;}
void init_lpt(ist66_cu_t *c,int i,int j,FILE*f){(void)c;(void)i;(void)j;(void)f;}
void init_tty(ist66_cu_t *c,int i,int j,int p){(void)c;(void)i;(void)j;(void)p;}
void start_render(render_loop_ctx_t *r){(void)r;}
void kill_render(render_loop_ctx_t *r){(void)r;}

/* ---- fake TTY -------------------------------------------------------- */
#define ENABLED 1
#define INTR_RET 8
#define INTR_OUT 32

typedef struct {
    ist66_cu_t *cpu;
    int irq;
    int control;
    int done;
    int pending;          /* byte loaded by transfer=1 awaiting start */
    char out[200000]; int outlen;
    unsigned char in[4096]; int inlen, inpos;
    int delivered;        /* input interrupt already raised? */
} faketty_t;

static faketty_t TTY;

static uint64_t faketty_io(void *vctx, uint64_t data, int ctl, int transfer) {
    faketty_t *t = (faketty_t *)vctx;

    if (transfer == 1) t->pending = (int)(data & 0x7F);
    else if (transfer == 3) t->control = (int)(data >> 8);

    if (transfer != 14) {
        if (ctl == 1) {
            if (transfer == 1) {                 /* output start */
                if (t->outlen < (int)sizeof(t->out)) t->out[t->outlen++] = (char)t->pending;
            }
            if (t->done) { t->done = 0; intr_release(t->cpu, t->irq); }
            if (transfer == 1 && (t->control & INTR_OUT)) {
                t->done = 1; intr_assert(t->cpu, t->irq);   /* INTR_OUT: re-fire */
            }
        } else if (ctl == 2) {
            if (t->done) { t->done = 0; intr_release(t->cpu, t->irq); }
        }
    }

    if (transfer == 14) return (uint64_t)((t->done << 1));
    else if (transfer == 0) {                    /* pop received byte */
        if (t->inpos < t->inlen) return (uint64_t)t->in[t->inpos++];
        return (uint64_t)-1;
    }
    else if (transfer == 2 || transfer == 4)     /* fill count */
        return (uint64_t)(t->inlen - t->inpos);
    return 0;
}

/* Raise the input interrupt once, as if INTR_RET fired on a typed line. */
static void deliver_input(faketty_t *t) {
    if (t->delivered || t->inlen == 0) return;
    t->delivered = 1;
    if (!t->done) { t->done = 1; intr_assert(t->cpu, t->irq); }
}

static void load_tape(ist66_cu_t *cpu, const char *fn) {
    FILE *f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "no tape %s\n", fn); exit(1); }
    int c;
    while ((c = fgetc(f)) != EOF) {
        int b1 = fgetc(f), b2 = fgetc(f);
        if (b1 < 0 || b2 < 0) break;
        uint64_t base = ((uint64_t)(c & 077) << 12)
                      | ((uint64_t)(b1 & 077) << 6) | (uint64_t)(b2 & 077);
        int d;
        while ((d = fgetc(f)) != 128) {
            if (d == EOF) break;
            int w1=fgetc(f),w2=fgetc(f),w3=fgetc(f),w4=fgetc(f),w5=fgetc(f);
            uint64_t w = ((uint64_t)(d &077)<<30)|((uint64_t)(w1&077)<<24)
                       |((uint64_t)(w2&077)<<18)|((uint64_t)(w3&077)<<12)
                       |((uint64_t)(w4&077)<<6)|(uint64_t)(w5&077);
            if (base < cpu->mem_size) cpu->memory[base] = w;
            base++;
        }
        if (d == EOF) break;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    static ist66_cu_t cpu;
    init_cpu(&cpu, 262144, 512);
    load_tape(&cpu, argv[1]);

    TTY.cpu = &cpu; TTY.irq = 10;
    cpu.io[48] = faketty_io;
    cpu.ioctx[48] = &TTY;
    if (argc > 2) {
        const char *s = argv[2];
        int n = 0;
        while (s[n] && n < (int)sizeof(TTY.in) - 2) { TTY.in[n] = (unsigned char)s[n]; n++; }
        TTY.in[n++] = '\r'; TTY.in[n++] = '\n';   /* telnet-style CR LF */
        TTY.inlen = n;
    }

    set_pc(&cpu, 1024);
    cpu.running = 1; cpu.exit = 0;

    long max = 60000000, i;
    for (i = 0; i < max; i++) {
        uint64_t cur_irql = (cpu.c[C_CW] >> 32) & 0xF;
        if (cpu.min_pending < cur_irql) do_intr(&cpu, cpu.min_pending);

        if (cpu.running) {
            uint64_t inst = read_mem(&cpu, cpu.c[C_PSW] >> 28, get_pc(&cpu));
            if (inst == MEM_FAULT) do_except(&cpu, X_MEMX);
            else if (inst == KEY_FAULT) do_except(&cpu, X_PPFR);
            else exec_all(&cpu, inst);
            if (cpu.do_inc) {
                write_mem(&cpu, cpu.c[C_PSW] >> 28, cpu.inc_addr, cpu.inc_data);
                cpu.do_inc = 0;
            }
            if (cpu.do_stack) { cpu.a[13] = cpu.next_stack; cpu.do_stack = 0; }
        } else {
            /* idle (lmwait with nothing pending): inject input once, else stop */
            if (cpu.min_pending < cur_irql) continue;
            if (!TTY.delivered && TTY.inlen) { deliver_input(&TTY); cpu.running = 1; continue; }
            break;
        }
    }

    fwrite(TTY.out, 1, TTY.outlen, stdout);
    printf("\n---- [cycles=%ld running=%d outlen=%d] ----\n", i, cpu.running, TTY.outlen);
    return 0;
}
