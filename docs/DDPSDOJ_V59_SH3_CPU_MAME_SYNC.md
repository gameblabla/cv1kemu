# DDPSDOJ v59 SH3 CPU / MAME sync pass

This pass targets the post-start execution path rather than the CV1000 pixel
renderer.  The title/coin graphical corruption remains suppressed by the v58
MMIO-only blitter path; the remaining blocker is still that the standalone SH3
scaffold does not reliably reach later game-issued blitter launches.

Changes:

- SH delayed branches now suppress IRQ acceptance while executing the delay-slot
  instruction.  MAME's SH3/SH4 core generates the delay slot as part of the
  branch/RTE path; an interrupt is not allowed to replace the delay-slot opcode
  and then return to a branch target chosen by the outer instruction.
- `RTE` keeps the same delayed-slot guard while preserving the existing MAME-style
  order: execute slot in the current exception context, then restore `SSR->SR`
  and `SPC->PC`.
- PC-relative `MOV.W @(disp,PC),Rn` and `MOV.L @(disp,PC),Rn` now use data-side
  program reads instead of the opcode-fetch cache helper.  MAME treats literal
  pool reads as memory/data accesses; the sandbox should not force the instruction
  cache path for those operands.
- The transient delay-slot IRQ guard is intentionally not serialized in save
  states, preserving compatibility with earlier v50+ state files.

Validation notes:

- Clean default `make` succeeds.
- Loading the v58 coin/title states remains visually stable; the old white/font
  atlas replay does not return.
- Correct `p1_b1` scripted input is accepted and changes the SH execution path,
  but the emulator still remains on the post-start/rank overlay instead of a
  clean live gameplay frame.
- Real blitter MMIO execute count remains at the earlier visible launch in the
  tested post-start state.  The next required work remains SH7709S/cache/IRQ/MMIO
  scheduling, not a CV1000 blend formula.
