# DDPSDOJ boot progress, v11

v11 does not reach the real title screen. It is a diagnostic pass over the v10 copied-RAM status-loop blocker.

## Default 10,000-frame result

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

This is the same clean boot checkpoint as v10: zero illegal opcodes, 3,244 DMA transfers, and 6,850,366 bytes transferred from NAND by the 10,000-frame checkpoint.

## Fetch-aware trace

Use:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --run-frames 10000 --save-state v11.ss
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --load-state v11.ss --trace-fetch --trace-steps 16
```

The fetch-side trace confirms that the loop is executing cached copied-RAM code, not the overwritten backing bytes. The repeated fetched sequence is:

```text
0c1fb3dc: 52a2
0c1fb3de: e805
0c1fb3e0: 3286
0c1fb3e2: 8922
0c1fb42a: 2cc8
0c1fb42c: 8bd6
```

That corresponds to loading a word through a high virtual work-RAM pointer, comparing it with five, and branching while the status/table value remains above the expected small range.

## Aggressive assist experiment

`--aggressive-assists` writes zero to the one observed status field at the current blocker. It is not enabled by default because it reaches later copied-RAM/payload bytes and then executes garbage:

```text
model=CV1000-D pc=0c1fe272 sr=40000100 frames=10000 cycles=120119125 illegal=1673 last_illegal=0c1fe270:8794 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=10 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=145623671/98 unmapped_r=493 last_r=6d05a38b unmapped_w=12 last_w=0d000003:c4
```

This indicates that the current blocker is not safely bypassable. The real fixes remain SH7709S TLB/cache-control semantics, DMAC transfer mode accuracy, NAND bad-block/OOB reconstruction, and missing peripheral/IRQ/timer side effects.
