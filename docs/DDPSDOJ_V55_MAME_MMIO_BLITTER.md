# DDPSDOJ v55 MAME blitter/MMIO correction

This pass removes the renderer-side cause of the title/coin white-atlas corruption.

MAME reference used:

- `src/mame/cave/cv1k_v.cpp`
- `src/mame/cave/cv1k_v.h`

Relevant MAME behavior:

- CV1000 rendering starts from `cv1k_blitter_device::blitter_w()` offset `0x04` only.
- The command-list pointer is stored at offset `0x08` and is masked with `0x1fffffff` when the blitter starts.
- MAME does not periodically replay a game-specific RAM range as a command list.

Changes:

- Added MMIO-launch accounting to the CV1000 video device.
- Changed the execute-register path to preserve MAME's full `0x1fffffff` address form before the RAM mask is applied.
- Restricted the DDPSDOJ fallback RAM-list replay so it is used only as a pre-visible bootstrap crutch. Once a real MMIO blitter launch has produced a visible frame, guessed RAM-list replay is disabled.
- Added save-state compatibility handling so old title-screen states are treated as past the bootstrap phase and do not replay the guessed list on load.

Validation:

- Default `make` builds successfully.
- A title-screen state followed by coin input no longer corrupts the title layer into the white/font-atlas frame.
- The remaining failure is not a pixel/blend bug: after disabling guessed replay, the current SH/cache/IRQ path still does not reliably advance the visible credit/start state from old title-screen states. That must be fixed in CPU/peripheral execution rather than by replaying DDPSDOJ-specific RAM lists.
