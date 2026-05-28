# RDC-700 Instruction Set Reference

## Instruction Word Format

Every instruction is one 36-bit word. Instructions are distinguished by the
top bits of the word:

```
 35  33   Bits [35:27]     Instruction class
  111 xxx  F[35:32]=0xE..0xF  ALU register-to-register
  000 0xx                     Memory reference (MR)
  000 001                     Multiply/divide
  001 0xx – 001 1xx           Accumulator–memory (AM), opcodes 040–067
  010 xxx – 011 xxx           Local trap (PLT/SLT), opcodes 0200–0377
  100 0xx – 100 1xx           Floating-point memory (FM)
  100 1xx                     Byte-field (BX)
  100 1xx – 100 1xx           (FM continued)
  101 0xx                     Float register-to-register (FR)
  110 100 000                 I/O (IO1)
  (all others with bit 35:33 ≠ 111) System management (SMI), opcodes 010, 070–076
```

The following sections describe each class in detail.

---

## Notation

Throughout this reference:
- `ea` — effective address computed by the address field
- `M[ea]` — word at effective address in memory
- `Rn` — general-purpose register n
- `CF` — carry flag (PSW bit 27)
- `→` — assignment
- *Skip* — the instruction following the current one is skipped (PC advances by 2 instead of 1)

**Skip convention**: Unless stated otherwise, an instruction advances the PC by 1.
When a skip condition is met, the PC advances by 2, bypassing the immediately
following instruction. Skip conditions are named from the perspective of the
condition that causes normal (non-skip) execution; when that condition is false,
the next instruction is skipped.

---

## Memory Reference Instructions (MR)

These instructions encode a full address field (I, index, displacement). The
source or destination register (if any) occupies bits 26–23 of the instruction
word. Bits 35–27 encode the operation subtype.

### JMP — Unconditional Jump
**Opcode**: `000 000 0000` (bits 35–27 = 0, bits 26–23 = 0)  
**Assembly**: `jmp ea`  
**Operation**: PC ← ea

### CALLR — Call Subroutine (Link Register)
**Opcode**: bits 26–23 = 1  
**Assembly**: `callr ea`  
**Operation**: A12(LR) ← PC+1; PC ← ea

Stores the return address in the link register (A12) and jumps. Return via
`jmp (lr)` or `retr`.

### ISZ — Increment Memory, Skip if Zero
**Opcode**: bits 26–23 = 2  
**Assembly**: `inctnz ea`  
**Operation**: M[ea] ← M[ea] + 1; if result = 0: *Skip*

Reads, increments, and writes back a memory location. Skips the following
instruction if the result wraps to zero (i.e., was −1 before the increment).

### DSZ — Decrement Memory, Skip if Zero
**Opcode**: bits 26–23 = 3  
**Assembly**: `dectnz ea`  
**Operation**: M[ea] ← M[ea] − 1; if result = 0: *Skip*

### SZR — Skip if Memory Zero
**Opcode**: bits 26–23 = 4  
**Assembly**: `tstmz ea`  
**Operation**: if M[ea] = 0: *Skip*  
Does not modify memory.

### SNZ — Skip if Memory Non-Zero
**Opcode**: bits 26–23 = 5  
**Assembly**: `tstmnz ea`  
**Operation**: if M[ea] ≠ 0: *Skip*  
Does not modify memory.

### CALL — Call Subroutine with Register Save
**Opcode**: bits 26–23 = 14 (0xE)  
**Assembly**: `calls ea`  
**Operation**: Reads a register-save mask from M[ea]; pushes selected registers
and return info on the stack; jumps to ea+1.

The word at `ea` must be a register mask: a 16-bit value where bit `15−n` is
set to save register An. The stack frame, from low address (new SP) to high:

```
SP+0  : PC+1 (return address)
SP+1  : register save mask
SP+2  : A0  (if bit 15 of mask set)
SP+3  : A1  (if bit 14 of mask set)
  ...
SP+2+n: A15 (if bit 0 of mask set)
```

The `save` assembler directive generates a mask word. For example:

```asm
myfunc: save    x0, x1, x2       ; generates mask word
        ...                      ; function body
        rets                     ; return
```

### RET — Return from Subroutine with Register Restore
**Opcode**: bits 26–23 = 15 (0xF)  
**Assembly**: `rets` (offset=0) or `retsd offset` (with stack adjustment)  
**Operation**: Restores saved registers and returns from a `calls` frame.

The effective address is added to SP before reading the frame, allowing
`retsd n` to discard `n` words of arguments that were pushed before the call.
SP is restored to after the frame unless it was among the saved registers.

---

## Multiply and Divide Instructions (MD)

These use the same address field as MR instructions (bits 22–0) but with bits
35–27 = 0000 0001. The suboperation is in bits 26–23.

All multiply instructions use A1(MQ) as the multiplicand. The 72-bit product
occupies A2(high):A0(low).

### MPY — Multiply (Signed)
**Assembly**: `mul ea`  
**Operation**: A2:A0 ← signed(A1) × signed(M[ea])

### MPA — Multiply-Accumulate (Signed)
**Assembly**: `fmadd ea`  
**Operation**: A2:A0 ← A2:A0 + signed(A1) × signed(M[ea]); CF ← carry out

Accumulates the product into the existing A2:A0 value. Useful for dot products.

### MNA — Multiply-Negate-Accumulate (Signed)
**Assembly**: `fmsub ea`  
**Operation**: A2:A0 ← A2:A0 + signed(A1) × (−signed(M[ea])); CF ← carry out

Subtracts the product from the accumulator.

### DIV — Divide (Signed)
**Assembly**: `div ea`  
**Operation**: A1 ← A0 / M[ea]; A2 ← A0 mod M[ea]  
**Exception**: X_DIVZ if M[ea] = 0

Uses A0 as the 36-bit dividend. Stores quotient in A1 and remainder in A2.

### MU — Multiply (Unsigned)
**Assembly**: `umul ea`  
**Operation**: A2:A0 ← unsigned(A1) × unsigned(M[ea])

### MAU — Multiply-Accumulate (Unsigned)
**Assembly**: `ufmadd ea`  
**Operation**: A2:A0 ← A2:A0 + unsigned(A1) × unsigned(M[ea]); CF ← carry out

### MNAU — Multiply-Negate-Accumulate (Unsigned)
**Assembly**: `ufmsub ea`  
**Operation**: A2:A0 ← A2:A0 + unsigned(A1) × (−unsigned(M[ea])); CF ← carry out

The "negate" in MNAU is applied to the memory operand before multiplication;
the accumulator is treated as unsigned.

### DU — Divide (Unsigned)
**Assembly**: `udiv ea`  
**Operation**: A1 ← A0 / M[ea]; A2 ← A0 mod M[ea] (unsigned)  
**Exception**: X_DIVZ if M[ea] = 0

---

## Accumulator–Memory Instructions (AM)

These instructions encode both a register field (bits 26–23) and an address field
(bits 22–0). The instruction subtype is in bits 35–27 (range 040–067 octal).

### EDIT — Edit (Self-Modifying Code)
**Opcode**: 041  
**Assembly**: `edit Rn, ea`  
**Operation**: instruction ← (M[ea] | Rn) using OR; execute that word as the next instruction.

The word at `ea` is combined with Rn using OR and executed as the following instruction.
The resulting instruction word is not written back to memory — it exists only for the
one execution. PC does not change (EDIT replaces what would have been the next fetch).

### EDITS — Edit and Skip
**Opcode**: 042  
**Assembly**: `edits Rn, ea`  
**Operation**: Same as EDIT, but the edited instruction is also treated as a skip:
after the edited instruction completes, PC advances by one extra word.

### LDEA — Load Effective Address
**Opcode**: 043  
**Assembly**: `ldea Rn, ea`  (or `ldea ea` to load into AC)  
**Operation**: Rn ← ea

Loads the computed effective address (27-bit, before any memory access) into
register Rn. Useful for computing pointer values.

### ADDEA — Add Effective Address
**Opcode**: 044  
**Assembly**: `addea Rn, ea`  
**Operation**: Rn ← Rn + ea; CF ← carry

Adds the effective address to Rn.

### ISE — Increment Register, Skip if Equal to Memory
**Opcode**: 045  
**Assembly**: `inctne Rn, ea`  
**Operation**: Rn ← Rn + 1; CF ← carry; if Rn = M[ea]: *Skip*

Increments the register and skips if it equals the memory word. Loop idiom:
decrement a counter in memory, skip out when the register matches the count.

### DSE — Decrement Register, Skip if Equal to Memory
**Opcode**: 046  
**Assembly**: `dectne Rn, ea`  
**Operation**: Rn ← Rn − 1; CF ← carry; if Rn = M[ea]: *Skip*

### LDEAS — Load Effective Address Shifted
**Opcode**: 047  
**Assembly**: `ldeas Rn, ea`  
**Operation**: Rn ← ea << 17 (bits 26–0 of ea placed in bits 35–9 of Rn; low 9 bits cleared)

Packs an address into the high portion of a register, useful for constructing
indirect auto-modify words (bit 35 set indicates auto-modify mode).

### LDCOM — Load Complement
**Opcode**: 050  
**Assembly**: `ldcom Rn, ea`  
**Operation**: Rn ← ~M[ea]

### LDNEG — Load Negation
**Opcode**: 051  
**Assembly**: `ldneg Rn, ea`  
**Operation**: Rn ← −M[ea] (two's complement)

### LD — Load
**Opcode**: 052  
**Assembly**: `ld Rn, ea`  or  `ld ea`  (loads AC when Rn omitted)  
**Operation**: Rn ← M[ea]

The `pop Rn` pseudo-instruction expands to `ld Rn, +1` (post-increment SP pop).

### ST — Store
**Opcode**: 053  
**Assembly**: `st Rn, ea`  or  `st ea`  (stores AC when Rn omitted)  
**Operation**: M[ea] ← Rn

The `push Rn` pseudo-instruction expands to `st Rn, =1` (pre-decrement SP push).

### ADCM — Add Complement of Memory
**Opcode**: 054  
**Assembly**: `addcom Rn, ea`  
**Operation**: Rn ← Rn + ~M[ea]; CF ← carry  
(computes Rn − M[ea] − 1 with carry out)

### SUBM — Subtract Memory
**Opcode**: 055  
**Assembly**: `sub Rn, ea`  
**Operation**: Rn ← Rn − M[ea]; CF ← carry (borrow notation)

### ADDM — Add Memory
**Opcode**: 056  
**Assembly**: `add Rn, ea`  
**Operation**: Rn ← Rn + M[ea]; CF ← carry

### ANDM — AND Memory
**Opcode**: 057  
**Assembly**: `and Rn, ea`  
**Operation**: Rn ← Rn & M[ea]

### ORM — OR Memory
**Opcode**: 062  
**Assembly**: `or Rn, ea`  
**Operation**: Rn ← Rn | M[ea]

### XORM — XOR Memory
**Opcode**: 066  
**Assembly**: `xor Rn, ea`  
**Operation**: Rn ← Rn ⊕ M[ea]

---

## ALU Register-to-Register Instructions

These instructions operate entirely on registers without accessing memory. They
are identified by bits [35:33] = 111.

The ALU is described in detail in [alu.md](alu.md). This section covers the
instruction format, encoding, and assembler mnemonics.

### Instruction Encoding

```
 35  33  32  31  30      27  26      23  22      20  19  18  17      15
┌─────┬───┬───┬──────────┬──────────┬──────────┬───────┬────────────┐
│ 111 │ NW│ H │  Src[3:0] │  Dst[3:0]│  Op[2:0] │  CI   │  Cond[2:0] │
└─────┴───┴───┴──────────┴──────────┴──────────┴───────┴────────────┘
 14   13   12   11          6  5                  0
┌───┬────┬───┬─────────────────┬──────────────────┐
│Mode│Sub │ MR│   Field A [5:0] │   Field B [5:0]  │
└───┴────┴───┴─────────────────┴──────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35–33 | — | Always 111 (ALU class marker) |
| 32 | H (op bit 3) | High bit of operation code. Combined with Op[2:0] forms a 4-bit operation |
| 31 | NW | No-Write: compute but do not store result in Dst |
| 30–27 | Src | Source register (A0–A15) |
| 26–23 | Dst | Destination register (A0–A15) |
| 22–20 | Op[2:0] | Low 3 bits of operation code |
| 19–18 | CI | Carry initialization: 0=use CF, 1=force CF=0, 2=force CF=1, 3=invert CF |
| 17–15 | Cond | Skip condition (see below) |
| 14 | Mode | 0=R/M-type, 1=S-type or Immediate |
| 13 | Sub | Sub-mode: 0=shift/S-type, 1=immediate |
| 12 | MR | Rotate direction: 0=left, 1=right (negate rotation amount) |
| 11–6 | Field A | Mask amount (R/M-type) or destination reg (S-type ADR) |
| 5–0 | Field B | Rotation amount (R/M-type) or rotation/shift amount |

### Operations

The 4-bit operation code (H:Op[2:0]) selects the arithmetic or logical function
applied to the source and destination operands:

| Op code | Assembler | Operation | Result |
|---------|-----------|-----------|--------|
| 0 | `com` | Complement | ~Src |
| 1 | `ngt` | Negate | −Src (two's complement) |
| 2 | `mov` | Move | Src |
| 3 | `inc` | Increment | Src + 1 |
| 4 | `adc` | Add complement (subtract borrow) | Dst − Src − 1 |
| 5 | `sub` | Subtract | Dst − Src |
| 6 | `add` | Add | Dst + Src |
| 7 | `and` | AND | Dst & Src |
| 10 (8) | `bis` | Bit Set (OR) | Dst \| Src |
| 14 (12) | `xor` | XOR | Dst ⊕ Src |
| 15 (13) | `pct` | Popcount | popcount(Src) |

For unary operations (com, ngt, mov, inc), only Src is used; Dst is the
destination register. For binary operations (adc through xor), both operands are
used.

### Carry Semantics

ALU instructions produce a 37-bit result internally. Bit 36 of this result becomes
the new CF value after the instruction. The carry initialization field (CI) sets
the carry input before the operation begins.

For `add` (op 6): carry out is set if the 36-bit sum wraps (unsigned overflow).  
For `sub` (op 5): carry behavior follows subtract-with-borrow; carry is set if
Dst ≥ Src (no borrow required).  
For `adc` (op 4): carry is set if Dst > Src.  
For `inc` (op 3): carry is flipped if Src wraps from all-ones to zero.

### Skip Conditions

After the result is computed, a skip condition may be tested:

| Code | Suffix | Execute-when | Skip-when |
|------|--------|--------------|-----------|
| 0 | `.no` | (never skip) | never |
| 1 | `.sk` | (always skip) | always |
| 2 | `.cn` | carry set | carry not set |
| 3 | `.cz` | carry not set | carry set |
| 4 | `.rn` | result ≠ 0 | result = 0 |
| 5 | `.rz` | result = 0 | result ≠ 0 |
| 6 | `.bn` | result ≠ 0 AND carry set | result = 0 OR carry not set |
| 7 | `.bz` | result = 0 OR carry not set | result ≠ 0 AND carry set |

**Convention**: the suffix names the condition under which the next instruction
executes normally. The next instruction is *skipped* when that condition is false.
Example: `movr.rn ac, ac` — executes next instruction when AC is nonzero; skips
it when AC is zero (effectively "branch if zero").

### Assembler Syntax

ALU register instructions are written as:

```
{op}{mode}[modifiers][.condition]  src, dst[, arg2[, arg3]]
```

Where:
- `{op}` is the 3-letter base mnemonic (com, ngt, mov, inc, adc, sub, add, and,
  bis, xor, pct)
- `{mode}` is one of: `r` (R-type), `m` (M-type, argument order swapped), `s`
  (S-type/shift), `i` (immediate)
- Modifiers (in order): `t` (rotate through carry, 37-bit), `r` (rotate/shift
  right), `z` (CI=0), `s` (CI=1), `c` (CI=invert), `n` (no-write)
- `.condition` is one of `.no .sk .cn .cz .rn .rz .bn .bz`

#### R-type (mode `r`): rotate and mask

```
{op}r[t][r][z|s|c][n][.cond]  src, dst [, mask [, rotate]]
```

Applies rotate and mask to the result before storing:
1. Compute ALU result from Src (and Dst for binary ops)
2. Rotate left by `rotate` bits (right if `r` modifier present; through carry if `t`)
3. Apply mask: positive mask fills top bits with carry, negative fills bottom bits

When mask and rotate are both omitted, no rotation or masking is applied (plain
register operation).

**Examples**:
```asm
addr    x0, x1          ; x1 ← x0 + x1
subr    ac, x0          ; x0 ← ac - x0
movr    x1, x0          ; x0 ← x1
incr    ac, ac          ; ac ← ac + 1
xorr    ac, ac          ; ac ← 0 (ac XOR ac)
movr.rn ac, ac          ; test ac; skip next if ac = 0
movr.rz x0, x0          ; test x0; skip next if x0 ≠ 0
comr    ac, ac          ; ac ← ~ac
```

#### M-type (mode `m`): same as R but arg order differs

In M-type, the rotation amount is arg2 and mask is arg3 (reversed from R-type).

#### S-type (mode `s`): shift with alternate destination

```
{op}s[r][z|s|c][n][.cond]  src, tgt, dst [, shift]
```

Shift amount is encoded as both mask and rotate (mk = −rt), creating a logical
shift operation. The result is stored in a third register `dst` (the alternate
destination register, ADR).

**Examples**:
```asm
adds    x0, x1, x2          ; x2 ← x0 + x1 (store to x2, not x1)
movrs   ac, x0, x1, 4       ; x1 ← x0 >> 4 (logical right shift 4)
```

#### Immediate type (mode `i`): 13-bit signed immediate

```
{op}i[z|s|c][n][.cond]  src, dst, immediate
```

The second operand is replaced by a 13-bit signed immediate value (range −4096
to +4095, or 0–017777 octal/0–0x1FFF hex).

**Examples**:
```asm
addi    x0, x0, 1           ; x0 ← x0 + 1
addi.rn x1, x1, -1          ; x1 ← x1 - 1; skip next if result ≠ 0
subin.rz ac, ac, 10         ; ac ← ac - 10; skip next if result = 0
xorri   ac, ac, 0777        ; ac ← ac ⊕ 0777
bisi    x5, x5, #180        ; x5 ← x5 | 0x180 (set present/writable bits)
```

### Compare Instructions

The following are pre-built ALU encodings using subtraction with specific skip
conditions. Both operands are registers.

```
{cmpXX}  src, tgt
```

Execute the next instruction when `src {op} tgt` is true (skip when false):

| Mnemonic | Execute-when | Description |
|----------|--------------|-------------|
| `cmpne` | src ≠ tgt | Skip when equal |
| `cmp` | src = tgt | Skip when not equal |
| `cmplt` | src < tgt | Skip when src ≥ tgt |
| `cmpge` | src ≥ tgt | Skip when src < tgt |
| `cmpgt` | src > tgt | Skip when src ≤ tgt |
| `cmple` | src ≤ tgt | Skip when src > tgt |

Comparisons treat operands as signed 36-bit two's-complement values.

---

## Byte-Field Instructions (BX)

These instructions load and store arbitrary-width bit fields from memory, using a
pointer register to track position and width within a word. The pointer format
encodes both a word address and a bit position:

```
 35      27  26                                              0
┌──────────┬──────────────────────────────────────────────────┐
│  Shift[8:0] (in bits 35–27 of pointer register)            │
│   — only bits 35–27 are used; 26–0 is the word address     │
└──────────┴──────────────────────────────────────────────────┘
```

The pointer register stores: `(shift << 27) | word_address`. The `shift` field
gives the number of bits from the LSB of the word where the current byte starts;
`word_address` is the 27-bit memory address. A byte of `size` bits occupies bits
`[shift + size − 1 : shift]` of the word. Bytes are packed from the LSB upward;
when the remaining space in a word is too small for the next byte, the pointer
advances to the next word.

### LCH — Load Character
**Assembly**: `ldb ac, ix, size`  
**Operation**: Rn ← bits [sh+size−1 : sh] of M[addr]; (pointer not modified)

Extracts `size` bits starting at bit `sh` of the addressed word into Rn. The
pointer register `ix` is not modified.

### DCH — Deposit Character
**Assembly**: `stb ac, ix, size`  
**Operation**: M[addr][sh+size−1 : sh] ← Rn[size−1 : 0]; (pointer not modified)

Deposits the low `size` bits of Rn into the bit field at the current pointer
position. Other bits of the word are unchanged. The pointer is not modified.

### ICX — Increment Character Pointer
**Assembly**: `incbx Rn, ix, size`  
**Operation**: pointer ← advance(pointer, size); Rn ← new pointer  
**Effect**: Advances the byte pointer by `size` bits. If the pointer would exceed
bit 35 of the current word, it wraps to bit `36 − size` of the next word.

### ILC — Increment and Load Character
**Assembly**: `incldb Rn, ix, size`  
**Operation**: pointer ← advance(pointer, size); Rn ← extracted character

Advances the pointer then loads the character at the new position. Updates the
pointer register `ix` in place. Use for sequential reads from a packed byte string.

### IDC — Increment and Deposit Character
**Assembly**: `incstb Rn, ix, size`  
**Operation**: pointer ← advance(pointer, size); store Rn into new position

Advances the pointer then deposits. Updates `ix`. Use for sequential writes.

**Example — iterating 7-bit ASCII bytes**:
```asm
ldea    x0, string_addr         ; initialize pointer to first byte position
incldb  ac, x0, 7              ; advance and load next 7-bit byte
movr.rz ac, ac                 ; skip (done) if byte is 0
...
```

The byte size parameter may be 1–36. Common sizes: 7 (ASCII), 8 (EBCDIC/binary),
9 (sixbit), 6 (legacy packed).

---

## System Management Instructions (SMI)

These instructions are privileged (require protection key 0). Any attempt to
execute them in user mode raises exception X_PPFS (6).

### HLT — Halt
**Assembly**: `hlt` (halt with AC=0, PC remains) or `wait Rn, ea`  
**Operation**: Saves AC as stop code; sets PC to ea; halts the processor.

The processor enters a wait state. It resumes when a pending interrupt at a
priority level higher than the current IRQL is asserted.

### INT — Assert Software Interrupt
**Assembly**: `intr Rn, ea`  
**Operation**: PC ← ea; take interrupt at priority level Rn

Forces an interrupt dispatch at the priority level in Rn, saving the current
PSW and jumping to the interrupt handler. Used for software-initiated exceptions
and privilege escalation.

### RFI — Return From Interrupt
**Assembly**: `reti` (advance PC by 0 after restore) or `retid n` (advance by n)  
**Operation**: Restores PSW0 and PSW1 from the save area for the old IRQL; PC ← old PC + 0 (or n)

Restores the interrupted context. The `retid n` form adds a displacement to the
restored PC, allowing the returning handler to skip instructions at the
interrupted point (used for restarting or skipping faulting instructions).

### RMSK — Load Interrupt Mask and Return
**Assembly**: `retlmi ea`  
**Operation**: Sets interrupt mask from M[ea]; then performs RFI

Loads a new interrupt mask and returns from interrupt in one operation.

### LDMSK — Load Interrupt Mask
**Assembly**: `ldmask ea`  
**Operation**: Interrupt mask ← M[ea]; PC ← PC+1

The interrupt mask is a 16-bit value; bit n enables priority level n. Both bits
0 and 15 are architecturally special (exception and NMI levels respectively).

### MWAIT — Mask and Wait
**Assembly**: `lmwait ea`  
**Operation**: Interrupt mask ← M[ea]; halt processor until an enabled interrupt arrives

Atomic mask-and-halt: sets the mask and suspends the processor. Ensures no
interrupt can be missed between setting the mask and halting.

### STMSK — Store Interrupt Mask
**Assembly**: `stmask ea`  
**Operation**: M[ea] ← interrupt mask; PC ← PC+1

### INVSM — Invalidate Segment Cache
**Assembly**: `invlsg ea`  
**Operation**: Flushes the segment cache entry for the segment containing ea

### INVPG — Invalidate TLB Entry
**Assembly**: `invlpg ea`  
**Operation**: Flushes the TLB entry for the page containing ea

### SLR — Set PSW from R15
**Assembly**: `retsv` (offset=0) or `retsvd ea`  
**Operation**: PSW0 ← A15; optionally adds ea to restored PC

Loads the Program Status Word from A15 (used by SLT trap handlers to return to
user mode after modifying the saved PSW).

### LDK — Load Page Key
**Assembly**: `ldkey Rn, ea`  
**Operation**: Rn ← page key of the 512-word page containing ea

Reads the 8-bit storage key of the page containing address ea into register Rn.
The result is the key byte only (0x00–0xFF).

### STK — Store Page Key
**Assembly**: `stkey Rn, ea`  
**Operation**: Page key of the 512-word page containing ea ← Rn[7:0]

Sets the protection key for the page. Effective only in flat mode; in segmented
mode, keys reside in segment descriptors.

### LCT — Load Control Register
**Assembly**: `ldctl CRn, ea`  
**Operation**: CRn ← M[ea]; PC ← PC+1

Loads control register CRn from memory. Loading SDR (C5) flushes all segment and
TLB caches. Loading any control register requires key 0 (supervisor mode).

Assembler names for control registers: `psw0`, `psw1`, `fpc`, `plt`, `slt`,
`sdr`, `sflt`, `cr8`.

**Pseudo-instructions**: `pushcr CRn` expands to push SP pre-decrement store;
`popcr CRn` expands to pop-load.

### STCTL — Store Control Register
**Assembly**: `stctl CRn, ea`  
**Operation**: M[ea] ← CRn; PC ← PC+1

### LXRT — Load Real Address (Virtual→Physical Translation)
**Assembly**: `ldtrt Rn, ea`  
**Operation**: Translates the virtual address `ea` and stores the resulting physical
address in Rn. If translation fails (segment/page fault), C6(SFLT) is updated with
the fault information and PC advances by 1 (no skip). On success, PC advances by 2 (skip).

Used by the OS to probe virtual-to-physical mappings without triggering faults.

---

## I/O Instructions

I/O instructions are privileged (key 0 required). They address up to 4096 devices
(12-bit device number).

### RIO — Read I/O
**Assembly**: `rio Rn, func, device` or `rios Rn, func, device` (with ctl=1) or `rioc Rn, func, device` (ctl=2) or `riop Rn, func, device` (ctl=3)  
**Operation**: Rn ← device.read(func, ctl); PC ← PC+1

Reads data from the device. The `func` field (0–15, encoded in bits [15:12]) is
a device-specific function selector. The `ctl` field (encoded in bits [17:16])
modifies the operation:

| Suffix | ctl | Common meaning |
|--------|-----|----------------|
| (none) | 0 | Normal |
| `s` | 1 | Status/start |
| `c` | 2 | Control/clear |
| `p` | 3 | Poll/parameter |

### WIO — Write I/O
**Assembly**: `wio Rn, func, device` (with ctl suffixes as for RIO)  
**Operation**: device.write(Rn, func, ctl); PC ← PC+1

### NIO — No-operand I/O (Control Only)
**Assembly**: `nio device` (or `nios`, `nioc`, `niop`)  
**Operation**: device.control(ctl); PC ← PC+1

Issues a control operation to a device without reading or writing data.

### I/O Skip Instructions

Test a device status bit and skip based on the result:

| Mnemonic | Skip-when |
|----------|-----------|
| `tionb device` | device is busy |
| `tiobz device` | device is NOT busy |
| `tiond device` | device is done |
| `tiodn device` | device is NOT done |

These are pseudo-instructions built from the I/O instruction with transfer
field = 14 (skip test) and the ctl field encoding the condition.

---

## Local Trap Instructions

Local traps provide a fast kernel-call mechanism without full interrupt overhead.
They use the instruction word's upper 9 bits (bits 35–27) as an index into a
dispatch table.

### PLT — Problem Level Trap
**Opcode**: bits 35–27 in range 0200–0277 (octal)  
**Operation**: A14 ← instruction word; A15 ← PC+1; PC ← PLT_base + (opcode & 077)

Saves the instruction word in A14 and the return address in A15, then jumps to
an entry in the PLT table (C3). The key and privilege level are unchanged (remains
in user mode). If C3 bit 27 (valid) is 0, raises X_USER.

### SLT — Supervisor Level Trap
**Opcode**: bits 35–27 in range 0300–0377 (octal)  
**Operation**: A14 ← instruction word; A15 ← PSW0; PSW0[35:28] ← 0; PC ← SLT_base + (opcode & 077)

Saves the full PSW (including key) in A15, clears the key (entering supervisor
mode), and jumps to an SLT table entry. This provides a secure privilege
escalation path: user code triggers an SLT, the OS handler sees the original PSW
in A15 and the specific function index in A14's opcode bits.

Return from SLT: `retsv` restores PSW from A15 (including the original user key).
