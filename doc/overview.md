# RDC-700 Architecture Overview

The RDC-700 is a 36-bit word-addressable mainframe. All memory references address
36-bit words; there is no byte addressing at the hardware level (bytes are handled
by software-assisted bit-field instructions).

## Key Parameters

| Parameter | Value |
|-----------|-------|
| Word width | 36 bits |
| Virtual address width | 27 bits (128 MW, ~512 MB of word addresses) |
| Physical address width | 36 bits (but limited to installed memory) |
| Page size | 512 words |
| General-purpose registers | 16 × 36-bit |
| Control registers | 8 × 36-bit |
| Floating-point registers | 16 × 80-bit internal |
| Interrupt priority levels | 16 (0 = exceptions, 1–14 = devices, 15 = NMI) |

## Word Format

All instructions and data are stored as 36-bit words. Numeric literals in source
code and documentation use octal unless prefixed: `0nnn` = octal, `#nnn` = hex,
bare decimal otherwise. Octal is the native base because 36 = 3 × 12.

## Processor State

The processor maintains the following visible state:

- **Accumulators A0–A15**: 16 general-purpose 36-bit registers.
- **Control registers C0–C7**: Privileged state including program counter, interrupt
  level, segment table, and FPU control.
- **Float registers F0–F15**: 80-bit internal floating-point values, accessed in
  banks of four.
- **Carry flag**: Single bit embedded in the Program Status Word (C0).
- **Interrupt mask**: 16-bit register controlling which IRQ levels are enabled.

## Memory Protection

The RDC-700 supports two mutually exclusive protection modes, selected by the
Segment Descriptor Register (C5/SDR):

**Flat mode (SDR = 0)**: The physical address equals the virtual address. Each
512-word page carries an 8-bit storage key stored alongside page-zero of the page.
A memory access succeeds if the access key matches the page key, the page key is
0 (supervisor-only, key 0 always succeeds), 0xFE (read-public), or 0xFF
(read/write-public).

**Segmented mode (SDR ≠ 0)**: The virtual address is divided into a 9-bit segment
selector (bits 26–18) and an 18-bit offset (bits 17–0). Each segment has a
descriptor with a base address, size limit, protection key, and rights bits. A
segment may optionally use a second level of paging (512-word pages), in which
case the 18-bit offset is subdivided into a 9-bit page number and 9-bit page
offset.

## Execution Model

Instructions are fetched from the address in the Program Counter (low 27 bits of
C0/PSW). After each instruction the PC advances by 1 (one word). Some instructions
skip the following word, advancing PC by 2 instead. Interrupts and exceptions
redirect execution through a fixed interrupt vector table at addresses 0–31.

Instructions may not be misaligned; each instruction occupies exactly one 36-bit
word.
