# DDPSDOJ v33 verification

v33 continues matching the standalone ANSI C CV1000 sandbox against the uploaded MAME source tree.

## MAME-matching changes

The v33 patch fixes four concrete CV1000 blitter mismatches found against MAME's `src/mame/cave/cv1k_v.cpp`, `src/mame/cave/cv1k_v.h`, and generated `cv1k_v_in.ipp` path:

- `0xc000` clip commands now advance the command-list pointer by four bytes: opcode word plus parameter word. The MAME timing byte count remains `CV1K_CLIP_OPERATION_SIZE_BYTES = 2`; that constant is not the command stride.
- Draw destination coordinates now use full 16-bit sign extension, matching `util::sext<int>(dst_x_start, 16)` and `util::sext<int>(dst_y_start, 16)` in MAME's `gfx_draw`.
- Tint conversion now follows MAME's `tint_to_clr` rule, `raw >> 2`. This maps raw `0x80` to the normal `0x20` multiplier and preserves raw zero as a real zero tint.
- Screen presentation now applies negative scroll, matching MAME's `copyscrollbitmap` call with `-m_gfx_scroll_x` and `-m_gfx_scroll_y`.

Save states are bumped to `CV1KSS33`.

## Verification

Default 10,000-frame run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=80 hpen=0 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366
```

MAME-TRAPA plus MAME-speedup diagnostic run:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 mspeed=1/4494
```

The supplied `ddpsdoj.zip` loaded successfully and CRCs matched the embedded MAME-derived manifest for `u2`, `u4`, `u23`, and `u24`.

## Result

v33 still does not reach the DDPSDOJ title screen. The run remains blocked before any draw operations execute. The change improves fidelity in the blitter path that will matter once the SH7709S/platform side reaches later video command lists, but it does not solve the remaining SH7709S MMU/TLB/cache/exception/timer/IRQ and DMAC/cache-coherency blockers.
