# DDPSDOJ v56 MAME video/list follow-up

This pass continues from v55 and focuses on the visible title/coin/start path.

## MAME-derived corrections

- `gfx_upload` no longer wraps every uploaded pixel through the draw-source VRAM indexer.  MAME masks the starting co-ordinates and then writes via a linear bitmap row pointer, so the sandbox now advances the upload stream with the original command dimensions and writes to a linear VRAM index rather than `vram_index(dst_x + px, dst_y + py)`.
- The DDPSDOJ RAM-list fallback remains bounded and still executes through the MAME-derived CV1000 command parser.  It now rejects short/partial post-visible lists, preventing the title/coin font-atlas corruption while allowing larger post-start lists built by the game to be tested.
- Removed the pre-existing unused `ss` local from the SH7709S interpreter build path.

## Validation

Validated from the v54/v55 title state with:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip \
  --dcache --mame-speedup --strict-cache-ops --vblank-irq-and-tick \
  --load-state /mnt/data/fix_v54/run/s60000.ss \
  --run-frames 1000 --save-state title.ss --dump-ppm title.ppm

./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip \
  --dcache --mame-speedup --strict-cache-ops --vblank-irq-and-tick \
  --load-state title.ss --tap-input coin1,0,3000 \
  --run-frames 3000 --save-state coin.ss --dump-ppm coin.ppm

./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip \
  --dcache --mame-speedup --strict-cache-ops --vblank-irq-and-tick \
  --load-state coin.ss --tap-input p1_start,0,3000 \
  --run-frames 5000 --save-state start.ss --dump-ppm start.ppm
```

Result: the title and coin screens no longer get overwritten by the white/font-atlas frame, and the larger post-start list renders the rank/start overlay cleanly.

## Remaining blocker

The build still does not reliably enter a live gameplay frame.  P1 start is read active-low and the program advances into the post-start code path, but the sandbox still misses later real MMIO blitter launches because the SH7709S/cache/IRQ scheduling model is incomplete.  v56 improves the visible post-start graphics without masking individual sprite IDs or hardcoding a gameplay frame.
