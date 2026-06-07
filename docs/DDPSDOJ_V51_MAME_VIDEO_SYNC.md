# DDPSDOJ v51 MAME video synchronization pass

This pass uses the local `mame-master` CV1000 video implementation as the reference for renderer behavior.  The sandbox remains an ANSI C CV1000/DDPSDOJ emulator scaffold, not a full MAME port.

## Implemented changes

- Matched MAME's CV1000 blitter execution clip rectangle semantics: `clip_origin - 32` through `clip_origin + visible_size - 1 + 32`, intersected with the sandbox VRAM bounds.
- Replaced the simplified component blend path with an ANSI C translation of MAME's generated `cv1k_v_pixel.ipp` blend phases, including the source-mode and destination-mode cases.
- Preserved the source transparency bit after blended writes, matching MAME's `to_pen() | (pen & 0x20000000)` behavior.
- Matched MAME's normal-tint bypass: the 0x80 tint byte converts to 0x20 and is not treated as an active tint multiplier.
- Added the MAME source-X wrap guard used by the generated blitter loops.
- Added a signature check to the local DDPSDOJ auto-blit handoff so unchanged game-built lists are not executed redundantly.

## Validation notes

Build command:

```sh
make clean && make -j2
```

Observed compiler output is clean except for the pre-existing unused `ss` variable warning in `src/cpu_sh7709s.c`.

Test command used to reach the shown DDPSDOJ attract/title state:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --dcache --mame-speedup --strict-cache-ops --vblank-irq-and-tick --run-frames 70000 --dump-ppm ddpsdoj_v51.ppm
```

The PPM is still the raw CV1000 raster; rotate it in a frontend/image tool for normal portrait viewing.

## Remaining limitation

The basic MAME PORT_C mapping is present and the CPU reads the active-low coin/start bits, but I did not verify a successful coin/start transition into gameplay in this sandbox.  The likely remaining work is outside the pixel renderer: the incomplete SH7709S/cache/IRQ/MMIO path and the local DDPSDOJ auto-blit handoff still diverge from a complete MAME execution path.
