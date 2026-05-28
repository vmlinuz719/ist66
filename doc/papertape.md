# RDC-700 Paper Tape Reader and Punch

The paper tape reader (`ppt.c`) and punch (`pch.c`) are byte-at-a-time
peripheral devices backed by host files. The reader reads one byte per
operation from a file (default: stdin) and places it in a one-byte buffer.
The punch writes one byte per operation to a file (default: stdout).
Both simulate hardware timing with a per-operation delay and use the interrupt
system to signal completion.

---

## Paper Tape Reader (PPT)

### Initialization

```c
init_ppt(cpu, id, irq);          /* read from stdin */
init_ppt_ex(cpu, id, irq, fname); /* read from named file (binary mode) */
```

| Parameter | Description |
|-----------|-------------|
| `cpu` | Pointer to the CPU structure |
| `id` | Device number (12-bit) |
| `irq` | Interrupt level to assert when a byte is ready |
| `fname` | Path to the tape image file |

### I/O Register Model

| Instruction | Operation |
|-------------|-----------|
| `rio Rn, 0, dev` | Read the last byte fetched into Rn |
| `nios dev` | Start reading the next byte (ctl=1, no data) |
| `rios Rn, 0, dev` | Read last byte into Rn **and** start reading the next |
| `nioc dev` | Clear done flag and release interrupt (ctl=2) |
| `tiond dev` | Skip if done (byte ready) |
| `tiodn dev` | Skip if not done |
| `tionb dev` | Skip if busy (read in progress) |
| `tiobz dev` | Skip if not busy |

**Timing**: each read operation takes approximately 2 ms (500 bytes/second),
simulating a real paper tape reader.

**End of tape**: when `fgetc` returns EOF, the device stops and sets the
buffer to 0. A message is printed to stderr:
```
PPT: 0012 End of tape
```
No interrupt is asserted at end of tape. The busy flag clears and the done
flag is set as normal; subsequent reads return 0.

### Status Word

Status is returned by the skip-test instructions (transfer=14).

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | Busy | A read is in progress |
| 1 | Done | A byte is ready in the buffer; interrupt asserted |

The interrupt is cleared by ctl=1 (which also starts the next read) or ctl=2
(which only clears the interrupt). Only one interrupt is outstanding at a time.

### Programming Pattern

**Polled single-byte read**:

```asm
; Start a read and wait for completion
            nios    PPT_DEV         ; start read (ctl=1)
rd_wait:    tiodn   PPT_DEV         ; skip if done
            jmp     .rd_wait
            rio     ac, 0, PPT_DEV  ; fetch byte into AC
```

**Chained read** (read current byte and immediately start the next):

```asm
; Used in the RIM loader: consume and overlap reads
            rios    ac, 0, PPT_DEV  ; fetch byte, start next read simultaneously
```

**Interrupt-driven read** (interrupt fires when each byte is ready):

```asm
; Interrupt handler — reads byte and starts next
ppt_intr:   rios    ac, 0, PPT_DEV  ; read byte, start next, clear interrupt
            ; process byte in AC ...
            reti
```

### Bootstrap Use (RIM Loader)

The RIM loader (`rimldr.a700`) uses device #00A (octal) for the bootstrap
paper tape reader. It initialises the reader with `nios`, discards leader
bytes (0x80), then accumulates 6-bit groups into words using the BX pipeline:

```asm
rimldr:     nios    #00a            ; start first read

rd_wait:    tiond   #00a            ; wait for byte
            jmp     .rd_wait

            rios    xy, 0, #00a     ; read byte, start next
            addin.rz xy, xy, -128  ; skip if byte is leader (0x80)
            jmp     .get_addr       ; discard and restart

            movrtz  ac, ac, 6       ; accumulate 6 bits into AC
            bisr.cz xy, ac          ; OR byte into accumulator
            jmp     .rd_wait        ; get next 6-bit group
```

---

## Paper Tape Punch (PCH)

### Initialization

```c
init_pch(cpu, id, irq);          /* write to stdout */
init_pch_ex(cpu, id, irq, fname); /* write to named file (binary mode) */
```

Parameters have the same meaning as for the reader. The output file is opened
in binary write mode.

### I/O Register Model

| Instruction | Operation |
|-------------|-----------|
| `wio Rn, 0, dev` | Load byte from Rn into output buffer |
| `wios Rn, 0, dev` | Load byte **and** start punching (ctl=1) |
| `nioc dev` | Clear done flag and release interrupt (ctl=2) |
| `tiond dev` | Skip if done (punch complete) |
| `tiodn dev` | Skip if not done |
| `tionb dev` | Skip if busy (punch in progress) |
| `tiobz dev` | Skip if not busy |

**Timing**: each punch operation takes approximately 16 ms (62.5 bytes/second),
simulating a mechanical tape punch.

There is no read function (func=0 for RIO returns 0). All data flows outward.

### Status Word

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | Busy | A punch is in progress |
| 1 | Done | Punch complete; interrupt asserted |

Same interrupt semantics as the reader: one outstanding interrupt, cleared by
ctl=1 (start next) or ctl=2 (clear only).

### Programming Pattern

**Polled single-byte punch**:

```asm
; Wait until ready, then punch byte in AC
pch_wait:   tiobz   PCH_DEV         ; skip if NOT busy
            jmp     .pch_wait
            wios    ac, 0, PCH_DEV  ; load byte and start punch
```

**Interrupt-driven punch** (interrupt fires when punch is complete, ready for
the next byte):

```asm
; Interrupt handler — punch next byte if available
pch_intr:   ; load next byte into AC from buffer ...
            wios    ac, 0, PCH_DEV  ; punch it and clear interrupt
            reti
```

---

## Emulator Diagnostics

```
PPT: 0012 IRQ 006, file tape.rim     ; reader init (or "file STDIN")
PPT: 0012 End of tape                ; EOF reached
PPT: 0012 deinitialized

PCH: 0013 IRQ 007, file out.rim      ; punch init (or "file STDOUT")
PCH: 0013 deinitialized
```

Device and IRQ numbers are printed in octal.
