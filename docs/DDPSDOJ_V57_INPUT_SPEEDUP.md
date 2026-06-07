# DDPSDOJ v57 input/speedup synchronization notes

This pass follows the same Cave CV1000 / SH7709S behavior used by MAME, but fixes an integration problem in the standalone runner.

MAME's DDPSDOJ driver uses an idle-loop optimization equivalent to `spin_until_interrupt()` at the vblank wait address. That is safe in MAME because the scheduler owns input sampling, interrupt delivery, and device wakeups. In this standalone emulator, scripted and SDL JAMMA inputs are sampled outside that scheduler model. If the SH core is suppressed at the idle PC while a coin/start bit is asserted, the game can miss or substantially delay the live port sample that should happen after the next vblank wake.

v57 changes the MAME-style idle detector so it is disabled while any JAMMA input is currently asserted. This lets the normal CPU loop observe the active-low port state and reach the post-title code path instead of spinning through the synthetic vblank shortcut during the input edge.

The frontend now also reapplies runtime execution options after loading a savestate. A savestate should restore emulated machine state, but flags such as `--dcache`, `--mame-speedup`, `--strict-cache-ops`, `--vblank-irq-and-tick`, and alias/debug modes are current frontend controls. Reapplying them after load makes title-screen validation states behave consistently.

Validation from the 60k-frame title state:

- `coin1` from title reaches a stable credited title state without the previous white/font-atlas overwrite.
- `p1_start` from that credited state is read through the live PORT_C path and advances the SH PC into the later post-title code path.
- The remaining visible frame still does not become a clean gameplay frame. The emulator continues to depend on the DDPSDOJ RAM-list fallback because later real blitter execute writes are not yet observed from the incomplete SH7709S/cache/IRQ/peripheral path.

Observed remaining blocker:

- Repeated CPU reads from P1 alias `8b1ae8eb` translate to physical `0b1ae8eb`, which is unmapped in the MAME CV1000-D program map. This appears to be a symptom of the current incomplete SH execution/cache/peripheral state rather than a CV1000 pixel-rendering blend issue.
