# Remaining path from this sandbox to a real CV1000 emulator

This project is now a stronger porting base, not a finished emulator.

1. Replace `cpu_sh7709s.c` with a complete SH7709S core. The current interpreter covers common data movement, ALU, branches, PC-relative loads, GBR addressing, and some system-register operations, but it is not exception/timer/MMU/cache accurate.
2. Replace or substantially extend `video.c` using MAME's full CV1000 blitter behavior. The current code has upload/draw command shape, full-size VRAM, blend/tint tables, and PPM output, but it does not model all FPGA ports, queue timing, clipping firmware interactions, IRQ timing, or every blend mode precisely.
3. Implement real YMZ770 sample playback. `sound_ymz770.c` is still a register/FIFO capture shim.
4. Extend `nand.c` to full K9F1G08U0M semantics, including robust OOB/bad-block policy and all command corner cases.
5. Replace `rtc9701.c` with a command-complete RTC9701 serial state machine.
6. Add ROM-set validation and per-game machine metadata externally. Do not include commercial ROMs.
7. Add a real frontend if desired. The current UI is a deterministic terminal shell, not an SDL/OpenGL PPSSPP-style UI.
8. Preserve MAME attribution and license headers for any further imported portions.
