# RDC-700 Floating-Point Architecture

## Floating-Point Formats

The RDC-700 uses a binary floating-point representation common to all three
precision levels. All formats use an explicit leading 1 in the significand (no
hidden bit). The exponent is biased by 127 (excess-127), not IEEE 754's
format-dependent biases.

### Single-Precision (F36): 36-bit

Stored as one 36-bit memory word:

```
 35  34     27  26                                           0
┌───┬─────────┬─────────────────────────────────────────────┐
│ S │ Exp[7:0]│             Significand [26:0]               │
└───┴─────────┴─────────────────────────────────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35 | S | Sign (0=positive, 1=negative) |
| 34–27 | Exp | 8-bit exponent, biased by 127 (excess-127) |
| 26–0 | Sig | 27-bit significand (explicit leading 1 is bit 26 for normalized values) |

Value = (−1)^S × 2^(Exp−127) × Sig/2^26

Exponent range: Exp 1–254 → values 2^(−126) to 2^(127).

### Double-Precision (F72): 72-bit

Stored as two consecutive 36-bit words (high word first):

```
Word 0 (high):  S[1] | Exp[8] | Sig[27] (same layout as F36)
Word 1 (low):   Sig[35:0] (additional significand bits)
```

Total significand width: 27 + 36 = 63 bits explicit. (The leading 1 is bit 26
of word 0; word 1 provides 36 additional fraction bits below it.)

### Internal Precision (F80): 80-bit

All floating-point registers hold values in an 80-bit internal format:

- **Sign**: 1 bit (bit 15 of `sign_exp` field)
- **Exponent**: 15 bits, biased by 16383 (excess-16383)
- **Significand**: 64 bits (explicit leading 1 at bit 63 for normalized values)

Arithmetic is performed at 80-bit precision; results are rounded to F36 or F72
when stored to memory by `stf`/`stg`.

### Special Values

| Representation | Interpretation |
|---------------|----------------|
| Exp=0, Sig=0, Sign=0 | Zero (positive) |
| Exp=0, Sig=0, Sign=1 | NaN |
| Exp=0, Sig≠0, Sign=0 | Pseudo-NaN |
| Exp=0, Sig≠0, Sign=1 | NaN |
| Exp=255 (0xFF), any | ±Infinity |
| Any Exp, Sig=0 | Unnormalized zero |
| Normal range | Numeric value |

**Note**: Unlike IEEE 754, there are no "quiet" vs. "signaling" NaN distinctions;
any NaN input to an arithmetic operation sets the F_ILGL status bit and produces
a NaN output.

---

## Status Flags

FP instructions accumulate status bits into A2 (the `xy` register) by OR-ing the
result of each operation into it. Software is responsible for clearing A2 before
a sequence of FP operations and checking it afterward.

| Bit | Constant | Meaning |
|-----|----------|---------|
| 0 | F_OVRF | Overflow: result exceeded representable range; infinity produced |
| 1 | F_UNDF | Underflow: result too small; zero produced |
| 2 | F_INSG | Insignificant: one operand was so much smaller than the other that it contributed nothing (conormalization dropped it) |
| 3 | F_ILGL | Illegal argument: NaN input, 0×∞, or 0÷0 |

---

## Float Register Bank

The 16 FP registers (F0–F15) are divided into four banks of four. The two low bits
of the FPU Control Word (C2, `fpc`) select the active bank:

| fpc[1:0] | Visible as f0–f3 |
|----------|-----------------|
| 0 | F0–F3 |
| 1 | F4–F7 |
| 2 | F8–F11 |
| 3 | F12–F15 |

To enable the FPU, bit 2 of `fpc` must be set. Loading `fpc` with 4 enables the
FPU in bank 0. If bit 2 is clear, any FP instruction raises exception X_NFPU (9).

---

## Float-Memory Instructions (FM)

These load or operate on float values between memory and the active float register
bank. They share the memory-reference address field (I, index, displacement) and
encode the float register in bits [24:23].

### Assembler Syntax

```
{mnemonic}[n][r]  [freg,] ea
```

Optional modifiers appended to the mnemonic (in order):
- `n` — normalize result after the operation
- `r` — round result after normalize (rounds toward nearest; sets F_OVRF/F_UNDF if needed)

The float register `freg` is `f0`–`f3` (assembler names). When omitted, `f0` is used.

### F36 (Single-Precision) Instructions

| Mnemonic | Operation | Description |
|----------|-----------|-------------|
| `ldf[n][r]` | freg ← F36(M[ea]) | Load single-precision float |
| `stf[n][r]` | M[ea] ← F36(freg) | Store single-precision float |
| `adf[n][r]` | freg ← freg + F36(M[ea]) | Add single-precision |
| `sbf[n][r]` | freg ← freg − F36(M[ea]) | Subtract single-precision |
| `mlf[n][r]` | freg ← freg × F36(M[ea]) | Multiply single-precision |
| `dvf[n][r]` | freg ← freg ÷ F36(M[ea]) | Divide single-precision |

All operations are performed at internal 80-bit precision. The `n` modifier
normalizes the result (shifts significand left until the leading bit is in the
MSB). The `r` modifier then rounds to the target precision (F36 for these).
Status bits accumulate into A2.

### F72 (Double-Precision) Instructions

| Mnemonic | Operation | Description |
|----------|-----------|-------------|
| `ldg[n][r]` | freg ← F72(M[ea]:M[ea+1]) | Load double-precision float |
| `stg[n][r]` | M[ea]:M[ea+1] ← F72(freg) | Store double-precision float |
| `adg[n][r]` | freg ← freg + F72(M[ea]:M[ea+1]) | Add double-precision |
| `sbg[n][r]` | freg ← freg − F72(M[ea]:M[ea+1]) | Subtract double-precision |
| `mlg[n][r]` | freg ← freg × F72(M[ea]:M[ea+1]) | Multiply double-precision |
| `dvg[n][r]` | freg ← freg ÷ F72(M[ea]:M[ea+1]) | Divide double-precision |

Double-precision operations read/write two consecutive words. The store (`stg`)
instruction rounds to F72 precision if `r` is specified.

### Exponent and Significand Instructions

These access the internal components of a float register directly, bypassing the
normal floating-point pipeline.

| Mnemonic | Operation | Description |
|----------|-----------|-------------|
| `ldexp freg, ea` | freg.exponent ← M[ea] (signed 36-bit, biased internally) | Load exponent |
| `stexp freg, ea` | M[ea] ← freg.exponent − 16383 (unbiased) | Store exponent |
| `ldsig freg, ea` | freg.significand ← M[ea]:M[ea+1] (72-bit signed integer) | Load significand |
| `stsig freg, ea` | M[ea]:M[ea+1] ← freg.significand (72-bit signed integer) | Store significand |

The `ldexp` instruction accepts a signed 36-bit two's-complement integer and
converts it to the internal biased exponent (adding 16383). Out-of-range exponents
set F_OVRF or F_UNDF in A2.

The `ldsig` and `stsig` instructions work with the 72-bit significand as a
two's-complement signed integer (two consecutive words, MSW first). This allows
integer↔float conversion: load a 36-bit integer using `ldexp` (to set the
exponent) and `ldsig` (to set the significand), then normalize.

---

## Float Register-to-Register Instructions (FR)

These perform floating-point operations between two or three float registers,
entirely within the register file.

### Assembler Syntax

```
{mnemonic}[n][f|g][k][.condition]  src, tgt [, dst]
```

Modifiers:
- `n` — normalize result
- `f` — round to F36 precision after normalize
- `g` — round to F72 precision after normalize
- `k` — suppress write: compute but do not update `dst`

Skip condition (from `.{test}`):

| Test | Condition for normal execution (no skip) | Skip-when |
|------|------------------------------------------|-----------|
| `.no` | (never skip) | never |
| `.sk` | (always skip) | always |
| `.lz` | result is negative (less than zero) | NOT negative (positive or zero) |
| `.zg` | result is positive or zero (not negative) | negative |
| `.rn` | result is nonzero | result = 0 |
| `.rz` | result is zero | result ≠ 0 |
| `.if` | result is infinite | NOT infinite |
| `.nn` | result is NaN | NOT NaN |

When two arguments are given (`src, tgt`), `dst` defaults to `tgt`.

### Operations

| Mnemonic | Operation | Description |
|----------|-----------|-------------|
| `mvl[n][f\|g][k][.cond]` | dst ← src | Move (copy) float register |
| `ngl[n][f\|g][k][.cond]` | dst ← −src | Negate float register |
| `adl[n][f\|g][k][.cond]` | dst ← src + tgt | Float register add |
| `sbl[n][f\|g][k][.cond]` | dst ← src − tgt | Float register subtract |
| `mll[n][f\|g][k][.cond]` | dst ← src × tgt | Float register multiply |
| `dvl[n][f\|g][k][.cond]` | dst ← src ÷ tgt | Float register divide |

Status bits from the operation accumulate into A2.

**Example — Mandelbrot inner loop using double-precision**:
```asm
ldg     f0, (x0)        ; load x (double)
mlgn    f0, (x0)        ; f0 ← x² (normalize)
ldg     f1, 2(x0)       ; load y (double)
mlgn    f1, 2(x0)       ; f1 ← y² (normalize)
sblng   f0, f1, f0      ; f0 ← x² - y² (normalize, double round)
adgnr   f0, 6(x0)       ; f0 ← f0 + cx (double, normalize, round)
stg     f0, 4(x0)       ; store xx temp

adlng   f0, f1, f2      ; f2 ← x² + y² (for escape test)
```

---

## Float Arithmetic Rules

### Addition

Conormalization aligns the lesser-magnitude operand to the greater's exponent by
right-shifting its significand. If the exponent difference exceeds 64, the lesser
operand is treated as zero (F_INSG status set). After alignment, the significands
are added or subtracted (depending on sign agreement).

### Multiplication

The 64-bit × 64-bit significand product is computed to 128 bits and rounded to
64 bits. The result exponent is Exp_src + Exp_tgt + normalization_adjustment − 16383.

### Division

The dividend significand is left-shifted to 128 bits and divided by the divisor's
64-bit significand. The result is rounded to 64 bits.

### Special Cases

| Operation | Result |
|-----------|--------|
| NaN op anything | NaN, F_ILGL |
| 0 × ∞ or ∞ × 0 | NaN, F_ILGL |
| n ÷ 0 | ±Infinity, F_ILGL |
| ∞ ± ∞ (same sign) | ±Infinity |
| ∞ − ∞ (opposite sign) | Zero |
| 0 × n | Zero |
| n × ∞ | ±Infinity, F_OVRF |

---

## Integer/Float Conversion

The RDC-700 has no dedicated integer-to-float or float-to-integer instruction.
Conversion is done manually:

**Integer → Float**: Load the integer into memory, then use `ldf`/`ldg` with the
appropriate exponent adjustment. The pattern in the monitor programs uses:

```asm
; Convert integer in XY to float in f0 (single-precision)
or      xy, .int_to_f      ; set exponent bits (0231000000000 = Exp for 2^17)
push    xy
ldfn    f0, +1             ; load and normalize
```

The constant `0231000000000` encodes Exp=0231 octal (153 decimal, bias 127, giving
actual exponent 26). Loading a 27-bit integer into the significand bits and
normalizing produces the correct float value.

**Float → Integer**: Store the float at a known precision and extract the significand
and exponent fields separately, then shift.
