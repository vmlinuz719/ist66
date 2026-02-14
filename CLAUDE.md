# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

IST-66 is a C emulator for the RDC-700, a 36-bit word mainframe computer. It emulates the CPU, floating-point unit, virtual memory with segmentation/paging, interrupt system, and several I/O devices (paper tape, line printer, telnet TTY, SDL2 panel display).

## Build Commands

```bash
make              # Build the ist66 executable
make clean        # Clean build artifacts
make -C tools     # Build tape format conversion utilities (nbt2tap, tap2nbt)
```

The assembler is a Python script: `python3 tools/assembler.py`

## Dependencies

- GCC with `-O3 -Wall -Iinclude -flto`
- SDL2 (`-lSDL2`) for the panel display
- pthreads for multi-threaded device emulation
- POSIX sockets for telnet TTY

## Architecture

**Core emulation loop:** `cpu.c` contains `main()`, the instruction execution engine (`exec_all()`), memory management (segmentation + TLB), and the interrupt system. This is by far the largest file (~2600 lines).

**Functional units:**
- `alu.c` — 36/37-bit ALU with rotation, masking, skip conditions, and 15 operations
- `fpu.c` — RDC-700 floating-point format (1-bit sign, 8-bit exponent, 27/64-bit significand) with add, multiply, divide, and format conversion

**I/O device model:** Devices register callback function pointers and run in separate pthreads. Each device has its own source file:
- `ppt.c` — Paper tape reader (bootstrap device)
- `lpt.c` — Line printer
- `pch.c` — Paper tape punch
- `tty.c` — Telnet terminal (TCP socket on port 8080)
- `panel.c` — SDL2 visual panel showing registers/accumulators

**Tape format support:** `9ball.c` handles 9-bit tape packing; `aws.c` provides auxiliary storage interface.

**Headers** are in `include/` with matching names (`cpu.h`, `alu.h`, `fpu.h`, etc.).

## Threading Model

- Main thread: interactive command interface and CPU control
- CPU thread: instruction execution loop (`run()`)
- Each I/O device spawns its own thread(s)
- Synchronization via pthreads mutexes and condition variables

## Key Conventions

- **Octal notation** is used extensively (device IDs, memory addresses, bootstrap code)
- **36-bit values** use `uint64_t` with `MASK_36` (0xFFFFFFFFFL) and `MASK_37`
- Memory protection uses 8-bit keys per 512-word page (0x00=supervisor, 0xFF=public r/w)
- 16 accumulators (A[0]-A[15]), 8 control registers (C[0]-C[7]), 16 FP registers (F[0]-F[15])
- Interrupts have 14 priority levels (lower number = higher priority)

## Running the Emulator

After building, `./ist66` starts an interactive command loop:
- `/address` — set memory pointer
- `.count` — display memory from pointer
- `=value [value ...]` — write values to memory
- `W` — start CPU and wait for halt
- `S` — single-step

The TTY device listens on port 8080 for telnet connections.
