# RDC-700 Memory Organization and Addressing

## Flat Mode (SDR = 0)

When the Segment Descriptor Register is zero, virtual addresses equal physical
addresses. The 27-bit address space is divided into 512-word pages. Each page
carries an 8-bit protection key stored in the high 8 bits of the physical memory
slot for word 0 of the page (the key occupies bits 43–36 of the 64-bit host
storage cell; it is never exposed as data).

Access rules in flat mode:

| Page key | Read | Write |
|----------|------|-------|
| 0x00 | key must be 0 (supervisor) | key must be 0 |
| 0x01–0xFD | key must match page key | key must match page key |
| 0xFE | always allowed | key must match page key |
| 0xFF | always allowed | always allowed |

Supervisor code always uses key 0 (PSW[35:28] = 0) and bypasses all key checks.

## Segmented Mode (SDR ≠ 0)

When SDR is non-zero, virtual addresses are translated through two levels of
tables before reaching physical memory.

### Virtual Address Layout

```
 26      18 17                                               0
┌──────────┬─────────────────────────────────────────────────┐
│ Seg[8:0] │               Offset [17:0]                     │
└──────────┴─────────────────────────────────────────────────┘
```

The 9-bit segment selector (bits 26–18) indexes the Segment Descriptor Table.
The 18-bit offset addresses within that segment.

### Segment Descriptor Table

The SDT is an array of two-word descriptors at the physical base given by SDR[26:0].
A segment selector of N addresses words `SDT_base + 2N` (word 0, base) and
`SDT_base + 2N + 1` (word 1, tag):

**Word 0 — Base**: Physical base address of the segment (36 bits).

**Word 1 — Tag**:

```
 35      28  27  26  25  24 23          18 17               0
┌──────────┬───┬───┬───┬───┬────────────┬──────────────────┐
│  Key[7:0] │ P │ W │ G │PG│ (reserved) │   Limit [17:0]   │
└──────────┴───┴───┴───┴───┴────────────┴──────────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35–28 | Key | Segment protection key (same semantics as flat-mode page keys, except 0xFF also allows writes) |
| 27 | P | Present — if 0, any access raises SEG_FAULT_PRESENT |
| 26 | W | Writable — if 0, any write raises SEG_FAULT_RIGHTS |
| 25 | G | Global — segment descriptor is not flushed by `invlsg` |
| 24 | PG | Paged — offset is further translated through a page table (see below) |
| 17–0 | Limit | Maximum valid offset when PG=0; ignored when PG=1 |

### Paged Segments (Tag bit 24 set)

When a segment is paged, the 18-bit offset is divided into a 9-bit page index
(bits 17–9) and a 9-bit page offset (bits 8–0). The segment's base address points
to a page table: an array of one-word page descriptors.

**Page descriptor word**:

```
 35                    9  8  7  6  5  4            0
┌────────────────────────┬──┬──┬──┬──┬─────────────┐
│   Physical base [35:9] │ P│ W│ G│NC│  (reserved) │
└────────────────────────┴──┴──┴──┴──┴─────────────┘
```

| Bits | Field | Description |
|------|-------|-------------|
| 35–9 | Base | Physical base address of the 512-word page (must be 512-word aligned) |
| 8 | P | Present |
| 7 | W | Writable |
| 6 | G | Global (not flushed by `invlpg` or context switches) |
| 5 | NC | No-cache (advisory) |

The physical address is `page_base + (vaddress & 0x1FF)`.

### TLB and Segment Cache

The processor caches the most recent 32 segment descriptor entries and 32 page
table entries. These caches are indexed by the low 5 bits of the selector and
tagged with the remaining bits. Software must flush stale entries after modifying
descriptors:

- `invlsg ea` — invalidate the segment cache entry for the segment containing `ea`
- `invlpg ea` — invalidate the TLB entry for the page containing `ea`

---

## Addressing Modes

All memory-reference instructions (including load/store, branches, and I/O) encode
an effective address using a 23-bit field in the instruction word:

```
 22  21       18  17                                         0
┌───┬──────────┬──────────────────────────────────────────────┐
│ I │  Index   │           Signed Displacement [17:0]         │
└───┴──────────┴──────────────────────────────────────────────┘
```

| Bit 22 | Field | Effect |
|--------|-------|--------|
| 0 | (direct) | Effective address = index base + sign-extended displacement |
| 1 | Indirect | Read word from computed address; use that as the final address (with optional auto-modify) |

### Index Field Values

| Index value | Assembler syntax | Effective address |
|-------------|-----------------|-------------------|
| 0 | `disp` | Zero-based absolute: EA = sign-extend(disp) |
| 1 | `_disp` | Direct page: EA = (C1[17:0] << 9) + sign-extend(disp) |
| 2 | `.disp` or `.label` | PC-relative: EA = PC + sign-extend(disp) |
| 3–13 | `disp(X0)`…`disp(SP)` | Register-indexed: EA = Rn + sign-extend(disp) (Rn = A3–A13) |
| 14 | `+disp` | Post-increment: EA = SP (old), then SP ← SP + disp |
| 15 | `=disp` | Pre-decrement: SP ← SP − disp first, then EA = new SP |

The displacement is an 18-bit signed two's-complement value, range −131072 to
+131071. In assembly syntax, a label reference with `.` prefix generates a
PC-relative displacement; without `.`, it generates an absolute address. Indirect
mode is specified by prefixing `@` in the assembler.

### Indirect Addressing

When I=1, the processor reads a word from the computed non-indirect address. If
bit 35 of that word is 0, it is used directly as the final 36-bit address
(only bits 26–0 are used for actual memory access).

If bit 35 of the fetched word is 1, an auto-modify mode is encoded in bits 34–27:

```
 35  34  33  32      27  26                                  0
┌───┬───┬───┬──────────┬────────────────────────────────────┐
│ 1 │ M1│ M0│  Inc[5:0]│             Address [26:0]         │
└───┴───┴───┴──────────┴────────────────────────────────────┘
```

| M1:M0 | Mode | Address used | Update written back |
|-------|------|--------------|---------------------|
| 00 | Post-increment | Address field (before increment) | Address ← Address + sign-extend(Inc) |
| 01 | Pre-decrement | Address − sign-extend(Inc) | Address ← Address − sign-extend(Inc) |
| 10, 11 | — | (fault: X_MEMX) | — |

The write-back to memory occurs after the instruction completes successfully. If
the instruction causes an exception, the write-back is suppressed so the sequence
can be retried.

### Stack Addressing

Index values 14 (post-increment) and 15 (pre-decrement) use A13 (SP) as the
base. Post-increment is used for `push` (store, then advance SP downward) and
pre-decrement for `pop` (retract SP, then load). The displacement operand gives
the step size; in practice it is almost always 1.

Combined with indirect addressing: `@+1` means "read the pointer at SP, use it
as an address, then advance SP by 1" — this is how indirect pop (e.g., `ld @(sp)`)
is implemented in the monitor for chained pointer tables.

---

## Memory Map Conventions

There are no hardware-enforced memory map regions beyond the interrupt vector table
at addresses 0–63. By convention, the monitor firmware uses:

| Region | Use |
|--------|-----|
| 0 | Exception vector (PSW0) |
| 1 | Exception vector (PSW1) |
| 2–31 | Interrupt vectors (PSW0/PSW1 pairs for IRQ 1–15) |
| 32–63 | Interrupt save area (PSW0/PSW1 for each IRQL) |
| 64+ | Application and OS code/data |
| 512–527 | Relocatable loader (pre-loaded at startup) |
| 1024+ | Firmware IPL code |
