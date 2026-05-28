# RDC-700 Architecture and Programming Documentation

## Contents

1. [Architecture Overview](overview.md) — word width, address space, execution model, protection modes
2. [Register Reference](registers.md) — general-purpose, control, and floating-point registers in detail
3. [Memory Organization and Addressing](memory.md) — flat and segmented modes, address field syntax, segment/page descriptor formats
4. [Instruction Set Reference](instructions.md) — all instruction classes with encoding and semantics
5. [ALU Instruction Detail](alu.md) — rotate/mask pipeline, carry semantics, skip conditions, encoding
6. [Floating-Point Architecture](fpu.md) — formats, registers, FM/FR instructions, special values
7. [Interrupt and Exception System](interrupts.md) — priority levels, vector table, dispatch, exceptions
8. [Assembler Reference](assembler.md) — syntax, directives, address fields, output format, calling conventions
9. [TTY Console Device](tty.md) — Telnet-based serial console, I/O register model, interrupt-driven and polled patterns
10. [Bishop Bitmap Display](bishop.md) — monochrome raster display, DMA read and pattern fill, scroll register, rect/base tiling
11. [Paper Tape Reader and Punch](papertape.md) — byte-at-a-time file-backed I/O, timing, RIM loader usage
12. [Line Printer](lpt.md) — 132-column line buffer, flush conditions, interrupt on line completion

## Quick Reference

### Register Names

| Number | Name | Role |
|--------|------|------|
| A0 | `ac` | Accumulator / return value |
| A1 | `mq` | Multiply quotient / high product |
| A2 | `xy` | Auxiliary / low product / FPU status |
| A3–A10 | `x0`–`x7` | Index registers / arguments |
| A11 | `ap` | Argument pointer |
| A12 | `lr` | Link register |
| A13 | `sp` | Stack pointer |
| A14 | `r14` | Trap instruction save |
| A15 | `r15` | Trap PSW/PC save |
| C0 | `psw0` | PC + carry + protection key |
| C1 | `psw1` | IRQL + direct-page base |
| C2 | `fpc` | FPU enable + register bank |
| C3 | `plt` | Problem-level trap table |
| C4 | `slt` | Supervisor-level trap table |
| C5 | `sdr` | Segment descriptor register |
| C6 | `sflt` | Segment fault status |

### Addressing Mode Prefixes (Assembly)

| Prefix | Mode |
|--------|------|
| `n` | Absolute |
| `_n` | Direct page |
| `.n` / `.label` | PC-relative |
| `n(Xk)` | Register-indexed |
| `+n` | Post-increment SP |
| `=n` | Pre-decrement SP |
| `@` (prefix to any) | Indirect |

### Common ALU Mnemonics

```
movr    — move register
addr    — add registers
subr    — subtract registers
xorr    — XOR registers (xorr ac, ac → clear AC)
incr    — increment register
comr    — complement register
ngtr    — negate register
bisr    — OR registers (bit-set)
movrz   — move with carry=0 init
movr.rn — move and skip-if-zero (test nonzero)
movr.rz — move and skip-if-nonzero (test zero)
addi    — add 13-bit immediate
subin   — subtract immediate (with n=no-write for compare)
```

### Exception Codes (C1/psw1 bits 27–24 on entry to level 0)

| Code | Name | Cause |
|------|------|-------|
| 0 | X_USER | Unimplemented instruction |
| 1 | X_INST | Illegal instruction |
| 2 | X_MEMX | Memory bounds fault |
| 3 | X_DEVX | Device not present |
| 4 | X_PPFR | Protection fault (read) |
| 5 | X_PPFW | Protection fault (write) |
| 6 | X_PPFS | Privileged instruction in user mode |
| 8 | X_DIVZ | Divide by zero |
| 9 | X_NFPU | FPU not enabled |
