# RDC-700 Interrupt and Exception System

## Priority Levels

The RDC-700 supports 16 interrupt priority levels (0–15). Lower numbers have
higher priority (level 1 preempts level 2, etc.):

| Level | Role |
|-------|------|
| 0 | Exception level (internal processor faults) |
| 1–14 | External device interrupts (maskable) |
| 15 | Non-maskable / power-fail |

Level 0 is never asserted by external devices; it is entered only via the exception
mechanism. Level 15 is intended for power-failure notification and cannot be masked.

---

## Interrupt Vector Table

Physical addresses 0–31 hold the interrupt vector table. Each priority level
occupies two consecutive words:

| Address | Contents |
|---------|----------|
| 2×n | Entry PSW0 for level n (including new PC and key) |
| 2×n+1 | Entry PSW1/CW for level n (including return IRQL and direct-page base) |

**Entry PSW0** format on entry: the new PC is in bits 26–0; the key for the handler
in bits 35–28; bit 27 (CF) is usually 0.

**Entry PSW1** format: the lower 18 bits set the new direct-page base; bits 31–28
must encode the previous IRQL (set by hardware during dispatch). The emulator
places bits 35–32 = new IRQL and bits 31–28 = old IRQL.

---

## Interrupt Save Area

Physical addresses 32–63 form the interrupt save area. When an interrupt at
level `n` occurs while the processor is running at level `k`:

```
memory[32 + 2k]     ← saved PSW0 (old PC, old key, old CF)
memory[33 + 2k]     ← saved PSW1 (old IRQL=n, old DPBase, prev_IRQL=k)
```

Return from interrupt restores from this area.

---

## Interrupt Dispatch

On asserting an interrupt at level `n`:

1. The hardware checks whether `n < current_IRQL` (smaller number = higher priority).
2. If so, saves PSW0 → `memory[32 + 2×current_IRQL]` and PSW1 →
   `memory[33 + 2×current_IRQL]`.
3. Loads new PSW1 from `memory[2n+1]` (sets IRQL = n, prev_IRQL = old IRQL).
4. Loads new PSW0 from `memory[2n]` (sets PC and key).
5. Any pending `edit`/`edits` state and stack-pointer auto-modify is cancelled.

---

## Interrupt Mask

A 16-bit interrupt mask register controls which external levels are enabled.
Bit `n` of the mask enables level `n` (1=enabled, 0=masked). Levels 0 and 15 are
always active and should not be masked (though the hardware will accept any mask
value).

The mask is managed by:

- `ldmask ea` — load new mask from memory (supervisor only)
- `stmask ea` — store current mask to memory (supervisor only)
- `lmwait ea` — load mask and halt (atomic mask-and-wait; supervisor only)
- `retlmi ea` — load mask and return from interrupt (supervisor only)

**Typical interrupt-enable sequence**:
```asm
ldmask  .irq_mask       ; irq_mask: dw 0x7FFE (enable levels 1-14, disable 15)
reti                    ; return from interrupt
```

---

## Return From Interrupt

```asm
reti            ; restore PSW from save area; PC = saved PC
retid  n        ; restore PSW; PC = saved PC + n (skip n instructions at return point)
retlmi ea       ; load mask from M[ea], then reti
```

The `retid n` variant is essential for exception handlers. When an exception is
raised, the PC points to the faulting instruction. `reti` would re-execute it;
`retid 1` skips it; `retid 0` re-executes it (normal for restartable faults after
the underlying condition is resolved).

---

## Exceptions (Level 0)

All synchronous processor faults dispatch through interrupt level 0. A four-bit
exception code is encoded in bits 27–24 of the new CW (PSW1) after dispatch.
Handlers can read this from the saved PSW1 in memory.

| Code | Constant | Cause |
|------|----------|-------|
| 0 | X_USER | Unimplemented instruction (undefined opcode, or PLT/SLT with V=0) |
| 1 | X_INST | Illegal instruction (valid opcode but invalid field combination) |
| 2 | X_MEMX | Memory address fault (physical address out of range) |
| 3 | X_DEVX | I/O device not present |
| 4 | X_PPFR | Protection fault — read or execute |
| 5 | X_PPFW | Protection fault — write |
| 6 | X_PPFS | Protection fault — privileged instruction executed in user mode |
| 7 | X_TIME | Timer interrupt (dispatched through level 0 when timer fires) |
| 8 | X_DIVZ | Divide by zero |
| 9 | X_NFPU | FPU not enabled (fpc bit 2 = 0) |
| 14 | X_MCHK | Machine check (hardware error) |
| 15 | X_PWRF | Power failure |

**Exception dispatch sequence**: An exception first executes as an interrupt to
level 0, saving the current PSW at the level-0 save area. The exception code
appears in CW bits 27–24. The handler at the level-0 vector gains supervisor
access to diagnose and resolve the fault.

---

## Interrupt Controller Conventions

External device interrupt levels 1–14 are level-triggered and can be asserted by
multiple devices simultaneously. The processor tracks, for each level, how many
devices are currently asserting it (a count, not just a flag). An interrupt level
is considered pending only while the count is greater than zero.

Devices assert their level by calling `intr_assert` and release by calling
`intr_release` (in the emulator). In real hardware this would correspond to
pulling a shared open-collector interrupt line.

---

## Software Interrupt (INT Instruction)

```asm
intr    Rn, ea      ; assert software interrupt at level Rn; PC ← ea
```

The `intr` instruction forces an interrupt dispatch at the level specified in Rn.
It saves the current PSW and jumps through the interrupt vector table as if a
hardware interrupt had occurred. This is used for:

- OS call gates (user code executes `intr 0` to invoke the exception handler)
- Testing interrupt handlers
- Inter-processor signalling in multi-CPU systems

---

## Local Traps (PLT and SLT)

Local traps provide a faster alternative to full interrupts for OS calls. See the
[instruction reference](instructions.md#local-trap-instructions) for details.

**PLT vs. SLT comparison**:

| | PLT | SLT |
|-|-----|-----|
| Mode change | No (stays in user mode) | Yes (key → 0, supervisor) |
| Saves | PC in A15 | Full PSW0 in A15 |
| Return | `jmp (r15)` or similar | `retsv` |
| Stack usage | None | None |
| Speed | Fast | Fast |
| Use case | User-mode dispatch table | Privilege escalation / syscall |

The PLT and SLT instruction words themselves carry a 6-bit index into their
respective dispatch tables (opcode bits 5–0), allowing 64 distinct trap entries
per table.

---

## Idle Loop Pattern

The monitor uses `lmwait` for a race-free idle loop:

```asm
idle_loop:
    pushim              ; save interrupt mask on stack
    ldmask  .enable_all ; enable relevant interrupts
    callr   (x0)        ; check if work is available (test subroutine)
    movr.rn ac, ac      ; if result nonzero (work found), skip jmp
    jmp     .done_idle
    lmwait  (sp)        ; atomically restore old mask and halt
    jmp     .idle_loop  ; wake up on interrupt, check again
done_idle:
    popim               ; restore interrupt mask
    rets
```

`lmwait` prevents the TOCTOU race between enabling interrupts and halting: if an
interrupt fires between `ldmask` and `halt` in a two-instruction sequence, the
processor would halt with the interrupt already pending and never wake up.
`lmwait` makes the mask-load and halt a single atomic operation.
