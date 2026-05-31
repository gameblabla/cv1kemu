# DDPSDOJ boot progress, v6

This revision does not show the real title screen. It is a boot/probe milestone.

## Confirmed in this build

- The uploaded `ddpsdoj.zip` is accepted as a ZIP ROM set with `u2`, `u4`, `u23`, and `u24`.
- CRCs match the MAME-style manifest embedded in `src/romset.c`.
- U4/U23/U24 use 16-bit word-swap loading, with the final `0x100` bytes ignored.
- U2 is retained as the full `0x08400000` NAND image, including spare/OOB data.
- The program executes for tested runs without illegal SH opcodes and without unmapped bus accesses at the documented checkpoints.
- FPGA firmware upload activity is observed and tracked through 2,323,240 bits.
- Minimal DMAC emulation advances the boot through NAND-to-RAM transfers.
- v6 gets past the v5 `0c30b71a` failure loop through one explicit compatibility assist.

## v6 10,000-frame result

```text
model=CV1000-D pc=0c30b6b0 sr=40000101 frames=10000 cycles=216899525 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=156031 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=74 dma_bytes=156029 assists=1 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

## v5-to-v6 delta

v5 stopped at the deliberate failure self-loop:

```text
PC=0c30b71a, DMA=64, DMA bytes=135168
```

v6 applies one documented allocator/table compatibility assist at `0c30cba0`, then reaches a later loop around `0c30b6b0` / `0c30b6f4` with:

```text
DMA=74, DMA bytes=156029
```

The assist is a temporary bridge around missing SH7709S cache/MMU/TLB and side-effect behavior. It should be removed once those devices are implemented accurately.

## Useful commands

```sh
make
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --run-frames 10000
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --probe-title --run-frames 10000 --dump-ppm ddpsdoj_v6_progress.ppm
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --run-frames 7000 --run-break-pc 0x0c30cba0 --trace-steps 64
```

## v9 checkpoint

v9 adds a post-FPGA blitter-ready readback, SH-3 banked-register fixes from v7, and a deterministic fast path for the large DDPSDOJ byte-fill loop at `0c1d7cfc`.

10,000-frame result for the uploaded ROM set:

```text
model=CV1000-D pc=0c1d7636 sr=40000101 frames=10000 cycles=116851314 illegal=198 last_illegal=0c1d8376:0001 irq_ack=0 nand=138412032B nand_r=425664 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=202 dma_bytes=425662 last_dma=b0000000>0c1d767e/2112/00004421 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 unmapped_r=8 last_r=cc1d7def unmapped_w=4 last_w=43c142ef:b8
```

This is not a title screen.  It is a later boot blocker in RAM code loaded by NAND/DMAC.

## v10 checkpoint

v10 adds 0x200-byte fetch-side cache prefill, PC-relative literal-pool reads through the fetch cache, broader SH virtual work-RAM aliases, and one counted high-alias blank-code guard. It reaches 3,244 DMA transfers and 6,850,366 copied bytes at 10,000 frames with zero illegal opcodes:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

The current blocker is the copied-RAM table/status wait around `0c1fb3dc-0c1fb42c`.

## v11 checkpoint

v11 keeps the default clean v10 boot checkpoint and adds fetch-aware tracing plus an off-by-default aggressive status-loop assist.

Default result:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Aggressive result:

```text
model=CV1000-D pc=0c1fe272 sr=40000100 frames=10000 cycles=120119125 illegal=1673 last_illegal=0c1fe270:8794 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=10 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=145623671/98 unmapped_r=493 last_r=6d05a38b unmapped_w=12 last_w=0d000003:c4
```

## v13 checkpoint

v13 is a correctness/diagnostics pass rather than a title-screen breakthrough. The default 10,000-frame checkpoint remains the v12 copied-RAM status loop, but the NAND command model now handles the observed `0x50` spare-area command without inheriting stale address-count state from the preceding `0x00/0x30` read.

Default result:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 dcache=0/0 stale=0 cachectl=0 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Strict cache-control experiment:

```text
model=CV1000-D pc=0c1fa292 sr=40000100 frames=10000 cycles=120121135 illegal=4473 last_illegal=0c1fa296:ffbc irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=145625450/155 dcache=0/0 stale=0 cachectl=1 unmapped_r=31 last_r=19fffeef unmapped_w=4 last_w=3442e06f:da
```

This confirms that the existing progress still depends on an intentionally simplified fetch-cache compatibility model. Honoring cache-control operations naively is not sufficient; the next real work is a fuller SH7709S cache/TLB/MMU model and exact DMAC/NAND coherency behavior.


## v15

Added `--wide-p0-alias`, `--dump-ram`, and a new status `widep0=` field. The default DDPSDOJ path remains at `0c1fb3e0`; the experimental `--dcache --wide-p0-alias` path reaches 20,447 DMA transfers / 43,183,102 bytes with no unmapped bus accesses before stopping at `0c30b71a`.

## v17

v17 adds a gated DDPSDOJ NAND-copy status assist for the `0c30b716/0c30b71a` helper loop, plus a debug-path fix so `--run-break-pc` advances the synthetic vblank word through the normal bus path instead of bypassing the experimental data-cache model.

Default 10,000-frame result remains the conservative v15-style checkpoint:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 ... dma=3244 dma_bytes=6850366 ... unmapped_w=0
```

Experimental v17 command:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --run-frames 8000
```

Verified result:

```text
model=CV1000-D pc=0c1d7cfc sr=40000101 frames=8000 cycles=175642745 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=116064000 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=54955 dma_bytes=116063998 last_dma=b0000000>1301f6be/2112/00004421 assists=548 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=207180859/96 dcache=76060841/162150 stale=815328 cachectl=0 widep0=1 tlb=0/0/428012602:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

This is not a title-screen milestone. At longer runs the experimental bridge eventually falls into copied payload/invalid control flow after the near-full NAND copy, which means the underlying SH7709S/DMAC/NAND/cache model is still wrong.

## v17

v17 adds a SH P2/uncached-address bypass for the experimental data cache, protects known physical CV1000 I/O windows from the broad `--wide-p0-alias` shortcut, adds an aggressive invalid-target diagnostic guard, and adds the optional `--nand-data-only` NAND spare/OOB read experiment.

Default 10,000-frame behavior is unchanged from the v14-v16 baseline: the boot/probe path stops at `0c1fb3e0` with 3,244 DMA transfers and no illegal opcodes. The experimental 8,000-frame checkpoint still reaches 54,955 DMA transfers and 116,063,998 copied NAND bytes with no illegal opcodes. The 10,000-frame experimental path still falls into payload-derived control flow around `df42d3c0`.

## v18

v18 increases the instruction-fetch cache geometry from 8,192 to 65,536 halfword entries and adds a narrowly gated aggressive exception bridge for the observed payload-derived invalid opcode targets. The default conservative result remains the same v14-v17 stop at `0c1fb3e0`. The experimental 10,000-frame path now stays in copied CV1000 work RAM around `0c1d928e` instead of falling through the high invalid `df42d3c0` target, and reduces the illegal/unmapped fallout, but it still does not reach the title screen.

## v23

Focus: memory mapping, NAND status/address-cycle behavior, and CPU correctness. Added a correct TAS.B test/set behavior, NAND ready/not-write-protected status, legacy 01h read-area handling, and an off-by-default compact 0x40000000 alias diagnostic. Default DDPSDOJ progress remains at the 0c1fb3dc copied-RAM status loop; compact alias gets past that loop but later diverges, so it remains experimental.
