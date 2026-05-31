# DDPSDOJ v22 progress

v22 does not reach a real game title screen. It is a diagnostics pass focused on the requested priority areas: memory mapping, NAND/OOB behavior, CPU/DMAC, and IRQ/timer scaffolding.

## What changed

- Added a physical NAND/OOB scan after loading the DDPSDOJ U2 image. The scan reports total blocks, empty blocks, first/second-page OOB-marker candidates, and pages with non-FF spare data. This is deliberately labeled diagnostic rather than definitive bad-block repair because the Cave dumps retain ECC/spare bytes.
- Added per-DMA NAND source diagnostics: the last DMAC transfer now records the NAND page, block, and column before and after the transfer.
- Added RTC offset-3 NAND CE tracking. The line is recorded for diagnostics but not allowed to gate reads/writes yet, matching the practical note in the MAME driver that the shared bus CE handling is unlikely to matter for emulation at this layer.
- Added `--nand-scan` to print the physical NAND map summary.
- Added `--vblank-irq-and-tick` to test IRQ2 requests while keeping the synthetic vblank RAM tick. This exposed that the current SH IRQ/vector model still blocks early boot, so the default path remains synthetic-tick only.
- Bumped save states to `CV1KSS22` and preserved the new NAND/DMAC diagnostic state.

## Verified conservative run

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 nand_r=6850368 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9805 nb=153-153 nc=0-0 nmap=1024/454/568/36309 unmapped_r=17 unmapped_w=0
```

## Verified experimental run

```text
model=CV1000-D pc=0c1d7cfc sr=40000101 frames=8000 cycles=175642745 illegal=0 nand_r=116064000 dma=54955 dma_bytes=116063998 last_dma=b0000000>1301f6be/2112/00004421/m01/00004422 np=34507-34508 nb=539-539 nc=0-0 nmap=1024/454/568/36309 unmapped_r=0 unmapped_w=0
```

## NAND scan

```text
nand-scan blocks=1024 empty=454 oob_marked=568 spare_non_ff_pages=36309 size=138412032
```

The scan result reinforces that NAND reconstruction/OOB handling remains a major blocker. The last conservative DMA is around physical page 9804/block 153. The long experimental path reaches page 34508/block 539.

## Current blocker

The conservative path still loops around `0c1fb3dc-0c1fb42c` because a high virtual work-RAM pointer resolves to payload-looking data instead of a small status/index value. The experimental path reaches much later NAND copying, but still diverges before graphics/title initialization. The remaining priority work remains: SH7709S cache/TLB/MMU behavior, exact DMAC/cache coherency, NAND bad-block/OOB reconstruction, and a real IRQ/timer model.
