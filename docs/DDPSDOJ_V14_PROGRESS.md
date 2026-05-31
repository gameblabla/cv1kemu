# DDPSDOJ v14 progress

v14 does not reach the real title screen. It is a SH7709S MMU/TLB diagnostics pass.

Default 10,000-frame run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 dcache=0/0 stale=0 cachectl=0 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Material change from v13: the emulator now has an explicit TLB shadow, an `LDTLB` hook, and TLB counters in the status line. The counter shows no `LDTLB` load has occurred before the current blocker, so the high virtual pointer seen at the `0c1fb3dc` loop is not yet explained by a software-loaded TLB entry in the partial emulator.

Current blocker: `0c1fb3dc-0c1fb42c`, a four-instruction status loop reading `@(8,R10)`. At the checkpoint, the status field resolves to `0xc298a0bf`, so the unsigned comparison against `5` always branches back.
