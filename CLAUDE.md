# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`ist66` is an emulator for a fictional 36-bit-word minicomputer (the "RDC700" family),
written in C. Words are 36 bits stored in `uint64_t`; the machine and all tooling are
**octal-oriented** — addresses, opcodes, and memory dumps are printed and parsed in octal
throughout. The emulated machine has a front panel, paper-tape and line-printer peripherals,
a TTY served over telnet, and two SDL-backed graphical displays.

## Build, test, run

```sh
make            # build the ./ist66 emulator
make test       # build + run CUnit suites (tests/test_fpu, tests/test_cpu)
make clean
```

- The emulator links `-lSDL2 -lSDL2_ttf -lSDL2_gfx` and pthreads; `make test` links `-lcunit`.
- `CFLAGS` use `-Wall -Werror`, so warnings break the build.
- Tests are CUnit binaries with no finer-grained selection — run a single suite directly:
  `./tests/test_fpu` or `./tests/test_cpu`.
- Tape-format converters build separately: `cd tools && make` produces `nbt2tap` and `tap2nbt`.

Running `./ist66` opens SDL windows and drops you into an interactive **monitor** prompt
(the loop at the bottom of `cpu.c`'s `main`). Monitor commands operate on a current octal
pointer: `/<addr>` sets it, `?` prints it, `.<count>` dumps memory, `=<vals...>` deposits
octal words. `main` hardcodes the initial device set and preloads a relocatable loader.

## Architecture

**CPU core — `cpu.c` (large) + `include/cpu.h`.** `struct ist66_cu` holds all machine state:
16 accumulators `a[]`, 8 control registers `c[]` (indexed by `C_PSW`, `C_CW`, `C_FCW`, …),
16 floats `f[]`, memory, the TLB/segment caches, and the device tables. The CPU runs on its
own pthread (`start_cpu`/`stop_cpu`); `cpu.h` defines the interrupt/exception model as inline
helpers — `do_intr`, `do_except`, `leave_intr`, `halt` — plus `read_mem`/`write_mem`, which go
through `seg_cache[]` + `tlb[]` for virtual-memory translation and raise faults via the
`MEM_FAULT`/`KEY_FAULT`/`SEG_FAULT_*` bits.

**ALU and FPU are separate, independently testable units.** `alu.c`/`include/alu.h` implement
the integer `compute()` plus inline 36-bit multiply; the masks and sign-extension macros
(`MASK_36`, `EXT*`, `CARRY`, `SKIP`) live in the header. `fpu.c`/`include/fpu.h` implement the
`rdc700_float_t` format (an 80-bit-ish internal representation packed into 36- or 72-bit
storage words) with add/mul/div/normalize. Both have CUnit coverage in `tests/`.

**I/O model — function-pointer device table.** Each device is registered into the parallel
arrays `cpu->io[id]`, `cpu->ioctx[id]`, `cpu->io_destroy[id]`. An I/O instruction decodes a
device id, a 2-bit `ctl`, a 4-bit `transfer`, and an accumulator index out of the instruction
word, then calls `io[id](ctx, accumulator, ctl, transfer)` (see `exec_io1` in `cpu.c`). Each
device file exposes an `init_*` that allocates its context and installs the three pointers:
- `tty.c` — TTY exposed as a telnet server on a TCP port
- `lpt.c` — line printer (to a `FILE*` or filename)
- `ppt.c` — paper-tape reader; `pch.c` — paper-tape punch
- `panel.c` — front panel (SDL window); `bishop.c` — raster graphics display (SDL)

**Display subsystem — `render.c` + `include/sdlctx.h`.** A single render-loop pthread owns
SDL. Devices that draw register a `window_ctx_t` (init/render/event/destroy callbacks) via
`register_window`; `start_render`/`kill_render` manage the loop. Keyboard input uses keysym
hotkeys only, never IME text input.

**Tape image formats.** `9ball.c`/`include/9ball.h` implement the project's "Nineball" 9-bit
tape format (paper-tape style, with `nbt_*` read/write/seek/mark calls). `aws.c`/`include/aws.h`
implement AWS-style tape records. `tools/nbt2tap` and `tools/tap2nbt` convert Nineball ⇄ SimH
`.tap`. `monitor.ppt` is a paper-tape image loaded by the PPT device in `main`.

## Assemblers and source conventions

Two assemblers exist for the same instruction set, with **different input formats**:

- `not_vibe_code/asm2.c` — **the current, up-to-date assembler.** A hand-written C assembler
  producing relocatable output (`gcc asm2.c -o asm2`, then `./asm2 <src>`). The `.a700` files in
  `not_vibe_code/` (`monitor.a700`, `mandelbrot.a700`, `keyserver.a700`, …) are its source format.
  Use this for new work.
- `tools/assembler.py` — **deprecated.** Python, **80-column punch-card layout** (fixed columns:
  label, opcode, argument, comment, sequence number). Run as `python assembler.py <src>`; output
  defaults to a PPT tape, `-r` emits RIM-loader format, `-c` emits C array initializers. The `.txt`
  files in `tools/` (e.g. `monitor.txt`, `string.txt`, `ringbuf.txt`) are card-format source for it.

`tools/instruction_formats.json` is the authoritative bit-field description of the ISA;
`tools/gen_instruction_doc.py` renders it to printable HTML with SVG field diagrams.

## Directory conventions

- `not_vibe_code/` — hand-written code the author vouches for (notably `asm2.c` and the `.a700`
  assembly programs). Treat this as the **trusted reference**.
- `vibe_code/` — AI-generated artifacts (e.g. `vibasic.a700`, `bcd_test.c`). **Not trusted** —
  do not rely on it as a correctness reference; verify anything taken from here. The Makefiles
  are also flagged "vibe coded."
