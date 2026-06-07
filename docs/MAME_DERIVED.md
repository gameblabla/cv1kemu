# MAME-derived material in v6

This sandbox is not MAME. It is a small ANSI C project that uses MAME's CV1000 driver/video implementation and SH-3 support code as a hardware reference.

Reference commit used by the previous work and this revision:

`acad9ca235f4026b1765f62fec340f6d95b2e9ab`

Files used as reference:

- `src/mame/cave/cv1k.cpp`
- `src/mame/cave/cv1k_v.cpp`
- `src/devices/cpu/sh/sh3comn.h`

Items derived from or aligned with MAME:

- `ddpsdoj` ROM manifest: `u2`, `u4`, `u23`, `u24` names, CRCs, sizes, and `ROM_IGNORE(0x100)` behavior for U4/U23/U24.
- CV1000-D model default for `ddpsdoj`.
- NAND region size `0x08400000`, preserving the 128 MiB NAND data area plus 4 MiB spare/OOB area.
- 16-bit word-swap handling for U4/U23/U24 to mirror `ROM_LOAD16_WORD_SWAP`.
- Hardware topology: SH7709S at 102.4 MHz, YMZ770C-F at 16.384 MHz, Samsung-compatible U2 NAND graphics flash, U23/U24 sound flash, RTC9701, CV1000-B/D RAM split, and CV1000 memory windows.
- Input port mapping structure for ports C/D/F/L and EEPROM output bit lines.
- SH3 `AS_IO` callback offsets from `sh3comn.h`: C/D/E/F/J/K/L style callback ranges.
- CV1000 blitter command constants, operation sizes, ARGB1555/tint/blend approach, ready register, DSW read, and FPGA port behavior.
- FPGA bitstream clocking convention: data bit on rising CLK while CE is asserted, 2,323,240-bit stream length, and MAME's firmware-version checksums.

Additional v6 behavior aligned to the MAME/SH reference model:

- SH-3 P1/P2 alias treatment for physical board windows.
- SH-3 P4 internal-register shadowing for high internal addresses used by cache/MMU/INTC probes.
- Experimental IRQ2/vblank notes: MAME wires the CV1000 screen vblank to the SH CPU IRQ2 line; the sandbox keeps IRQ2 optional and uses a synthetic RAM tick for the default headless DDPSDOJ probe until the SH interrupt core is complete.

MAME attribution:

The referenced CV1000 MAME files carry BSD-3-Clause license headers and name David Haywood, Luca Elia, and MetalliC as copyright holders. Preserve this notice when redistributing this sandbox. The wider MAME project is GPL-2.0-or-later; wholesale imports of additional MAME framework code may change redistribution obligations.

Additional v6 note:

- The DDPSDOJ boot compatibility assist is not copied from MAME. It is a temporary sandbox shim based on traces from this partial emulator and should be replaced with accurate SH7709S cache/MMU/TLB and peripheral behavior.

## v7 notes

The SH7709S banked-register behavior added in v7 follows the SH-family architectural behavior expected by MAME's SH7709S device rather than being a line-for-line source copy. The CV1000 memory map, port map, NAND/YMZ770/RTC topology, screen timing, and ROM manifest remain based on MAME's `src/mame/cave/cv1k.cpp` and `src/mame/cave/cv1k_v.cpp` at commit `acad9ca235f4026b1765f62fec340f6d95b2e9ab`.

## v8 notes

v8 continues to use MAME's CV1000 driver/video source as the hardware reference.  The added blitter-ready readback is based on the MAME CV1000 reset path installing the blitter register handlers at `0x18000000-0x18000057`, and the observed firmware-upload bit count follows MAME's CV1000 video firmware length.  The byte-fill-loop accelerator is not a MAME-derived hardware behavior; it is a local interpreter optimization for a deterministic DDPSDOJ software loop.

## v11 notes

v11 keeps the MAME-derived CV1000 topology and ROM manifest. The added `--trace-fetch` and odd-PC alignment are local diagnostic/interpreter scaffolding. The `--aggressive-assists` status-loop experiment is not MAME-derived hardware behavior and is disabled by default; it exists only to prove that the current `0c1fb3dc` loop cannot be safely bypassed without fixing the upstream SH7709S/NAND/DMAC model.

## v12 note

v12 adds a local, opt-in data-cache experiment. It is not copied from MAME. It exists because MAME's CV1000 notes call out experimental SH7709S cache/memory timing and the driver maps the SH internal cache-like RAM window at `0xf0000000-0xf0ffffff`. The experiment is disabled by default because the simplified non-coherent behavior is not yet accurate.

## v13 local changes

v13 adds a local NAND command-state fix for command `0x50`, based on the fact that MAME's CV1000 driver uses the generic Samsung K9F1G08U0M NAND device for U2 rather than a hand-written byte cursor. The command-state fix is not copied from MAME line-for-line; it is a C89 approximation intended to avoid stale address-cycle state when DDPSDOJ probes the spare-area command after normal page reads.

v13 also adds optional cache-control instruction hooks for `PREF`, `OCBI`, `OCBP`, and `OCBWB`. These hooks are disabled by default because the current cache model is not yet accurate enough to honor them without regressing the boot path.

## v14 local changes

v14 adds a local SH7709S TLB shadow and `LDTLB` opcode hook. This is not copied line-for-line from MAME's SH CPU core. It is a small C89 approximation added because MAME's CV1000 driver depends on the real SH7709S device for MMU/cache behavior while this sandbox only has a partial interpreter. The CV1000 hardware topology, memory map, DDPSDOJ ROM manifest, NAND notes, and vblank/IRQ2 reference remain based on MAME's `src/mame/cave/cv1k.cpp` and `src/mame/cave/cv1k_v.cpp` at commit `acad9ca235f4026b1765f62fec340f6d95b2e9ab`.


## v15 local additions

`--wide-p0-alias`, RAM-slice dumping, and the `widep0=` status field are local sandbox diagnostics. They are not copied from MAME. They are meant to help replace broad compatibility assists with a more accurate SH7709S cache/TLB/DMAC model.

## v17 local additions

v17 adds local diagnostics and a gated DDPSDOJ-specific NAND-copy status assist. This assist is not copied from MAME and is not accurate hardware emulation. It exists to expose the next boot blocker while the sandbox is still missing MAME's full SH7709S cache/TLB/MMU, DMAC, NAND, IRQ, timer, and peripheral implementations.

## v17 local additions

v17 adds local C89 diagnostics not copied from MAME: P2/uncached data-cache bypass behavior, physical I/O-window protection when the broad P0 alias experiment is enabled, an invalid-target guard for aggressive DDPSDOJ tracing, and a `--nand-data-only` experiment. These are local scaffolding around the missing full MAME SH7709S/NAND/DMAC/cache behavior, not replacement implementations of the MAME devices.


## v21 local changes

v21 adds local C89 diagnostics and approximations around areas MAME delegates to full devices: NAND random-output address-cycle handling, NAND spare/OOB counters, SH internal-I/O protection when broad P0 aliasing is enabled, and non-invasive SH TRAPA/IRQ diagnostic state. These are not copied MAME code; they are scaffold work around the missing full SH7709S/NAND/DMAC devices.
## v24 MAME SH7709S cache metadata note

v24 adds an opt-in diagnostic cache metadata tracker based on MAME's SH7709S source at commit `acad9ca235f4026b1765f62fec340f6d95b2e9ab`.  The tracker mirrors the source-level cache geometry constants and metadata behavior only: 16 KiB total cache, 16-byte lines, 4-way set associativity, LRU movement, write dirty bits, and dirty-eviction counting.  It does not import MAME's DRC, scheduler, bus timing, or SH core framework, and it is disabled by default because the sandbox still lacks a complete SH7709S cache/MMU/TLB/coherency implementation.

## v25 MAME RTC9701/YMZ770 port note

v25 was built after decompressing the uploaded `mame-master.tar.gz`.  The RTC9701 serial state machine in `src/rtc9701.c` is an ANSI C port of the MAME `rtc9701_device` command/data flow: command wait, RTC read/write, EEPROM read/write, write-enable drain, 4-bit RTC register addresses, 12-bit EEPROM address phase, 8-bit RTC data, and 16-bit EEPROM data.  The implementation is trimmed to the CV1000 `cv1k.cpp` serial line usage and does not import MAME's device framework.

The YMZ770 register scaffold in `src/sound_ymz770.c` is likewise aligned to MAME's `ymz770_device::write` / `internal_reg_write` behavior for YMZ770C register select/data writes, global registers, playback channel bookkeeping, key-on/key-off, looping, pan/volume latches, and sequence stop masks.  AMM/MPEG decoding and audio stream generation remain unimplemented.

Additional MAME copyright holders represented by these v25 ports: Angelo Salese and David Haywood for RTC9701; Olivier Galibert, R. Belmont, and MetalliC for YMZ770.  The MAME source files carry BSD-3-Clause license headers.

## v26 MAME NAND port note

v26 was built from the uploaded `mame-master.tar.gz` reference tree.  The NAND model in `src/nand.c` and `src/nand.h` is an ANSI C adaptation of MAME's generic NAND flash command/state behavior from `src/devices/machine/nandflash.cpp` and `src/devices/machine/nandflash.h`, configured for the Samsung K9F1G08U0M geometry used by the CV1000 driver.  The port covers MAME-style command modes, pointer modes, address-cycle sequencing, page-register program/random-input behavior, read/status/ID paths, and erase/program status handling.  It does not import MAME's device framework or scheduler.

Additional MAME copyright holder represented by this v26 NAND port: Raphael Nabet.  The MAME NAND flash source files carry BSD-3-Clause license headers.

## v27 MAME blitter-register-map port note

v27 corrects the standalone ANSI C blitter register side effects to match MAME's `cv1k_blitter_device::blitter_r` / `blitter_w` map in `src/mame/cave/cv1k_v.cpp`: `0x18000004` low-byte bit 0 launches a blit, `0x18000008` supplies the command-list address, `0x18000010` returns the ready bit, `0x18000014/0x18` are scroll registers, `0x18000040/0x44` are clip-origin registers, and `0x18000024/0x28` read back as all ones.  The pixel pipeline remains the existing simplified C renderer rather than a wholesale port of MAME's generated blit function tables.


## v28 MAME blitter-command-stream port note

v28 adds the `0xc000` clip command accepted by MAME's `cv1k_blitter_device::gfx_create_shadow_copy` and `gfx_exec` command stream parsers in `src/mame/cave/cv1k_v.cpp`. The ANSI C port consumes the command word and one parameter word, applies the same full-VRAM versus screen-clip-with-margin behavior, and records clip-command diagnostics. v28 also changes default blitter register reads to return zero, matching MAME's `blitter_r` default return path, and classifies the uploaded FPGA firmware checksum using MAME's firmware table.

## v29 MAME blitter/SH exception alignment note

v29 continues the `cv1k_v.cpp` and SH7709S alignment work. The blitter register read at offset `0x50` now returns the default CV1000 `DSW` input-port value (`0xfffffff0`) instead of zero, matching MAME's `cv1k_blitter_device::blitter_r` callback behavior for the default DIP configuration. Blitter command execution now snapshots RAM before parsing/executing a list, approximating MAME's `m_ram16_copy` shadow-copy contract in a simple ANSI C way.

v29 also adds the opt-in `--mame-trapa` path. This follows MAME's SH3/SH7709S `TRAPA` behavior by saving `TRA`, `SSR`, `SPC`, and `SGR`, setting `MD/RB/BL`, recording `EXPEVT = 0x160`, and vectoring to `VBR + 0x100`. It remains disabled by default because the sandbox still lacks enough MAME-equivalent interrupt/timer/cache/MMU fidelity for that path to boot DDPSDOJ farther than the diagnostic default path.


## v30 MAME SH7709S interrupt-vector alignment note

v30 corrects a SH7709S interrupt-vector mismatch in the standalone CPU core.  MAME's SH3/SH7709S path stores the line-specific interrupt event code in the interrupt event registers, but accepted interrupt exceptions vector through `VBR + 0x600`; the previous sandbox path jumped to `VBR + event`, which sent IRQ2 to `VBR + 0x640` and skipped the start of the game's dispatcher.  This correction affects the optional IRQ/vblank diagnostic modes and is kept separate from the conservative no-IRQ default run.


## v31

Adds an opt-in ANSI C scheduler equivalent for MAME's DDPSDOJ speedup handler (`install_speedups(0x02310, 0x0c1d1346, true)`) and resets the CV1000 blitter execution clip from the MAME 0x40/0x44 clip registers at the start of each blit list. Source attribution remains BSD-3-Clause MAME (`src/mame/cave/cv1k.cpp`, `src/mame/cave/cv1k_v.cpp`).

## v32 MAME blitter timing-accounting note

v32 continues the ANSI C adaptation of MAME's `src/mame/cave/cv1k_v.cpp` / `cv1k_v.h` blitter behavior. It corrects the clip idle-operation size to MAME's `CV1K_CLIP_OPERATION_SIZE_BYTES = 2`, adds MAME-style 64-byte idle-operation chunk accounting with the 700 ns operation-read interval, ports `calculate_vram_accesses()` for 32x32 VRAM row-crossing estimates, and applies MAME's horizontal-line contention penalty. The pixel renderer remains the sandbox implementation; this patch aligns timing/accounting and diagnostics, not the full threaded MAME scheduler.


## v33 MAME blitter pixel/command alignment note

v33 fixes four concrete CV1000 blitter mismatches found by comparing the sandbox renderer against MAME's `src/mame/cave/cv1k_v.cpp`, `cv1k_v.h`, and `cv1k_v_in.ipp`: clip-list commands now advance over both opcode and parameter words while still charging MAME's 2-byte idle operation, destination coordinates are sign-extended as full 16-bit values like MAME's `util::sext<int>(..., 16)`, tint bytes are converted with MAME's `tint_to_clr` `>> 2` rule so `0x80` maps to the normal `0x20` multiplier and zero remains a real zero tint, and screen presentation applies MAME's negative scroll (`copyscrollbitmap` with `-m_gfx_scroll_x/y`).

## v35

Adds an ANSI C adaptation of MAME's SH-3 DMAC transfer-size/address-update behavior from `src/devices/cpu/sh/sh4dmac.cpp` and matches the DDPSDOJ speedup handler's `idlepc || idlepc+2` condition from `src/mame/cave/cv1k.cpp`. Attribution remains BSD-3-Clause MAME.

## v35

v35 corrects the SH7709S RTE ordering to match MAME's SH3/SH7709S implementation.  MAME executes the RTE delay slot before restoring `SR` from `SSR` and branching to `SPC`; the previous sandbox restored `SR` before executing the slot.  This is most visible in optional IRQ/vblank experiments because restoring `SR` too early changes BL/RB/MD and the interrupt mask during the delay-slot instruction.  The fix is an ANSI C adaptation of the ordering in MAME's `sh4.cpp` RTE generator, not a wholesale copy of the SH dynamic recompiler.


## v36 MAME SH7709S interrupt-controller alignment note

v36 corrects two SH7709S interrupt-controller mismatches against MAME's `intc_7709_map` / SH3 interrupt path. First, the standalone CPU now keeps the asserted interrupt line separate from its effective priority: IRQ2 still records event code `0x640`, while the priority is read from IPRC bits 8..11 and compared against `SR.IMASK`. Second, accepted interrupts now mirror `INTEVT2` at the MAME-mapped SH7709S internal offset `0x04000000` rather than the older sandbox-only `0xfe00` shadow. This is still not a full MAME INTC/TMU/scheduler port; it fixes the concrete line/priority/register mapping used by the optional vblank IRQ diagnostics.

## v37 MAME SH-3 DMAC completion/alignment note

v37 tightens the ANSI C DMAC adaptation against MAME's `src/devices/cpu/sh/sh4dmac.cpp`.  It adds MAME's source/destination alignment before 16-bit, 32-bit, and 16-byte transfers, treats zero `DMATCR` as an active transfer request rather than an error, and clears `DMATCR` on completion while setting transfer-end in `CHCR`.  MAME expands zero `DMATCR` to `0x1000000` transfers; this standalone verification harness uses a bounded transfer count to keep DDPSDOJ headless test runs finite, so this remains a partial adaptation rather than a full scheduler-equivalent port.


## v38 MAME zero-DMATCR transfer-count note

v38 adds an opt-in `--mame-full-dmatcr` diagnostic for MAME's zero-DMATCR rule.  MAME's SH DMAC treats `DMATCR == 0` as `0x1000000` transfers; the conservative default keeps the v37 finite bound because the standalone core still lacks MAME's asynchronous DMA timer/scheduler.  When the option is enabled, v38 uses the full count and an ANSI C fast path for the CV1000 NAND data-port to incrementing RAM DMA shape while still reading through the MAME-derived NAND command/state machine and ignoring writes beyond the mapped CV1000-D RAM window.  This continues the adaptation of MAME's `src/devices/cpu/sh/sh4dmac.cpp` behavior without importing MAME's scheduler/timer framework.


## v38 MAME DMAC start-gating note

v38 also corrects the standalone DMAC start conditions to match MAME's SH7709S DMAC map more closely: channel checks are now run only when CHCR is written or when DMAOR is written; DMAOR.DME must be set; CHCR.TE and DMAOR.AE/NMIF block transfer; and CHCR.RS must be in MAME's accepted range.  DMAOR at `0x04000060-0x04000061` is now readable as the stored controller register rather than a forced zero ready value.

## v39 MAME SH7709S TMU note

v39 adds a bounded ANSI C adaptation of MAME's SH7709S timer unit behavior from `src/devices/cpu/sh/sh4tmu.cpp`, the SH3 `tmu_map` in `sh4.cpp`, and the TUNI event constants in `sh4comn.cpp`. The sandbox now tracks TSTR, TCOR/TCNT/TCR state, the MAME divisor table, underflow flagging, TCNT reloads, and TUNI0/TUNI1/TUNI2 event codes (`0x400`, `0x420`, `0x440`). The `--mame-tmu-irq` option is diagnostic only because the standalone core still lacks MAME's full scheduler, interrupt arbitration, and exception-vector timing.


## v40 MAME SH7709S reset-state note

v40 mirrors the nonzero SH7709S internal-register reset/start defaults that MAME initializes in `src/devices/cpu/sh/sh4.cpp`: TMU compare/count registers start at `0xffffffff`, CPG/BSC wait-control registers use MAME's SH-3 defaults, CMT `CMCOR` starts at `0xffff`, SCI/IRDA/SCIF status and baud/data registers use their MAME reset values, ADC/DAC control defaults are populated, SH7709 port-control registers are initialized to the documented `0xaaaa`/`0xaa8a`/`0xa888` patterns, and UDI `SDIR` starts at `0xffff`. This does not import MAME's device framework; it removes the prior all-zero shadow-register mismatch for code that reads untouched SH7709S internal registers.


## v42 reset context

The SH7709S reset PC/SP behavior is adapted from MAME `src/devices/cpu/sh/sh4.cpp`, specifically `sh34_base_device::device_reset()`: PC begins at `0xa0000000`, and R15 is initialized from `read_long(4)` after the boot ROM is visible.


## v43

- Corrected SH7709S CCN/MMU register offsets to match MAME `sh3_base_device::ccn_map`: `MMUCR=0xffffffe0`, `CCR=0xffffffec`, `PTEH=0xfffffff0`, `PTEL=0xfffffff4`, `TTB=0xfffffff8`, and `TEA=0xfffffffc`. This fixes the prior accidental use of BSC wait-state registers as TLB state.


## v44

v44 adds the TUNI cancellation side of the SH7709S TMU adaptation from MAME `src/devices/cpu/sh/sh4tmu.cpp`.  In MAME, a write to `TCR0`, `TCR1`, or `TCR2` calls `sh4_exception_unrequest(SH4_INTC_TUNI*)` whenever either the timer interrupt enable bit (`TIE`, bit 5) or the underflow flag (`UNF`, bit 8) is clear.  The ANSI C scaffold now mirrors that for its optional `--mame-tmu-irq` path by clearing a pending internal event-code IRQ for `0x400`, `0x420`, or `0x440`.  This is not a full scheduler/interrupt-controller port; it closes the concrete stale-TMU-IRQ latch mismatch introduced when v39 added underflow requests.

## v45

v45 mirrors MAME's SH3 interrupt-cause register behavior more closely.  In MAME's SH3 interrupt path, accepted interrupts update `m_intevt2` from the SH7709S INTC source table, update `m_expevt` from the exception-code table, and also update CCN `m_intevt`: source codes below `0x600` are mirrored directly, while line-style IRQ sources at or above `0x600` become the priority-coded value `0x3e0 - priority * 0x20`.  Earlier sandbox builds only updated the MAME-mapped `INTEVT2` shadow, leaving `INTEVT` at `0xffffffd8` stale.  The v45 ANSI C adaptation writes both visible register shadows and preserves the existing `EXPEVT` write in the exception path.


## v46

v46 adapts MAME's SH-3 DMAC completion timing more closely.  For memory-style RS 4-6 transfers, MAME performs the memory copy but sets `DMATCR=0` and `CHCR.TE` from the DMA timer callback after `2 * count + 1` CPU cycles, not immediately at `CHCR` write time.  The ANSI C sandbox now keeps a small per-channel pending-completion timer so polling code observes the in-flight DMA state for at least the MAME-equivalent delay window.

## v47

v47 does not add a new wholesale MAME device port.  It adds a guarded SH7709S interpreter accelerator for a DDPSDOJ copied-RAM loop observed after the v46 DMAC fixes.  The accelerator only fires when the exact SH instruction sequence at `0c30b6ae-0c30b6f6` is present and applies the same register, SR.T, stack-variable, and cycle-accounting effects in one host step.  This is a sandbox execution-speed aid so the standalone ANSI C interpreter can continue exposing later MAME-matching issues; it is deliberately not enabled by a loose PC-only check.
## v48 SHAD/SHLD edge cases

Source: MAME `src/devices/cpu/sh/sh4.cpp`, functions `sh34_base_device::SHAD` and `sh34_base_device::SHLD`.

The ANSI C CPU core now mirrors MAME's handling of negative shift counts that are exact multiples of 32 for SHAD/SHLD.

## v50 copied-RAM helper stride/coherency note

v50 does not copy new MAME source. It adjusts sandbox glue around the DDPSDOJ copied-RAM helper at `0c30b6ae-0c30b6f6` so zero/erased table stride reads in this exact verified sequence are treated as missing SH7709S cache/MMU/DMAC coherency. The fallback is intentionally narrow and documented; it is not a MAME framework port and it is not a game-specific title-screen bypass.

## current DDPSDOJ title-screen work

The current title/attract-screen path adds three MAME-aligned fixes and one local compatibility bridge:

- SH7709S port data-register offsets for ports C/D/E/F/J/L now match the MAME SH3 AS_IO map, and CV1000 PORT_J routes only to the FPGA serial loader as in `src/mame/cave/cv1k.cpp` / `src/mame/cave/cv1k_v.cpp`.
- The CV1000 renderer now follows MAME's blend/tint table formulas from `cv1k_v.h` and `cv1k_v_pixel.ipp`, including the optimized non-blend case for source alpha 0x1f plus destination reverse-alpha 0x1f.
- Frame presentation keeps MAME's negative graphics scroll direction from `cv1k_blitter_device::screen_update`.
- The DDPSDOJ auto-blit handoff is original sandbox glue. It reads the game's own command-list base/end pointers from RAM, validates that the list is inside CV1000 work RAM and begins with a recognized CV1000 command opcode, then runs the existing MAME-derived blitter parser bounded by the game's current list end. This bridges a missed MMIO launch in the partial SH7709S/IRQ/cache model; it is not copied MAME code and should be removed once the underlying CPU/platform model is complete.

Relevant MAME source files remain BSD-3-Clause. `cv1k_v.cpp` and `cv1k_v.h` name David Haywood, Luca Elia, and MetalliC; `cv1k_v_pixel.ipp` names David Haywood.

## v52 SDL 1.2 / YMZ770 audio note

v52 adds an SDL 1.2 frontend and a MAME-derived YMZ770C audio output path.  The frontend itself (`src/ui_sdl12.c`) is original sandbox code using SDL 1.2 video, joystick/keyboard input, and an audio ring buffer.

The stereo YMZ770 mixer in `src/ymz770_mame_audio.cpp` follows MAME `src/devices/sound/ymz770.cpp` for phrase offsets, sequence offsets, sequencer wait/end commands, channel key-on/key-off, loop handling, pan/volume scaling, main volume, and clip limiting.  The AMM/MPEG frame decoder in `src/mame_mpeg_audio.cpp` and `src/mame_mpeg_audio.h` is a standalone copy/adaptation of MAME `src/devices/sound/mpeg_audio.cpp` and `src/devices/sound/mpeg_audio.h` with the class renamed and MAME framework dependencies removed.  These files remain BSD-3-Clause-derived material; MAME names Olivier Galibert, R. Belmont, and MetalliC for the relevant YMZ770/MPEG audio files.
