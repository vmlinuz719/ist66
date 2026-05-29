# RDC-700 TTY Console Device

The TTY device (`tty.c`) implements a serial console as a Telnet TCP server. Each
instance listens on a configurable TCP port; a Telnet client connecting to that
port attaches as the console terminal. Only one client may be connected at a time —
a second connection attempt receives "Line busy" and is immediately closed.

---

## Initialization

```c
init_tty(cpu, id, irq, port);
```

| Parameter | Description |
|-----------|-------------|
| `cpu` | Pointer to the CPU structure |
| `id` | Device number (12-bit), used in all I/O instructions |
| `irq` | Interrupt level to assert when input is available |
| `port` | TCP port to listen on |

On initialization the device starts listening and spawns a listener thread.
When a client connects, reader and writer threads are created for that session.
The console is removed cleanly via `destroy_tty`.

**Default state at init**: receive **disabled** (ENABLED not set), echo all
characters, interrupt on LF, destructive backspace, threshold 160 bytes.
Software must write the config word with the ENABLED flag set before the device
will accept input.

---

## Telnet Negotiation

On accepting a connection the device immediately sends:

```
IAC WILL ECHO             (FF FB 01)
IAC WILL SUPPRESS-GO-AHEAD (FF FB 03)
```

This requests that the Telnet client suppress local echo and character-at-a-time
mode. The reader thread strips all Telnet IAC sequences from the incoming byte
stream before passing characters to the receive buffer.

---

## I/O Register Model

I/O instructions address the TTY by its device number. The `func` argument to
`rio`/`wio` selects which register to access. The instruction suffix (`s`, `c`)
selects the control action.

### Function Numbers

| Direction | func | Operation |
|-----------|------|-----------|
| `rio Rn, 0, dev` | Read | Pop next byte from receive buffer; −1 (all ones) if empty |
| `wio Rn, 0, dev` | Write | Load output byte into send register |
| `rio Rn, 1, dev` | Read | Read config word: bits[23:8] = control flags, bits[7:0] = threshold |
| `wio Rn, 1, dev` | Write | Write config word: same layout |
| `rio Rn, 2, dev` | Read | Read receive buffer fill count (0–255) |

Even `func` values read; odd `func` values write. The assembler enforces this
automatically through the `rio`/`wio` distinction.

### Control Actions (instruction suffix)

| Suffix | ctl | Action |
|--------|-----|--------|
| (none) | 0 | Data transfer only; no side effect |
| `s` | 1 | **Start/Send**: transmit the loaded output byte; clear done flag and release interrupt |
| `c` | 2 | **Clear**: clear done flag and release interrupt without sending |
| `p` | 3 | Pulse (not used by TTY) |

The ctl field and the func field are independent: a single instruction performs
both a data transfer and a control action. For example, `wios Rn, 0, dev` loads
the output byte **and** starts transmission in one instruction.

### Status and Skip Tests

The skip-test instructions use `transfer=14` internally; they do not consume
or produce a register value.

| Instruction | Condition | Skip when |
|-------------|-----------|-----------|
| `tionb dev` | Busy | output in progress |
| `tiobz dev` | Not busy | no output pending |
| `tiond dev` | Done | input interrupt asserted |
| `tiodn dev` | Not done | no interrupt pending |

**Busy** (bit 0 of status): set while an output byte is queued for transmission.
Cleared when the writer thread sends it.

**Done** (bit 1 of status): set when an interrupt condition fires (input
available). Cleared by ctl=1 or ctl=2.

---

## Config Word Layout

The config word is a 36-bit value with control flags packed into bits[23:8]
and the threshold byte in bits[7:0]. It is read and written via `rio`/`wio`
with func=1.

```
 35      24  23                  8  7          0
┌──────────┬──────────────────────┬────────────┐
│ (unused) │   Control flags      │  Threshold │
└──────────┴──────────────────────┴────────────┘
```

### Control Flag Bits (bits[23:8])

| Bit (in word) | Flag | Effect |
|---------------|------|--------|
| 8 | `ENABLED` | Enable receive; if clear, incoming chars are discarded with a bell |
| 9 | `INTR_ANY` | Assert interrupt on every received character |
| 10 | `INTR_ESC` | Assert interrupt on ESC (0x1B) |
| 11 | `INTR_RET` | Assert interrupt on LF (0x0A) — **default** |
| 12 | `DESTRUCT` | Enable destructive backspace: DEL (0x7F) or BS (0x08) removes the last buffered character and echoes it back; bell if buffer empty — **default** |
| 13 | `INTR_OUT` | Assert interrupt after each transmitted character completes |
| 14 | `ECHO_RET` | Echo CR (0x0D) and LF (0x0A) back to the terminal — **default** |
| 15 | `ECHO_TAB` | Echo TAB (0x09) — **default** |
| 16 | `ECHO_ALL` | Echo all received characters — **default** |

When `ECHO_ALL` is set it supersedes `ECHO_TAB` and `ECHO_RET`; those flags
are meaningful only when `ECHO_ALL` is clear.

### Threshold (bits[7:0])

When the receive buffer fill count reaches this value, an interrupt is
asserted regardless of the other interrupt conditions. Default: 160.
Setting threshold to 0 disables threshold-triggered interrupts.

### Default Config Word

At initialization: control flags = 0x1D8 (ECHO_ALL|ECHO_TAB|ECHO_RET|DESTRUCT|INTR_RET),
threshold = 0xA0 (160). Config word value = 0x1D8A0.

```asm
; Restore defaults
or      ac, .dflt_cfg
wio     ac, 1, TTY_DEV          ; write config

dflt_cfg:   dw  0x1D8A0         ; ECHO_ALL|ECHO_TAB|ECHO_RET|DESTRUCT|INTR_RET, thresh=160
```

---

## Receive Buffer

The receive buffer is a 256-byte ring. Characters are added by the reader
thread as they arrive from the TCP connection. `pop_char` (RIO func=0)
removes one character from the head.

When the buffer is full (255 bytes buffered) or `ENABLED` is clear, incoming
characters are discarded and a bell character (0x07) is sent back to the terminal.

---

## Interrupt Behavior

The device has one shared interrupt line used for both input and output events.
Exactly one interrupt is outstanding at a time: once asserted, no further
interrupt is generated until the current one is cleared.

**Input**: the interrupt is asserted when any of the following occurs:
- `INTR_ANY` is set (every received character)
- `INTR_ESC` is set and an ESC (0x1B) arrives
- `INTR_RET` is set and a LF (0x0A) arrives
- The buffer fill count reaches the threshold

**Output**: when `INTR_OUT` is set, the interrupt is asserted after each
transmitted character completes (the writer thread finishes the `send` call).
The `wios` instruction (ctl=1) clears any pending interrupt and starts
transmission; the interrupt re-fires when the send is done. This enables
interrupt-driven output without polling.

Because input and output share one interrupt and one done flag, `INTR_OUT`
should not be used simultaneously with receive interrupt conditions unless the
handler can distinguish the source by checking the receive buffer fill count.

The interrupt is cleared by executing any I/O instruction to the device with
ctl=1 (s suffix) or ctl=2 (c suffix).

---

## Programming Patterns

### Interrupt-Driven Input (monitor style)

The interrupt handler clears the interrupt and signals the foreground:

```asm
; Interrupt vector for TTY (IRQ level 10 → vector address 20)
tty_vec:    dw  tty_intr, 0

; Enable level 10 in the interrupt mask
mask:       dw  02000           ; bit 10 = level 10

; Interrupt handler
tty_intr:   nioc    TTY_DEV     ; clear interrupt (ctl=2, no data)
            st      .tty_acsv   ; save AC
            xorr    ac, ac      ; clear AC
            incr    ac, ac      ; AC ← 1
            st      .tty_flag   ; signal: data available
            ld      .tty_acsv   ; restore AC
            reti

tty_flag:   bss     1
tty_acsv:   bss     1
```

The foreground waits for `tty_flag` to become nonzero, then drains the buffer:

```asm
; Check if data is ready (call from idle loop)
test_rst:   ld      .tty_flag
            retr

; Read loop — called after tty_flag is set
readloop:
            rio     ac, 2, TTY_DEV      ; read buffer fill count
            movr.rz ac, ac              ; if zero, go back to idle
            jmp     .idleloop

            rio     ac, 0, TTY_DEV      ; pop one byte
            ; process character in AC ...
            jmp     .readloop
```

### Polled Output

```asm
; Write a single byte in AC to the console (polling)
write_byte:
wrt_wait:   tiobz   TTY_DEV     ; skip if NOT busy
            jmp     .wrt_wait   ; wait until ready

            wios    ac, 0, TTY_DEV  ; load byte and start (func=0, ctl=1)
            retr
```

### Interrupt-Driven Output

Enable `INTR_OUT` in the config word, then send the first byte. The interrupt
fires when each character is sent; the handler sends the next one:

```asm
; Start an output transfer (foreground: point X0 at string, set X1 = -length)
            ; configure TTY with INTR_OUT set
            or      ac, .out_cfg
out_cfg:    dw      #1F9A0      ; INTR_OUT|ECHO_ALL|ECHO_TAB|ECHO_RET|DESTRUCT|INTR_RET|ENABLED, thresh=160

            wio     ac, 1, TTY_DEV
            ; ... then kick off first byte:
            incldb  ac, x0, 7
            wios    ac, 0, TTY_DEV  ; send first byte, clears any pending interrupt

; Interrupt handler — fires after each byte is sent
tty_out_ih: st      .ih_acsv        ; save AC
            incr.rz x1, x1          ; x1++; skip jmp if x1 reached 0 (done)
            jmp     .out_done

            ld      .ih_acsv        ; (restore AC not needed — about to overwrite)
            incldb  ac, x0, 7       ; load next byte
            wios    ac, 0, TTY_DEV  ; send it (ctl=1 clears interrupt, starts next)
            reti

out_done:   nioc    TTY_DEV         ; clear interrupt without sending
            ; signal foreground that output is complete ...
            ld      .ih_acsv
            reti
```

### Writing a Length-Data String

```asm
; writeld: write dsn string at address in X0
writeld:    dw  0               ; callr return address (lightweight call)

            ldneg   x1, (x0)    ; x1 = -(length)
            movr.rz x1, x1      ; skip body if length = 0
            rets

wrt_loop:   tiobz   TTY_DEV     ; wait until not busy
            jmp     .wrt_loop

            incldb  ac, x0, 7   ; advance pointer, load 7-bit byte into AC
            wios    ac, 0, TTY_DEV

            incr.rn x1, x1      ; increment (toward zero); skip when zero
            jmp     .wrt_loop

            rets
```

---

## Emulator Diagnostics

The emulator prints to stderr:

```
TTY: 0030 IRQ 012, remote port 5000     ; at init
TTY: 0030 connected                     ; client connected
TTY: 0030 deinitialized                 ; at destroy
```

Device and IRQ numbers are printed in octal; TCP port in decimal.
