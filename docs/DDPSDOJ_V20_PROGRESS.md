# DDPSDOJ v20 progress

v20 focuses on memory mapping, NAND/DMAC-visible CPU behavior, and IRQ/cache diagnostics. RTC, YMZ770, and full FPGA/blitter accuracy were not the focus of this pass.

## Changes

- Added address-translation counters in the status line: P1/P2 aliases, P4/internal-register aliases, wide P0 aliases, low P0 work-RAM aliases, `0x40000000` work aliases, and `0xe0000000` high aliases.
- Added `--dma-cache-sync`, an opt-in DMAC/cache coherency experiment that invalidates simplified data-cache and instruction-fetch-cache lines when DMAC writes touch a new line/block.
- Added `dmasync=` and `dmainv=` status fields.
- Fixed numeric command-line parsing for debugging options. Bare hex values such as `0c1d928e` now parse as hex, not decimal zero. This fixes ambiguous `--break-pc`, `--run-break-pc`, `--dump-ram-addr`, and `--blit` use.
- Bumped save states to `CV1KSS20` to include the new diagnostic/cache-sync fields.

## Conservative run

Command:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --run-frames 10000
```

Result is still the same conservative boot checkpoint: no illegal opcodes, 3,244 DMA transfers, and 6,850,366 bytes copied from NAND.

## Experimental run

Command:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --run-frames 8000
```

This still reaches the long experimental checkpoint: no illegal opcodes, 54,955 DMA transfers, and 116,063,998 bytes copied from NAND.

## DMAC/cache-sync experiment

Command:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --dma-cache-sync --run-frames 8000
```

This intentionally diverges earlier: it reaches only 202 DMA transfers and then falls into copied-code invalid opcode behavior. That result is useful because it shows the previous stale-cache behavior is required for this partial boot path; a real fix needs proper SH7709S cache-control and DMAC coherency instead of blanket invalidation.

## Current blocker

The emulator still does not reach a real title screen. The live blockers remain SH7709S MMU/TLB/cache behavior, exact DMAC/cache coherency, NAND OOB/bad-block reconstruction, and a real IRQ/timer model.
