# DDPSDOJ title/coin/start validation notes (v53)

This pass targeted the title-screen credit/start path and additional visible DDPSDOJ graphics issues.

## Code changes

- Corrected the MAME-derived CV1000 fixed-alpha multiply order used by the renderer.  MAME's `clr_t::mul_fixed(val, clr)` indexes the color table as `colrtable[val][component]`, not `colrtable[component][val]`.
- Added explicit ANSI C translations of the MAME `cv1k_v_pixel.ipp` special source-mode paths for blend source modes 0 and 2.  These modes have destination-mode-specific formulas in MAME and cannot be represented accurately by the previous generic `left + destination` helper.
- Removed temporary PORT C diagnostic logging/counters used during the coin/start investigation.

## Validation commands

The tested headless flags were:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip \
  --dcache --mame-speedup --strict-cache-ops --vblank-irq-and-tick \
  --load-state /mnt/data/work/s70_current.ss \
  --tap-input coin1,0,3000 --tap-input p1_start,3000,2000 \
  --run-frames 8000 --dump-ppm /mnt/data/task_v53/v53_coin_start.ppm
```

After the blend correction the coin path visibly advances to `CREDIT 01`.  A second run from that state with P1 Start held visibly decrements the credit to `CREDIT 00`, confirming that both coin and start are now accepted by the game logic.

## Remaining graphics/gameplay blockers

Gameplay is not yet visibly reached.  After P1 Start is accepted, the emulated program state and blit-list addresses change, but the frame remains visually stuck on a mostly white title/credit layer.  Additional input after start can corrupt the title layer rather than drawing the expected next screen.

The test menu remains reachable, but entering `INPUT TEST` still draws only the heading.  The input-test body/list is missing.  Disabling the DDPSDOJ auto-blit duplicate-list skip did not change this, so the remaining problem is more likely in the copied overlay/VRAM source population, transparency/color conversion, or the incomplete SH7709S/cache/DMAC/MMIO handoff rather than the skip guard alone.

Observed remaining graphical issues:

- White/washed title background after credit/start transitions.
- Missing input-test body under the `INPUT TEST` heading.
- Post-start title-layer tearing/corruption when more player inputs are sent.
- Many post-start draw operations reference VRAM source regions that are currently transparent or not populated as expected.

This package therefore improves title credit handling and fixes a real MAME-derived pixel-blend mismatch, but it does not claim fully playable ingame DDPSDOJ yet.
