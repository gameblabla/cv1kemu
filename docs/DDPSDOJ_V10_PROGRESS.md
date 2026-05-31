# DDPSDOJ v10 progress

v10 still does not reach the real title screen. It advances materially beyond v9 by improving the cache/MMU model around RAM code that is later overlaid by NAND/DMAC transfers.

## Added in v10

- Instruction-cache misses now prefill a 0x200-byte work-RAM block. This gives copied code and nearby literal pools a shared fetch-side lifetime instead of preserving only already-executed halfwords.
- PC-relative literal loads use the fetch-side cache path. That prevents literal pools beside cached RAM code from immediately changing after a later DMA overlay.
- The coarse SH7709S virtual-to-work-RAM mapping now covers the observed low P0 virtual window after the boot ROM area, `0x40000000-0x4fffffff`, and the `0xe0000000-0xe0ffffff` high alias seen after the new cache path.
- A narrow high-alias blank-code guard returns through `PR` when execution reaches `e00xxxxx` blank mapped RAM. This is counted in `assists=` and remains a compatibility bridge, not hardware-accurate exception handling.

## Verified 10,000-frame checkpoint

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

## Movement from v9

v9 reached 799 DMA transfers and 1,686,526 copied bytes at 10,000 frames. v10 reaches 3,244 DMA transfers and 6,850,366 copied bytes with zero illegal opcodes. The remaining unmapped read count drops from the v9 20M+ level to 17 at the checkpoint due to the broader virtual work-RAM aliases.

## Current blocker

The run is now in copied RAM code around `0c1fb3dc-0c1fb42c`. That loop reads a table/status word through a high virtual work-RAM pointer and waits while the word is greater than five. The word currently looks like copied payload data, not a small status value. The likely root causes remain incomplete NAND bad-block/OOB reconstruction, incomplete SH7709S TLB/cache-control behavior, exact DMAC transfer modes, and missing peripheral/timer side effects.
