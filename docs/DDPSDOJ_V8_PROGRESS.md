# DDPSDOJ v8 progress

v8 still does not reach the real game title screen.  It advances beyond v7 by asserting the post-FPGA blitter-ready status bit, accelerating the large byte-fill loop at `0c1d7cfc`, and reaching a later NAND/DMAC-loaded code path.

## Verified command

```sh
make
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --probe-title --run-frames 10000 --dump-ppm docs/DDPSDOJ_V8_PROGRESS.ppm
```

## Result

```text
model=CV1000-D pc=0c1d7636 sr=40000101 frames=10000 cycles=116851314 illegal=198 last_illegal=0c1d8376:0001 irq_ack=0 nand=138412032B nand_r=425664 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=202 dma_bytes=425662 last_dma=b0000000>0c1d767e/2112/00004421 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 unmapped_r=8 last_r=cc1d7def unmapped_w=4 last_w=43c142ef:b8
```

## Interpretation

Compared with v7, v8 moves from 65 DMA transfers / 135,933 bytes to 202 DMA transfers / 425,662 bytes.  The new blocker is no longer the FPGA-ready wait or allocator scan; it is execution out of RAM populated by NAND/DMAC transfers.

The likely missing pieces are accurate SH7709S DMAC channel semantics, NAND page/OOB/bad-block reconstruction, P0 MMU/TLB/cache behavior, and real interrupt/timer behavior.  The v8 compatibility assists are deliberately narrow and counted in the `assists=` status field.
