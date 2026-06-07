# CV1K Sandbox

A compact CV1000 / DDPSDOJ (DoDonPachi SaiDaiOuJou) emulator that boots the real
game through a MAME-derived Hitachi SH-3 (SH7709S) CPU core and the MAME-derived
CV1000 blitter.

## Current status

The CPU is now a faithful port of MAME's SH7709S interpreter (see
`src/sh3_core.cpp` + `src/sh_common_ops.inc`), replacing the previous
hand-written partial core.  As a result the game boots through its real
sequence — memory check, EEPROM check, initialization — entirely via the
emulated CPU:

- `illegal=0` (no unimplemented opcodes)
- real vblank IRQ2 accepted every frame (`irq_ack` ≈ frame count)
- real MMIO blitter execute writes (`mmio>0`) drive all rendering
- **no DDPSDOJ-specific hacks**: the old auto-blit RAM-pointer replay, synthetic
  vblank tick, idle-PC speedup tick, and boot-assist register pokes are gone
  (`autoblit=0`).

One `cv1k_machine_frame()` now advances one real video frame of SH-3 cycles
(102.4 MHz / 60.024 Hz) and asserts IRQ2 at the vsync pulse, with an accurate
idle-loop skip (MAME's `spin_until_interrupt` equivalent).

## Build

```sh
make                 # headless build -> cv1k_sandbox
make sdl12 SDL12_CONFIG=sdl-config   # SDL 1.2 frontend -> cv1k_sandbox_sdl12
```

The whole project now links with the C++ toolchain because the SH-3 core is
C++.  After editing `src/cpu_sh7709s.h` run `make clean` first — the Makefile
does not track header dependencies and a stale object causes a struct-size
mismatch crash.

## Run

Headless (dumps the final frame, raw 320x240 landscape framebuffer):

```sh
./cv1k_sandbox --model d --romset ddpsdoj.zip --run-frames 5000 --dump-ppm out.ppm
```

SDL 1.2 frontend (portrait TATE window, ROT270, audio + input):

```sh
./cv1k_sandbox_sdl12 --model d --romset ddpsdoj.zip --sdl
```

IRQ2 and the idle speedup are on by default now; use `--no-irq2` / `--no-speedup`
for diagnostics.  The ROM archive is not included — provide `ddpsdoj.zip`.

## Controls (SDL 1.2)

- Movement: arrows or W/A/S/D
- P1 buttons: J/K/L/I (plus Space/Ctrl/Alt/Shift aliases)
- P1 start: 1, Coin 1: 5, P2 start: 2, Coin 2: 6, Service: 9
- Save / load quick state: F5 / F8, Quit: Escape

## Known remaining work

- Reach and validate the title / attract / gameplay against MAME (boot is long
  because the interpreter runs ~10x slower than real-time during the CPU-bound
  memory check).
- Remaining video fidelity differences vs MAME beyond the boot screens.
- Complete YMZ770C sound (currently only early register writes are observed).
