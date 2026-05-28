# RDC-700 ALU Instruction Detail

## Pipeline

Each ALU register instruction executes the following pipeline in order:

1. **Carry initialization** — the carry input `c` is set from `CF` per the CI field
2. **Operation** — compute result and update carry from source/destination registers
3. **Rotate** — rotate the 36-bit (or 37-bit if `t`) result by the rotation count
4. **Mask** — fill top or bottom bits with the carry value
5. **Skip test** — set skip bit if skip condition is met
6. **Write-back** — store result to destination register (unless NW=1)
7. **CF update** — carry flag in PSW ← carry bit from step 4/5 result

## Carry Initialization (CI Field)

| CI | Assembler modifier | Effect on `c` before operation |
|----|-------------------|-------------------------------|
| 0 | (none) | `c` = current CF |
| 1 | `z` | `c` = 0 |
| 2 | `s` | `c` = 1 |
| 3 | `c` | `c` = ~CF (invert) |

## Operation Details

All operations are performed on 36-bit values. "Carry out" means the result of
modifying `c` during the operation.

| Op | Name | Result | Carry out |
|----|------|--------|-----------|
| 0 | com | ~Src | unchanged |
| 1 | ngt | −Src (two's complement: ~Src + 1) | unchanged |
| 2 | mov | Src | unchanged |
| 3 | inc | Src + 1 | flipped if Src = 0xFFFFFFFFF (all ones) |
| 4 | adc | Dst + ~Src = Dst − Src − 1 | flipped if Src < Dst |
| 5 | sub | Dst + ~Src + 1 = Dst − Src | flipped if Src ≤ Dst |
| 6 | add | Dst + Src | flipped if Dst + Src > 0xFFFFFFFFF |
| 7 | and | Dst & Src | unchanged |
| 8 (octal 10) | bis | Dst \| Src | unchanged |
| 12 (octal 14) | xor | Dst ⊕ Src | unchanged |
| 13 (octal 15) | pct | popcount(Src) | unchanged |

The "carry out" description shows when the internal carry variable `c` is *flipped*
from its initialized value. The carry that ends up stored in CF after the
instruction is the value of `c` after all modifications.

**Note on `sub` and `adc`**: These implement subtract-with-borrow semantics common
in 36-bit minicomputer design. Using CI=`z` (force carry 0) before `sub` gives
a clean subtraction. Using the default CI (carry from previous) chains borrows
across multi-precision subtractions.

## Rotate and Mask

After the operation, the result passes through a rotate-then-mask pipeline
(R-type and S-type only; not applicable to immediate mode).

### Rotation

The rotation count is a signed 6-bit value. Positive = rotate left; negative =
rotate right. With the `t` modifier (rotate-through-carry), the 37-bit value
(result + carry bit at position 36) is rotated as a unit instead of rotating only
the 36-bit result.

```
Without t: rotate 36-bit result; carry bit is preserved unchanged
With    t: rotate 37-bit (carry:result) as one unit
```

Direction: left by default; the `r` modifier negates the count, producing a right
rotation.

### Mask

The mask operation fills a contiguous run of bits with copies of the carry bit.
The mask count is a signed 6-bit value:

- **Positive mask `m`**: The leftmost `m` bits of the result are replaced by the
  carry value. (Fills from the MSB downward.)
- **Negative mask `−m`**: The rightmost `m` bits are replaced by the carry value.
  (Fills from the LSB upward.)

This allows efficient sign-extension, zero-extension, and field isolation when
combined with rotation. For example, to extract bits [17:4] of a register into the
low 14 bits of the destination with zero-extension:

```asm
; Extract bits [17:4], zero-extend into dest
movrs.z     src, dest, dest, 32     ; S-type: logical right-shift 32? No...
; More precisely, use R-type:
movrz       src, dest, -14, 18     ; rotate right 18, mask low 14 bits (fill rest with carry=0)
```

### S-type: Coupled Shift

In S-type mode (`mode=1, submode=0`), the mask count is automatically set to
`−rotation_count`. This couples them into a single logical shift operation:
rotating right by N and then filling the top N bits with carry (usually 0 with
CI=`z`) produces a logical right shift.

The S-type encoding also provides an alternate destination register (ADR, bits
9–6), so `src`, `tgt`, and `dst` can all be different registers.

## Skip Conditions

The skip test examines the 37-bit result (36-bit data + carry bit at bit 36):

| Code | Suffix | Condition tested | Skip when |
|------|--------|-----------------|-----------|
| 0 | `.no` | — | never |
| 1 | `.sk` | — | always |
| 2 | `.cn` | carry bit (bit 36) | NOT set |
| 3 | `.cz` | carry bit (bit 36) | set |
| 4 | `.rn` | data bits [35:0] | = 0 |
| 5 | `.rz` | data bits [35:0] | ≠ 0 |
| 6 | `.bn` | result=0 OR carry not set | (see below) |
| 7 | `.bz` | result≠0 AND carry set | (see below) |

**Condition `.bn`** skips when: data = 0 or carry = 0. Execute-when: data ≠ 0 AND carry set.  
**Condition `.bz`** skips when: data ≠ 0 and carry set. Execute-when: data = 0 OR carry not set.

These combined conditions are useful for detecting unsigned overflow or underflow
in arithmetic sequences.

## Common Idioms

```asm
xorr    ac, ac              ; clear AC (XOR with self = 0); no carry change
incr    ac, ac              ; AC ← AC + 1
movr.rn ac, ac              ; test AC; skip next if AC = 0 (branch-if-zero)
movr.rz ac, ac              ; test AC; skip next if AC ≠ 0 (branch-if-nonzero)
addi.rn x0, x0, -1         ; decrement and loop: skip when x0 reaches 0

; Multi-precision add: 64-bit in (A1:A0) + (A3:A2) → (A1:A0)
addrs   a0, a2, a0          ; low 36 bits; carry captures overflow
addrs.cn a1, a3, a1         ; high 36 bits with carry-in

; Logical right-shift by 4:
movrrs.z  src, dest, dest, 4    ; S-type, right, CI=0, shift=4

; Sign-extend from bit 11 to 36 bits:
movr    src, dest, -24, 12  ; rotate right 12, fill top 24 with carry from bit 11
; (requires careful carry setup — load sign bit into carry first)
```

## Rotate-and-mask Examples

To extract a 9-bit field from bits [17:9] of a register into the low 9 bits
of the destination, zero-extended:

```asm
movrz   src, dst, 9, 18     ; R-type, CI=0, mask=+9 (fill top 27 with 0), rotate-right 18
```

Step by step:
1. CI=z: carry ← 0
2. op=mov: result ← src (36 bits)
3. rotate right 18: bits [17:0] of src are now at bits [35:18]
4. mask +9: top 9 bits → carry (0); net: 27 zeros in top, 9 bits from [17:9] in bits [8:0]

To pack the low 9 bits of a register into bits [17:9] of the destination (OR into place):

```asm
bisrs   src, dst, dst, -9   ; S-type bis, right... actually use R-type:
bisr    src, dst, -9, 9     ; rotate left 9, mask bottom 9 with carry
```

Experimentation with the assembler and sample programs is the best guide to
mastering the rotate/mask pipeline.
