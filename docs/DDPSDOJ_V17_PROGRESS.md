# DDPSDOJ v17 progress note

v17 is not a title-screen milestone. It is a bus/cache/NAND diagnostics pass over v16.

Verified default command:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --run-frames 10000
```

Result:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 dcache=0/0 stale=0 cachectl=0 widep0=0 ndata=0 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Verified experimental command:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --probe-title --run-frames 8000 --dump-ppm docs/DDPSDOJ_V17_PROGRESS.ppm
```

Result:

```text
model=CV1000-D pc=0c1d7cfc sr=40000101 frames=8000 cycles=175642745 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=116064000 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=14 dma=54955 dma_bytes=116063998 last_dma=b0000000>1301f6be/2112/00004421 assists=548 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=207199361/96 dcache=71352699/10068 stale=815296 cachectl=0 widep0=1 ndata=0 tlb=0/0/428012626:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

The v17 experimental path preserves the v16 8,000-frame checkpoint while changing the data-cache behavior so P2/uncached SH addresses bypass the experimental cache. It also protects known physical CV1000 I/O windows from the broad `--wide-p0-alias` RAM shortcut.

A new `--nand-data-only` experiment returns `0xff` for U2 spare/OOB bytes during normal NAND data reads. It is off by default. In this ROM set it does not move the title-screen blocker; the 10,000-frame experimental run still diverges into payload-derived control flow at `df42d3c0`.

Remaining blockers are unchanged: exact SH7709S cache/TLB/MMU behavior, DMAC transfer/coherency details, NAND bad-block/OOB reconstruction, IRQ/timer/peripheral behavior, RTC9701 protocol, YMZ770 playback, and complete FPGA/blitter behavior.
