# DDPSDOJ v49 progress note

v49 remains a diagnostic/fidelity patch. It does not reach the title screen.

## Change

The v47 copied-RAM loop accelerator for the helper at `0c30b6ae-0c30b6f6` now uses the same fetch-side view as the SH interpreter. The instruction guard and PC-relative literal fetch now use `cv1k_bus_fetch16()` rather than normal data reads.

This matters because the current standalone SH/cache approximation can still execute cached copied-RAM helper code while the backing RAM dump at the literal address looks erased. The observed path resolves a literal pointer to `0c7f83c0`, then reads `[0c7f83c8] == 0xffffffff`; the bad stride makes the loop advance incorrectly and prevents real draw-list execution.

A fallback that substitutes the stack-held stride for erased `0xffffffff` table data is available only with `--aggressive-assists`. It is intentionally not part of the conservative verification path.

## Verification

Default 10,000-frame endpoint:

```text
model=CV1000-D pc=0c30b6ae frames=10000 illegal=0 nand_r=7115757 dma=3245 blit_ops=1 up=1 draw=0 assists=3188
```

MAME-TRAPA plus MAME-speedup endpoint:

```text
model=CV1000-D pc=0c1d1346 frames=10000 illegal=0 nand_r=307769 dma=152 blit_ops=7 up=7 draw=0 mspeed=1/4469 assists=95
```

The important video-side diagnostic is still `draw=0`.
