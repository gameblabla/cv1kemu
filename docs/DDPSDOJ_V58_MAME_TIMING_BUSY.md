# DDPSDOJ v58 MAME timing / blitter-ready pass

This pass removes another source of post-title graphical corruption by aligning
more of the standalone timing path with MAME's CV1000 driver.

Changes:

- Added `CV1K_CYCLES_PER_VBLANK` from MAME's CV1000 board timing: SH7709S at
  102.4 MHz and the screen pulse at 60.024 Hz, or about 1,705,984 CPU cycles per
  vblank.
- Replaced the older approximate 3,332,000-cycle vblank divisor in the frame
  runner and run-break helper.
- Changed the synthetic DDPSDOJ speedup tick to advance by one vblank tick rather
  than by a 256-count shortcut once graphics exist. MAME's speedup waits until an
  interrupt; it does not artificially jump the game's frame counter.
- Added a MAME-style delayed blitter-ready state. The sandbox still executes the
  copied command list synchronously, but register `0x18000010` now remains busy
  for a CPU-cycle-scaled version of the MAME blitter delay estimate instead of
  becoming ready immediately.
- Removed the DDPSDOJ-specific post-start exception that allowed guessed RAM-list
  replay after a real visible MMIO blit had already occurred. MAME only launches
  CV1000 blits from the blitter execute register at `0x18000004`.

Validation notes:

- The long coin-hold repro that previously could replay the bad `0x051000` RAM
  fallback list no longer overwrites the title with the grey/white atlas frame.
- The post-start path is still not a clean gameplay transition. With the guessed
  RAM-list replay removed, the emulator remains dependent on the incomplete
  SH7709S/cache/IRQ/MMIO path to reach later real blitter execute writes.

The remaining work is still CPU/peripheral scheduling, not another CV1000 pixel
formula change.
