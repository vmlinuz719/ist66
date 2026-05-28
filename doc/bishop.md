# RDC-700 Bishop Bitmap Display

The Bishop display device (`bishop.c`) is a monochrome raster display rendered
in an SDL2 window. Each pixel is one bit — black (0) or green (1). The display
is 512×480 visible pixels with a 32-row overscan region (512×512 total), and
supports hardware vertical scrolling. All drawing is done by DMA: either reading
pixels from memory or filling a rectangle with a repeating 1-bit pattern.

---

## Initialization

```c
init_bishop(cpu, id);
```

| Parameter | Description |
|-----------|-------------|
| `cpu` | Pointer to the CPU structure |
| `id` | Device number (12-bit), used in all I/O instructions |

Bishop does not take an IRQ parameter; the done flag is set on completion but
no interrupt is wired. Use `tiond`/`tiodn` for polling. The SDL window is
titled "You're watching Bishop TV" and is rendered at 2× scale (1024×960).

---

## I/O Register Model

### Function Numbers

All registers are 36-bit wide and are read and written using `rio`/`wio` with
the function numbers below.

| func | RIO (read) | WIO (write) |
|------|------------|-------------|
| 0 | Read DMA address (updated after each DMA read) | Write DMA address |
| 1 | Read fill pattern | Write fill pattern |
| 2 | Read rect register | Write rect register |
| 3 | Read base register | Write base register |
| 4 | Read scroll register | Write scroll register |

### Control Actions (instruction suffix)

| Suffix | ctl | Condition | Action |
|--------|-----|-----------|--------|
| `s` | 1 | Last WIO was func=0 | Start DMA read from memory into display |
| `s` | 1 | Last WIO was func=1 | Start DMA pattern fill |
| `c` | 2 | — | Cancel pending command; clear done flag |
| `p` | 3 | — | Advance scroll position by one step |

The device inspects which transfer preceded the ctl=1 to decide whether to do
a memory read or a pattern fill. If neither func=0 nor func=1 was the last
write, ctl=1 has no effect.

### Status and Skip Tests

| Instruction | Condition | Skip when |
|-------------|-----------|-----------|
| `tionb dev` | Busy | DMA operation in progress |
| `tiobz dev` | Not busy | No operation pending |
| `tiond dev` | Done | Last operation completed |
| `tiodn dev` | Not done | Operation not yet finished |

---

## Display Geometry

```
 x: 0 ──────────────────────── 511
 y:  0  ┌──────────────────────┐
        │  visible (512 × 480) │
480     └──────────────────────┘
        │  overscan (512 × 32) │  ← scroll wrap region
512     └──────────────────────┘
```

All coordinate fields are 9-bit unsigned values (0–511). The x coordinate
wraps at 512; the y coordinate wraps at 512 (total including overscan).

Scrolling shifts which row of the internal 512-row buffer is displayed at the
top of the visible window. Pixels written into the overscan rows become visible
when the scroll position advances past row 480.

---

## Register Descriptions

### DMA Address Register (func=0)

```
 35      27  26                           0
┌──────────┬────────────────────────────────┐
│ Shift[8:0]│        Word address [26:0]     │
└──────────┴────────────────────────────────┘
```

Pixels are read from physical memory 1 bit at a time, starting at bit
`shift−1` within the word at `word_address` and counting downward toward
bit 0. When bit 0 is passed, the word address increments and reading resumes
from bit 35. This packs 36 pixels per 36-bit word, MSB first.

After a DMA read completes, the DMA address register is automatically updated
to point to the word and bit position immediately following the last pixel
consumed. Reading func=0 after a DMA read returns this updated pointer, which
can be used to chain consecutive DMA operations.

### Fill Pattern Register (func=1)

A 36-bit repeating pattern used by the DMA pattern fill command. Pixels are
taken from bits 35 down to 0 in order; the pattern repeats every 36 pixels.

### Rect Register (func=2)

Defines the destination rectangle for the next DMA operation.

```
 35      27  26      18  17       9  8        0
┌──────────┬──────────┬───────────┬────────────┐
│   y[8:0] │   x[8:0] │  h−1[8:0] │  w−1[8:0]  │
└──────────┴──────────┴───────────┴────────────┘
```

| Field | Bits | Description |
|-------|------|-------------|
| y | [35:27] | Top-left y coordinate (base offset applied at dispatch) |
| x | [26:18] | Top-left x coordinate (base offset applied at dispatch) |
| h−1 | [17:9] | Height minus 1 (0 = 1 row) |
| w−1 | [8:0] | Width minus 1 (0 = 1 column) |

The actual pixel coordinates used are `(rect.x + base.x0) mod 512` and
`(rect.y + base.y0) mod 512`, allowing the base register to tile or offset
operations without modifying the rect.

### Base Register (func=3)

Provides a starting offset added to the rect coordinates, plus an
auto-advance increment applied after each DMA operation.

```
 35      27  26      18  17       9  8        0
┌──────────┬──────────┬───────────┬────────────┐
│  iy[8:0] │  ix[8:0] │   y0[8:0] │   x0[8:0]  │
└──────────┴──────────┴───────────┴────────────┘
```

| Field | Bits | Description |
|-------|------|-------------|
| iy | [35:27] | Y increment applied to y0 after each DMA operation |
| ix | [26:18] | X increment applied to x0 after each DMA operation |
| y0 | [17:9] | Current Y offset added to rect.y |
| x0 | [8:0] | Current X offset added to rect.x |

After each DMA command completes: `x0 ← x0 + ix`, `y0 ← (y0 + iy) mod 512`.
This enables rendering a sequence of tiles by issuing multiple DMA commands
without updating the base register between them.

### Scroll Register (func=4)

Controls vertical hardware scrolling.

```
 35      18  17       9  8        0
┌──────────┬───────────┬────────────┐
│ (unused) │  step[8:0]│  pos[8:0]  │
└──────────┴───────────┴────────────┘
```

| Field | Bits | Description |
|-------|------|-------------|
| step | [17:9] | Row increment applied per ctl=3 pulse |
| pos | [8:0] | Current scroll position (top visible row index) |

`wio Rn, 4, dev` sets both fields. `nio dev` (ctl=3) advances `pos` by `step`
without altering the step field: `pos ← (pos + step) mod 512`. A step of 1
scrolls one row per pulse; a step of 0 freezes the display.

---

## Programming Patterns

### Clear the Display (pattern fill)

```asm
; Fill the entire display with black (pattern = 0)
            xorr    ac, ac
            wio     ac, 1, BISHOP_DEV       ; pattern = 0 (black)
            xorr    ac, ac
            wio     ac, 2, BISHOP_DEV       ; rect: y=0, x=0, h=479, w=511
            or      ac, .full_rect
full_rect:  dw      0000000077777           ; h-1=479, w-1=511

            xorr    ac, ac
            wios    ac, 1, BISHOP_DEV       ; start pattern fill
wait_fill:  tiodn   BISHOP_DEV
            jmp     .wait_fill              ; wait for done
```

### Load Pixels from Memory

```asm
; DMA read: copy pixels from memory address 'framebuf' to display origin
            ldea    ac, .framebuf           ; word address (shift=0 → start at bit 35)
            or      ac, .dma_start
dma_start:  dw      (35 << 27)              ; shift = 35 (start from MSB)
            wios    ac, 0, BISHOP_DEV       ; write DMA address and start
wait_dma:   tiodn   BISHOP_DEV
            jmp     .wait_dma
```

### Smooth Scroll

```asm
; Set scroll step to 1 row
            dw      (1 << 9)                ; step=1, pos=0
            wio     ac, 4, BISHOP_DEV       ; load scroll register

; Each frame: advance scroll one row
            niop    BISHOP_DEV              ; ctl=3: pos += step
```

---

## Emulator Diagnostics

```
TV2: 0030                    ; at init (device number in octal)
TV2: 0030 deinitialized      ; at destroy
```
