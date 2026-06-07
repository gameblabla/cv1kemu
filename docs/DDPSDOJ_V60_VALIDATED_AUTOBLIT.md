# DDPSDOJ v60 validated auto-blit fallback

This build keeps the CV1000 pixel parser aligned with the MAME-derived command
format, but makes the local DDPSDOJ fallback less blunt.

MAME's `cv1k_blitter_device` starts work from the 0x18000004 execute register.
The sandbox SH7709S/cache/IRQ path can still miss later execute writes after the
title/rank transition, so the existing fallback reads DDPSDOJ's game-built RAM
command-list pointers at `0x0c1d1454` and `0x0c1d1460`.

v58/v59 disabled all post-visible fallback lists after a real MMIO frame to stop
the old white/font-atlas title corruption.  That was safe but too aggressive: it
also blocked a valid post-start list at `0x051000..0x058e94`.  Manually blitting
that list proved that it contains a normal CV1000 command stream and displays the
rank/ship overlay.

v60 replaces the fixed size cutoff with a bounded validator:

- Parse the RAM range as CV1000 draw/upload/clip commands using the MAME-derived
  operation layout.
- Reject malformed or too-small ranges.
- Reject the observed font/atlas overwrite pattern: repeated 32x32 draws from
  VRAM source `0x0300,0` dominating the list.
- Require a non-trivial number of draw operations and source variation before
  accepting a post-visible replay.

This lets the emulator naturally execute the game-built `0x051000` post-start
list without requiring `--blit 0x51000`, while still avoiding the earlier
coin/title atlas overwrite.

Validation performed in the sandbox:

- Clean default `make` succeeds.
- Loading the v59 post-start state and running 32 frames now executes the
  validated `0x051000` list via the fallback (`autoblit=1`) and renders the
  post-start/rank/ship overlay.
- Pressing P1 Button 1 from that state updates the overlay through subsequent
  validated lists instead of leaving the display frozen.
- Coin insertion from the title state remains stable and does not restore the
  white/font-atlas corruption.

Remaining blocker:

This is still not a final hardware-accurate fix for gameplay.  It improves the
bounded fallback while the deeper SH7709S/cache/IRQ/MMIO scheduling issue remains:
the program should be reaching real blitter execute writes instead of relying on
any DDPSDOJ RAM-pointer replay.
