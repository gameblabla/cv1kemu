# DDPSDOJ v19 progress

Scope: memory mapping, NAND, CPU, and IRQ. RTC, YMZ770, and the full FPGA/blitter remain low-priority in this pass.

## Changes

- SH IRQ event selection now uses an IRQ-line stride (`0x600 + level * 0x20`) instead of always entering the IRQ0 vector. The IRQ ack path records the event into the SH internal register shadow for deterministic tracing. The pure `--irq2` path still does not boot correctly because the copied RAM IRQ vector/handler setup is incomplete, but it now fails with visible `irq_req`/event diagnostics rather than silently using the wrong vector.
- The SH DMAC scaffold now decodes the observed CHCR address-mode bits and records source/destination modes. For DDPSDOJ's dominant `00004421` transfer this decodes as source fixed / destination increment (`m01`), matching the NAND-data-port-to-RAM copy pattern. The CHCR completion value now preserves mode bits and sets TE (`00004422`) instead of collapsing all completions to `00000002`.
- Status lines now expose `last_dma=.../chcr/modes/status` and `irq_req=count/level/event` so long boot runs can be compared without a full trace.
- Save states are now `CV1KSS19` and include the new DMAC/IRQ diagnostic state.

## Verified conservative run

```
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --run-frames 10000
```

Result: still stops at the known copied-RAM/status loop, with zero illegal opcodes.

```
model=CV1000-D pc=0c1fb3e0 ... dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 irq_req=0/0/00000000 assists=9 ...
```

## Verified experimental run

```
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --run-frames 8000
```

Result: still reaches the long NAND-copy path with zero illegal opcodes at 8,000 frames.

```
model=CV1000-D pc=0c1d7cfc ... dma=54955 dma_bytes=116063998 last_dma=b0000000>1301f6be/2112/00004421/m01/00004422 irq_req=0/0/00000000 assists=548 ...
```

## Current blockers

The title screen still does not render. The remaining hard blockers are SH7709S cache/TLB/MMU/DMAC coherency, NAND bad-block/OOB reconstruction, and a real IRQ/timer model. The pure IRQ2 path now records the correct line/event shape, but the current RAM/vector state is still not good enough to replace the synthetic vblank tick.
