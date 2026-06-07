# DDPSDOJ v54 MAME video recheck

Scope: rechecked the sandbox CV1000 renderer against MAME's `src/mame/cave/cv1k_v.cpp`, `cv1k_v.h`, `cv1k_v_in.ipp`, and `cv1k_v_pixel.ipp` after the title/coin/start tests showed obvious visual corruption.

Changes made:

- The active blitter clip rectangle now keeps MAME's signed `origin - 32 .. origin + visible - 1 + 32` extent instead of clamping the top/left edge to zero. Off-VRAM writes are still discarded by the sandbox memory bounds check, but the clip decision now matches MAME before the finite VRAM guard.
- RGB555-to-output conversion now matches MAME `clr_t::to_pen()`: 5-bit channels are shifted into the high output bits without low-bit replication.
- MAME's two different fixed-multiply call orders are now separated: `clr_t::mul_fixed()` uses `table[alpha][source]`, while generic destination mode 0 uses MAME `add_with_clr_mul_fixed()` semantics, `table[destination][alpha]`.
- Save-state video clip fields were adjusted to preserve signed clip origins while keeping the existing 32-bit on-disk field width.
- Status output now prints signed clip origins.

Validation performed:

- Clean default build with `make` succeeds. The only remaining warning is the pre-existing unused `ss` variable in `src/cpu_sh7709s.c`.
- Reloaded the v53 60k-frame title state and captured a title frame.
- Inserted a coin from that state and captured the resulting frame.
- Held service/test from that state and captured the service menu.

Observed result:

- The service menu body renders, so the earlier missing-menu-body symptom is not reproduced after this pass.
- The title/character-select screen still renders before coin input.
- The coin-triggered white/atlas corruption still occurs. This is not explained by the pixel blend formulas after the MAME recheck.

Current blocker:

The corrupt coin frame is produced when the sandbox's DDPSDOJ auto-blit fallback executes a RAM list beginning at `0x051000`. That list contains repeated 32x32 transparent tinted draws from VRAM source `0x0300,0` across the screen, followed by text draws. In this sandbox run, source page `0x0300,0` is an atlas/font/source page, not a composed frame page, so replaying that list as a full-frame auto launch turns the display white/atlas-like. MAME only executes lists that the game launches through the blitter MMIO path; the sandbox fallback exists because the incomplete SH/cache/IRQ path can miss later launches. Fixing the remaining corruption requires fixing the launch/MMIO/cache/CPU path or replacing the fallback with a verified hardware-equivalent trigger. A game-specific skip for that list was tested and rejected because it only masks one symptom and does not produce a correct ingame transition.
