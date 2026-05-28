# RDC-700 Line Printer (LPT)

The line printer device (`lpt.c`) is a character-at-a-time output peripheral
backed by a host file. It accumulates characters into an internal 132-character
line buffer and flushes it to the file when a line-ending character arrives or
the buffer fills. The interrupt fires only on line flush, not on every character;
the busy flag clears after every character regardless.

---

## Initialization

```c
init_lpt(cpu, id, irq, fd);           /* write to an already-open FILE* */
init_lpt_ex(cpu, id, irq, fname);     /* open named file (binary write) */
```

| Parameter | Description |
|-----------|-------------|
| `cpu` | Pointer to the CPU structure |
| `id` | Device number (12-bit) |
| `irq` | Interrupt level to assert after each line flush |
| `fd` / `fname` | Output file handle or path |

---

## I/O Register Model

### Function Numbers

| Instruction | Operation |
|-------------|-----------|
| `wio Rn, 0, dev` | Load byte from Rn into output buffer |
| `wios Rn, 0, dev` | Load byte **and** start printing (ctl=1) |
| `rio Rn, 0, dev` | Read busy status into Rn (bit 0 = busy) |
| `nioc dev` | Clear done flag and release interrupt (ctl=2) |

There is no second function register; all data I/O uses func=0.

### Control Actions

| Suffix | ctl | Action |
|--------|-----|--------|
| `s` | 1 | **Start**: accept the buffered byte, print it, assert interrupt on line flush |
| `c` | 2 | **Clear**: clear done flag and release interrupt without printing |

### Status and Skip Tests

| Instruction | Condition | Skip when |
|-------------|-----------|-----------|
| `tionb dev` | Busy | Print operation in progress |
| `tiobz dev` | Not busy | Ready for next character |
| `tiond dev` | Done | Line has been flushed; interrupt asserted |
| `tiodn dev` | Not done | No interrupt pending |

**Busy** (bit 0): set while the print thread is processing a character.
Clears when the thread finishes, whether or not a flush occurred.

**Done** (bit 1): set only when a line flush completes (see below). Cleared
by ctl=1 or ctl=2.

---

## Line Buffer and Flush Behavior

Each printed character is appended to a 132-byte internal line buffer. The
buffer flushes — and the interrupt fires — when any of the following occurs:

| Condition | Character (octal) | Action |
|-----------|-------------------|--------|
| Buffer full | — | Accumulated 132 characters |
| Carriage return | 015 (0x0D) | Flush including the CR |
| Line feed | 012 (0x0A) | Flush including the LF |
| Form feed | 014 (0x0C) | Flush including the FF |

On flush:
1. The accumulated bytes (including the terminating character) are written to
   the output file in one `fwrite` call.
2. If the buffer reached exactly 132 characters without a line-ending, an
   additional newline (`\n`) is appended to the file.
3. The thread sleeps 4 ms to simulate print time (≈250 lines/second).
4. The interrupt is asserted.

Characters that do not trigger a flush complete immediately (no sleep) and do
not assert the interrupt. Software must poll `tiobz` between characters to
avoid overrunning the device; it need not wait for `tiond` unless it wants to
know that a full line was accepted.

---

## Programming Patterns

### Polled Character Output

Wait for the printer to be idle, load the byte, and start:

```asm
; Print byte in AC (polled)
print_byte:
pb_wait:    tiobz   LPT_DEV         ; skip if NOT busy
            jmp     .pb_wait
            wios    ac, 0, LPT_DEV  ; load byte and start
            retr
```

### Print a Null-Terminated String

```asm
; print_str: print null-terminated string
;   X0: BX pointer initialised to one before the first character
;   X1: device number (patched into EDITS instruction)

print_str:
ps_loop:    tiobz   LPT_DEV         ; wait until not busy
            jmp     .ps_loop

            incldb  ac, x0, 7       ; advance pointer, load 7-bit byte
            movr.rz ac, ac          ; skip body if byte is zero (end)
            jmp     .ps_done

            wios    ac, 0, LPT_DEV  ; print byte and start
            jmp     .ps_loop

ps_done:    retr
```

### Interrupt-Driven Line Output

The interrupt fires after each line flush. Use it to pipeline line printing
while the CPU continues other work:

```asm
; Interrupt handler — signal foreground that the printer accepted a line
lpt_intr:   nioc    LPT_DEV         ; clear interrupt
            st      .tty_acsv       ; save AC
            xorr    ac, ac
            incr    ac, ac
            st      .lpt_flag       ; signal: line flushed
            ld      .tty_acsv
            reti

lpt_flag:   bss     1
```

---

## Emulator Diagnostics

```
LPT: 0020 IRQ 005                   ; init with FILE* (no filename)
LPT: 0020 IRQ 005, file output.txt  ; init with named file
LPT: 0020 deinitialized
```

Device and IRQ numbers are printed in octal.
