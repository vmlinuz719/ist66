# RDC-700 Assembler Reference

The assembler (`tools/asm2.c`) is a single-pass assembler with forward-reference
resolution. It reads a source file and produces a relocatable binary in the RIM
(Read-In Mode) tape format suitable for loading with the bootstrap loader.

## Invocation

```
asm2 <source.a700> > output.rim
```

Output goes to stdout as a binary stream. Error messages go to stderr. Unresolved
forward references are reported at the end of assembly.

---

## Lexical Conventions

**Comments**: Start with `;` and extend to the end of the line.

**Symbols (labels)**: Up to 10 characters, case-sensitive. Must start with a letter
or underscore. Defined by appending `:` to the symbol name.

**Numbers**:
| Prefix | Base | Example |
|--------|------|---------|
| `0` (leading zero) | Octal | `0777` = 511 |
| `#` | Hexadecimal | `#1FF` = 511 |
| (none) | Decimal | `511` |
| Negative | Two's complement (any base) | `-1` = all ones |

**Strings**: Enclosed in `"..."`. Used only with the `ds` and `dsn` directives.

**Lists**: Multiple operands on the same line are separated by commas. The last
operand in a comma-separated list is followed by the instruction terminator
(end of line or next token).

---

## Label Definitions

```asm
label_name:  instruction_or_directive
```

or on a line by itself:

```asm
label_name:
```

A label definition sets the symbol's value to the current assembly address (PC).
Labels may be defined before or after the instructions that reference them; the
assembler resolves forward references during output.

---

## Directives

### `origin  address`

Sets the current assembly address (PC) to `address`. Emits a new relocation record.

```asm
origin      1024        ; start assembling at word address 1024 (octal 2000)
origin      #800        ; start at hex 800 = decimal 2048
```

### `bss  count`

Advances the assembly address by `count` words without emitting data. Used to
reserve storage.

```asm
buffer:     bss         256     ; reserve 256 words
```

### `dw  value [, value ...]`

Emits one or more 36-bit words with the given literal values.

```asm
table:      dw          0, 1, -1, 0777777777777
ptr:        dw          my_label                ; emit address of my_label
```

### `save  reg [, reg ...]`

Emits a register-save mask word for use with `calls`/`rets`. Each named register
sets the corresponding bit in the mask (bit `15 − n` for register An).

```asm
myfunc:     save        x0, x1, x2, lr     ; generates mask: bits for A3,A4,A5,A12
```

### `ds  item [, item ...]`

Emits a packed 7-bit ASCII byte string. Characters are packed 5 per 36-bit word,
starting from the most-significant bits. The string is not length-prefixed.

```asm
msg:        ds          "Hello, world!", 13, 10, 0
```

### `dsn  item [, item ...]`

Like `ds` but emits a length word first (count of bytes), then the packed bytes.
This produces the "length-data" string format used by `writeld` in the monitor.

```asm
msg:        dsn         "Ready.", 13, 10
```

### `reloc  address`

Sets the assembler's virtual PC (used for relative-address calculations) without
emitting a new relocation record. Used when code is assembled at one address but
will execute at another.

---

## Address Field Syntax

All memory-reference instructions take an address field. The address field
encodes the indirect bit, index register, and 18-bit displacement:

### Basic Forms

| Syntax | Mode | Description |
|--------|------|-------------|
| `n` | Absolute | Displacement = n, no index |
| `label` | Absolute | Displacement = address of label |
| `_n` | Direct page | Displacement = n, index = 1 (DP base) |
| `.n` or `.label` | PC-relative | Displacement = n or (label − PC), index = 2 |
| `n(Xk)` | Indexed | Displacement = n, index = Xk (X0–SP = A3–A13) |
| `+n` | Post-increment SP | Displacement = n, index = 14 |
| `=n` | Pre-decrement SP | Displacement = n, index = 15 |

### Indirect Addressing

Prefix any address form with `@` to make it indirect:

```asm
ld      @table_ptr          ; indirect: load M[M[table_ptr]]
ld      @(x0)               ; indirect indexed: load M[M[X0]]
ld      @+1                 ; indirect post-increment: load M[M[SP]], SP += 1
```

### Indexed Forms

The index register must be one of `x0`–`x7`, `ap`, `lr`, or `sp` (A3–A13).
Registers `ac`, `mq`, and `xy` (A0–A2) cannot be used as index registers.

```asm
ld      4(x0)               ; load M[X0 + 4]
st      -2(sp)              ; store to M[SP - 2]
ld      name(x1)            ; load M[X1 + address_of_name]
```

---

## Instruction Syntax Summary

### Memory Reference Instructions

```asm
jmp     ea                  ; unconditional jump
callr   ea                  ; call (LR = PC+1)
inctnz  ea                  ; ISZ: increment M[ea], skip if zero
dectnz  ea                  ; DSZ: decrement M[ea], skip if zero
tstmz   ea                  ; SZR: skip if M[ea] = 0
tstmnz  ea                  ; SNZ: skip if M[ea] ≠ 0
calls   ea                  ; CALL: save registers and call (mask at ea)
rets                        ; RET: restore and return (no stack adjustment)
retsd   n                   ; RET n: restore and return, discard n stack words
```

### Accumulator–Memory Instructions

```asm
edit    Rn, ea              ; OR M[ea] with Rn, execute result
edits   Rn, ea              ; same, plus skip after
ldea    [Rn,] ea            ; Rn ← ea (effective address)
addea   [Rn,] ea            ; Rn ← Rn + ea
inctne  [Rn,] ea            ; Rn++; skip if Rn = M[ea]
dectne  [Rn,] ea            ; Rn--; skip if Rn = M[ea]
ldeas   [Rn,] ea            ; Rn ← ea << 17
ldcom   [Rn,] ea            ; Rn ← ~M[ea]
ldneg   [Rn,] ea            ; Rn ← -M[ea]
ld      [Rn,] ea            ; Rn ← M[ea]
pop     Rn                  ; Rn ← M[SP], SP += 1
st      [Rn,] ea            ; M[ea] ← Rn
push    Rn                  ; SP -= 1, M[SP] ← Rn
addcom  [Rn,] ea            ; Rn ← Rn + ~M[ea]
sub     [Rn,] ea            ; Rn ← Rn - M[ea]
add     [Rn,] ea            ; Rn ← Rn + M[ea]
and     [Rn,] ea            ; Rn ← Rn & M[ea]
or      [Rn,] ea            ; Rn ← Rn | M[ea]
xor     [Rn,] ea            ; Rn ← Rn ^ M[ea]
```

### Multiply/Divide Instructions

```asm
mul     ea                  ; A2:A0 ← A1 × M[ea] (signed)
fmadd   ea                  ; A2:A0 ← A2:A0 + A1 × M[ea] (signed)
fmsub   ea                  ; A2:A0 ← A2:A0 + A1 × (-M[ea]) (signed)
div     ea                  ; A1 ← A0 / M[ea], A2 ← A0 mod M[ea] (signed)
umul    ea                  ; A2:A0 ← A1 × M[ea] (unsigned)
ufmadd  ea                  ; A2:A0 ← A2:A0 + A1 × M[ea] (unsigned)
ufmsub  ea                  ; A2:A0 ← A2:A0 + A1 × (-M[ea]) (unsigned)
udiv    ea                  ; A1 ← A0 / M[ea], A2 ← A0 mod M[ea] (unsigned)
```

### Byte-Field Instructions

```asm
ldb     Rn, Rx, size        ; Rn ← character at Rx pointer (size bits)
stb     Rn, Rx, size        ; store character from Rn at Rx pointer
incbx   Rn, Rx, size        ; Rn ← advance pointer by size bits (copy to Rn)
incldb  Rn, Rx, size        ; advance Rx pointer, load character into Rn
incstb  Rn, Rx, size        ; advance Rx pointer, store Rn character
```

### System Management Instructions

```asm
hlt                         ; halt processor (stop code from AC)
wait    [Rn,] ea            ; halt, stop code = Rn, PC = ea
intr    Rn, ea              ; software interrupt at level Rn, PC = ea
reti                        ; return from interrupt
retid   n                   ; return from interrupt, PC += n
retlmi  ea                  ; load mask from M[ea], then reti
ldmask  ea                  ; load interrupt mask
stmask  ea                  ; store interrupt mask
lmwait  ea                  ; load mask and halt (atomic)
invlsg  ea                  ; invalidate segment cache entry
invlpg  ea                  ; invalidate TLB entry
retsv                       ; restore PSW from R15 (SLT return)
retsvd  ea                  ; restore PSW from R15, advance by ea
ldkey   Rn, ea              ; Rn ← page protection key of page containing ea
stkey   Rn, ea              ; set page protection key of page containing ea
ldctl   CRn, ea             ; CRn ← M[ea] (load control register)
stctl   CRn, ea             ; M[ea] ← CRn (store control register)
pushcr  CRn                 ; push control register on stack
popcr   CRn                 ; pop control register from stack
ldtrt   Rn, ea              ; Rn ← physical address of ea; skip on success
pushim                      ; push interrupt mask on stack
popim                       ; pop interrupt mask from stack
```

### I/O Instructions

```asm
rio[s|c|p]  Rn, func, dev  ; read from device dev, function func, into Rn
wio[s|c|p]  Rn, func, dev  ; write Rn to device dev, function func
nio[s|c|p]  dev             ; control device dev (no data transfer)
tionb   dev                 ; skip if device busy
tiobz   dev                 ; skip if device NOT busy
tiond   dev                 ; skip if device done
tiodn   dev                 ; skip if device NOT done
```

The `s`, `c`, `p` suffixes on RIO/WIO/NIO set the control field to 1, 2, 3
respectively (default = 0).

### Floating-Point (Memory) Instructions

```asm
ldf[n][r]   [f0..f3,] ea    ; load F36 from M[ea]
stf[n][r]   [f0..f3,] ea    ; store F36 to M[ea]
adf[n][r]   [f0..f3,] ea    ; freg += F36(M[ea])
sbf[n][r]   [f0..f3,] ea    ; freg -= F36(M[ea])
mlf[n][r]   [f0..f3,] ea    ; freg *= F36(M[ea])
dvf[n][r]   [f0..f3,] ea    ; freg /= F36(M[ea])

ldg[n][r]   [f0..f3,] ea    ; load F72 from M[ea]:M[ea+1]
stg[n][r]   [f0..f3,] ea    ; store F72 to M[ea]:M[ea+1]
adg[n][r]   [f0..f3,] ea    ; freg += F72(M[ea]:M[ea+1])
sbg[n][r]   [f0..f3,] ea    ; freg -= F72
mlg[n][r]   [f0..f3,] ea    ; freg *= F72
dvg[n][r]   [f0..f3,] ea    ; freg /= F72

ldexp       [f0..f3,] ea    ; freg.exp ← M[ea] (signed)
stexp       [f0..f3,] ea    ; M[ea] ← freg.exp - 16383
ldsig       [f0..f3,] ea    ; freg.sig ← M[ea]:M[ea+1] (72-bit)
stsig       [f0..f3,] ea    ; M[ea]:M[ea+1] ← freg.sig
```

Modifiers: `n` = normalize after, `r` = round after normalize.

### Floating-Point (Register) Instructions

```asm
mvl[n][f|g][k][.cond]  src, tgt [, dst]    ; copy float register
ngl[n][f|g][k][.cond]  src, tgt [, dst]    ; negate
adl[n][f|g][k][.cond]  src, tgt [, dst]    ; add
sbl[n][f|g][k][.cond]  src, tgt [, dst]    ; subtract
mll[n][f|g][k][.cond]  src, tgt [, dst]    ; multiply
dvl[n][f|g][k][.cond]  src, tgt [, dst]    ; divide
```

Modifiers: `n` = normalize, `f` = round to F36, `g` = round to F72, `k` = no-write.
When two registers are given (no dst), dst = tgt.

### ALU Register Instructions

See [alu.md](alu.md) for a comprehensive description. Common forms:

```asm
{op}r[t][r][z|s|c][n][.cond]  src, dst [, mask [, rotate]]
{op}i[z|s|c][n][.cond]        src, dst, immediate
{op}s[r][z|s|c][n][.cond]     src, tgt, dst [, amount]
cmpne/cmp/cmplt/cmpge/cmpgt/cmple  src, tgt
```

---

## Output Format (RIM Tape)

The assembler emits a binary stream in RIM (Read-In Mode) format. Each contiguous
block of assembled code becomes one record:

```
[3 bytes: base address, 6 bits each]
[6 bytes per word: 36 bits in 6-bit groups, MSB first]
...
[0x80: block terminator]
```

Multiple records may appear for non-contiguous code sections. The final record in
the stream has its base address equal to the entry point (the last `origin` value),
which signals the bootstrap loader to jump to that address after loading.

The bootstrap loader (`rimldr`) reads this format from a paper-tape reader (device
0x00A) and loads words directly into physical memory.

---

## Programming Conventions (Monitor ABI)

The monitor establishes the following calling convention:

**Register usage**:
- `ac`, `mq`, `xy` — caller-saved scratch; `ac` holds return value
- `x0`–`x3` — first four integer arguments (caller-saved)
- `x4`–`x7` — caller-saved scratch
- `ap`, `lr`, `sp` — callee-saved (preserve across calls)
- `r14`, `r15` — reserved for trap mechanism

**Call/return**:
- `calls .func` — push frame and call; first word at label is the `save` mask
- `rets` — restore and return
- `callr .func` — lightweight call via LR (no register save)
- `retr` — return via LR (for callr-called functions)

**Stack direction**: grows downward (SP decrements on push).

**String format**: Length-data strings (`dsn`): first word holds byte count,
followed by packed 7-bit ASCII bytes (5 per word, MSB first).

**Example subroutine**:
```asm
; MYFUNC - does something
;   X0: input value
;   Return: result in AC

myfunc:     save    x1, x2, lr      ; save mask (generated by assembler)

            ; ... function body ...
            movr    x1, ac          ; set return value

            rets
```
