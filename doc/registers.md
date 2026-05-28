# RDC-700 Register Reference

## General-Purpose Registers (A0–A15)

The RDC-700 has 16 general-purpose 36-bit accumulators. They are mostly
interchangeable but a few have conventional or architectural roles.

| Number | Assembler name | Conventional role |
|--------|---------------|-------------------|
| A0 | `ac` | Primary accumulator; implicit source/destination for many instructions |
| A1 | `mq` | Multiply quotient; high word of 72-bit multiply results, quotient of divide |
| A2 | `xy` | Index/auxiliary accumulator; low word of 72-bit multiply, remainder of divide; FPU status accumulates here |
| A3 | `x0` | First index register; first integer argument (fastcall) |
| A4 | `x1` | Second index register; second integer argument |
| A5 | `x2` | Third index register; third argument |
| A6 | `x3` | Fourth index register; fourth argument |
| A7 | `x4` | |
| A8 | `x5` | |
| A9 | `x6` | |
| A10 | `x7` | |
| A11 | `ap` | Argument pointer |
| A12 | `lr` | Link register; `callr`/JSR stores the return address here |
| A13 | `sp` | Stack pointer; hardware-supported push/pop addressing modes |
| A14 | `r14` | Trap instruction word saved here by local trap instructions |
| A15 | `r15` | Return address or full PSW saved by local trap instructions |

**Note**: Registers A0 (ac), A1 (mq), and A2 (xy) cannot be used as index
registers in memory-reference addressing modes. Index registers are A3–A13
(x0–sp).

**Multiply/divide register conventions**: 72-bit signed multiply (`mul`) produces
the product in A2:A0 (high:low). Unsigned multiply (`umul`) is the same. Divide
(`div`, `udiv`) uses A0 as the 36-bit dividend and stores the quotient in A1 and
remainder in A2. Multiply-accumulate instructions (`fmadd`, `fmsub`, `ufmadd`,
`ufmsub`) accumulate into A2:A0 using the current carry.

---

## Control Registers (C0–C7)

Control registers are 36-bit words accessible only in supervisor mode (protection
key 0) via the `ldctl` and `stctl` instructions.

### C0 — Program Status Word (PSW0, `psw0`)

```
 35      28 27 26                                            0
┌──────────┬──┬──────────────────────────────────────────────┐
│  Key[7:0]│CF│              Program Counter [26:0]          │
└──────────┴──┴──────────────────────────────────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35–28 | Key | Current memory-protection key (0 = supervisor) |
| 27 | CF | Carry flag |
| 26–0 | PC | Program counter (word address) |

The carry flag is shared with ALU instructions. It is preserved across most
operations that do not explicitly modify it.

### C1 — Control Word (PSW1, `psw1`)

```
 35      32 31      28 27                18 17               0
┌──────────┬──────────┬────────────────────┬──────────────────┐
│  IRQL[3:0]│ PrevIRQL │    (reserved)      │  DPBase[17:0]    │
└──────────┴──────────┴────────────────────┴──────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35–32 | IRQL | Current interrupt priority level (0–15) |
| 31–28 | PrevIRQL | Previous IRQL, saved when an interrupt occurs |
| 17–0 | DPBase | Direct-page base: shifted left 9 to form a 27-bit page-aligned base address for direct-page addressing (index mode 1) |

**Direct page**: When an instruction uses index mode 1 (`_displacement` in assembly),
the effective address is `(DPBase << 9) + signed_displacement_18`. This provides a
fast 512-word "home segment" addressable without a full 27-bit address.

### C2 — FPU Control Word (`fpc`)

```
 35                   3  2  1  0
┌───────────────────────┬──┬────┐
│       (reserved)       │EN│Bnk│
└───────────────────────┴──┴────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 2 | EN | FPU enabled. If 0, any FP instruction raises exception X_NFPU (9) |
| 1–0 | Bnk | Float register bank selector (0–3). Selects which group of four from F0–F15 is visible as `f0`–`f3` in FP instructions |

The 16 floating-point registers are divided into four banks of four. Bank 0 exposes
F0–F3, bank 1 exposes F4–F7, and so on. The bank selector allows a subroutine to
use a private set of float registers without saving the caller's.

### C3 — Problem-Level Trap Table (`plt`)

```
 35      28 27 26                                            0
┌──────────┬──┬──────────────────────────────────────────────┐
│ (reserved)│V │              Table Base [26:0]               │
└──────────┴──┴──────────────────────────────────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 27 | V | Valid — if 0, any PLT instruction raises X_USER (0) |
| 26–0 | Base | Base address of the problem-level trap dispatch table (64 entries) |

### C4 — Supervisor-Level Trap Table (`slt`)

Same layout as C3/PLT. The V bit enables SLT instructions; the Base points to a
64-entry supervisor dispatch table.

### C5 — Segment Descriptor Register (`sdr`)

```
 35      27 26                                               0
┌──────────┬─────────────────────────────────────────────────┐
│  Limit[8:0]│              SDT Base [26:0]                   │
└──────────┴─────────────────────────────────────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35–27 | Limit | Maximum valid segment selector. A virtual address with selector > Limit generates a segment-not-present fault |
| 26–0 | Base | Physical base address of the Segment Descriptor Table |

When SDR = 0, segmentation is disabled and flat mode (page-key protection) is
active. Loading any non-zero value into SDR enables segmented virtual memory and
flushes all segment and TLB caches.

### C6 — Segment Fault Status (`sflt`)

Set by the hardware on any memory-access fault in segmented mode. The low 27 bits
contain the faulting virtual address; bits 27–30 encode the fault type:

| Bits 30:27 | Constant | Meaning |
|------------|----------|---------|
| 0000 | SEG_FAULT_PRESENT | Segment or page not present |
| 0001 | SEG_FAULT_KEY | Protection key mismatch |
| 0010 | SEG_FAULT_BOUNDS | Offset beyond segment bounds |
| 0011 | SEG_FAULT_RIGHTS | Segment is read-only (write attempted) |

Bit 29 (SEG_FAULT_WRITE) is additionally set for write faults. Bit 30
(SEG_FAULT_PAGE) is set when the fault originated in the page table (TLB miss
or write-protected page) rather than the segment descriptor.

### C7 — Reserved (`cr8`)

Currently unused; reserved for future expansion.

---

## Floating-Point Registers (F0–F15)

Floating-point registers hold values in an 80-bit internal format: a 1-bit sign,
a 15-bit biased exponent (excess-16383), and a 64-bit significand with explicit
leading 1. This is similar to IEEE 754 extended precision but with distinct special
value encoding (see the [Floating-Point](fpu.md) chapter).

Only four registers are directly addressable in any instruction — `f0` through
`f3` — corresponding to the bank selected by C2[1:0]. To use a different bank,
write the bank number to `fpc`.

---

## Interrupt Vector Table

A 32-word region at physical addresses 0–31 forms the interrupt vector table.
Each priority level occupies two words: the entry PSW0 at `2×irq` and the entry
PSW1/CW at `2×irq + 1`. On interrupt, the current PSW0 and PSW1 are saved to
`32 + 2×prev_irql` and `33 + 2×prev_irql` (the "save area"), and execution
resumes from the entry PSW.
