# ACR 7000 emulator — performance review

Findings from a profiling pass over the interpreter core, September 2026.
Everything below was measured, not guessed; the "rejected" section exists so
nobody spends a second afternoon on the ideas that turned out not to pay.

## Method

A headless harness ran the CPU thread (`run()` in `cpu.c`) directly on a
160M-instruction integer workload: loads, stores, ALU register ops, branches,
indirect addressing with autoincrement, `calls`/`rets`, push/pop, and
multiply/divide. Wall time is min-of-9 on an idle Ryzen 9 9900X3D, gcc 16.1,
`-O3 -flto`. Host instruction counts come from callgrind, which is
deterministic and immune to the ±8% code-alignment noise that plain timing has
between builds.

Correctness was checked by diffing all 16 accumulators, all 8 control
registers, and a hash of the full 256KW memory image against the pre-change
build. Every change here is bit-identical to the old behaviour on that
workload.

## Result

| build | time | MIPS | host instructions per emulated instruction |
|---|---|---|---|
| before | 0.975 s | 164 | 194 |
| after  | 0.688 s | **233 (+42%)** | 139 |

194 host instructions to emulate one ACR 7000 instruction was the headline
problem. A tight interpreter for a machine this simple belongs in the 40–80
range, so there is still room beyond what is applied here.

## Applied

### 1. `compute()` is now inlined (≈12%, the single biggest win)

`compute()` lived in `alu.c` with nine parameters. x86-64 passes six arguments
in registers, so all 26 call sites spilled three to the stack — that alone was
**51% of every data write the emulator performed**. Worse, every call site
passes literal constants (`compute(data, cpu->a[ac], get_cf(cpu), 6, 0, 0, 0,
0, 0)`), so the entire rotate/mask/skip pipeline was dead code the compiler
could not see through across the translation unit boundary. `-flto` did not
inline it — the function accounted for 22% of all host instructions executed.

`compute()` and its helpers moved to `include/alu.h` as `static inline`, which
folds `rotmask()` and `skip()` away entirely at the constant call sites.
`alu.c` remains as an (almost empty) translation unit so the object list and
build are unchanged.

### 2. The run loop got a fast path (≈10%)

Every iteration of `run()` reloaded nine fields from the CPU context —
`throttle`, `do_edit`, `c[C_CW]`, `min_pending`, `running`, `do_inc`,
`do_stack`, `cycles`, `exit` — roughly 35 host instructions of bookkeeping
before any work happened, none of which the compiler could cache in registers
because `exec_all()` may write through the same pointer.

`do_edit`/`do_edsk`/`do_inc`/`do_stack` are now bits of a single `deferred`
word (`DEF_EDIT`, `DEF_EDSK`, `DEF_INC`, `DEF_STACK`), and a new `event` flag
signals "something needs attention before the next fetch": a staged edit
target, a pending interrupt, a halt or stop request, or an active throttle.
`event` is set by `intr_assert`, `intr_release`, `intr_set_mask`, `halt`,
`leave_intr`, `stop_cpu`, `start_cpu`, `exec_smi` and the monitor's throttle
command, and recomputed by `cpu_event_state()` at the end of every slow-path
pass. The fast path reloads exactly two fields per instruction.

**The one trap here, which cost a debugging session:** the deferred
`DEF_INC`/`DEF_STACK` commit must happen in the *same* pass as the instruction
that staged it. Deferring it one iteration silently corrupts SP on the first
`push`/`pop` — the integer benchmark still passed, and a program using the
stack hung. `commit_deferred()` is therefore called from both paths.

`exec_smi` sets `event` unconditionally on entry, because `reti`/`ldctl`/
`ldmask`/`wait` can lower IRQL or unmask an IRQ, and the fast path does not
re-check `min_pending` on its own. System instructions are rare enough that
this costs nothing measurable.

### 3. `read_mem`/`write_mem` load the page key word once (≈3%)

The `else if` chain evaluated `cpu->memory[address & ~0x1FF] >> 36` up to three
times, and that word lives in a different cache line from the datum — so every
memory access touched two lines. All four branches return `memory[address]`
when `key == 0`, so the whole key check is now skipped for supervisor
accesses and collapsed into one load otherwise.

### 4. `cpu->inst = inst` removed (≈3%)

Nothing in the tree read `cpu->inst`. It was a store to the CPU struct on every
instruction. The struct field is left in place in case the front panel ever
wants it.

### 5. `acr7k_fnorm` uses `__builtin_clzll` (8.5× on that function)

`fpu.c` normalized the significand one bit at a time — up to 63 loop iterations
per call. Mandelbrot's inner loop uses the `n` suffix on nearly every operation
(`mlgn`, `sblng`, `adgnr`, `adgn`), so this ran constantly. `clzll` produces
the whole shift count at once. Verified bit-identical over 3M random inputs;
8.5× faster in isolation on a realistic shift distribution.

Not measured end to end — the benchmark is integer-only — but it should be
worth real time on the FP demos.

### 6. PGO is opt-in in the Makefile

`-fprofile-use` together with `-Werror` is a **hard build failure** whenever the
`.gcda` files are absent (any clean tree, and they are untracked) or stale (any
edit that changes a function's control flow, via `-Werror=coverage-mismatch`).
The tree as committed could not be built by anyone else.

PGO now lives behind a `pgo` target:

```
make clean && make PGO=-fprofile-generate
./acr7000                 # exercise a representative workload, then exit
make clean && make pgo    # clean keeps *.gcda; distclean discards them
```

## Tried and rejected — do not spend time on these

- **Jump-table `switch` for the `exec_all` opcode dispatch.** A wash, slightly
  negative. The if-else chain predicts perfectly; the indirect branch does not.
- **Fusing `set_cf` + `get_pc` + `set_pc` into one PSW read-modify-write.**
  Exactly zero change in host instruction count over 75 call sites. GCC already
  CSEs the PSW access within a basic block. The ~32M instructions callgrind
  attributes to `set_pc` are inlining misattribution, not real cost.
- **`-march=native`.** Helped the old build (0.98 → 0.89 s) but *hurt* the new
  one (0.695 → 0.727 s). That is code-alignment noise, not a real effect. Do
  not adopt it without re-measuring on whatever the code looks like at the time.

## Still on the table

- **Virtual memory translation is unprofiled.** The benchmark runs in real
  mode, so `read_vmem`/`seg_lookup`/`tlb_lookup` never got hot. With `C_SDR`
  nonzero, every single access does a descriptor lookup plus key, bounds and
  rights checks. The standard fix is a one-entry "last page translated" cache
  (virtual page → host pointer + rights) consulted before the segment walk.
  Worth profiling with a VM workload before writing any code.
- **139 host instructions per emulated instruction is still high.** The next
  structural step would be decoding once into a threaded/predecoded form rather
  than re-extracting bit fields from the raw word on every execution.
- **`cpu.c` has 78 copies of the read-then-check-`MEM_FAULT`-then-check-
  `KEY_FAULT` block** and 75 `set_pc(cpu, get_pc(cpu) + n)` calls. The I1 cache
  miss rate is 0.00%, so this is **not** costing performance — it is a
  maintenance and correctness-risk issue only. Fold it into a helper for
  readability, not for speed.

## Reproducing the measurement

The harness compiles `cpu.c` with `main` renamed, links the normal object set,
loads a program image directly into `cpu.memory[]`, and calls `run()` on the
current thread — no SDL, no monitor, no device threads. Programs are assembled
with `not_vibe_code/asm2.c` patched to call `output_c()` instead of
`output_r()`, which emits `cpu.memory[n] = 0...;` lines ready to `#include`.
