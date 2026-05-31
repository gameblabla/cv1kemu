# DDPSDOJ v9 progress

v9 still does not reach the real game title screen.  It does advance materially beyond v8 by adding a small SH instruction-fetch cache model.  The earlier v8 failure occurred after NAND/DMAC copied code over a work-RAM region that was still being executed; without any SH cache model the sandbox fetched the overwritten bytes and fell into invalid/random code.

## Hardware-model change

`cv1k_bus_fetch16()` is now separate from generic data reads.  Instruction fetches from work RAM populate an 8192-line direct-mapped cache and keep returning cached opcodes even if a later NAND/DMAC copy overwrites the same RAM.  This is not a cycle-accurate SH7709S cache, but it models the important boot-time distinction between instruction fetch and DMA/data writes well enough to clear the v8 copied-code illegal-opcode path.

Save states were bumped to `CV1KSS09` and now preserve the instruction-cache tags, opcodes, valid bits, and hit/miss counters.  The debug paths now use `cv1k_machine_step()`, so `--run-break-pc`, `--break-pc`, and `--trace-steps` go through the same compatibility-assist path as frame execution.

## Verified command

```sh
make
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --probe-title --run-frames 10000 --dump-ppm docs/DDPSDOJ_V9_PROGRESS.ppm
```

## Result

```text
model=CV1000-D pc=0c30b6cc sr=40000100 frames=10000 cycles=216322757 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=1686528 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=799 dma_bytes=1686526 last_dma=b0000000>0c30b3be/2112/00004421 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=215904219/3641 unmapped_r=20806376 last_r=0ff38ffa unmapped_w=0 last_w=00000000:00
```

## Movement from v8

v8 reached 202 DMA transfers and 425,662 bytes before falling through overwritten/copied code and accumulating illegal opcodes.  v9 reaches 799 DMA transfers and 1,686,526 bytes with zero illegal opcodes and zero unmapped writes in the 10,000-frame run.

## Current blocker

The current run is back in the allocator / block-walk code around `0c30b6ae-0c30b6f4`.  With the instruction cache enabled, the loop no longer executes random copied bytes, but it performs many reads from an unmapped computed address ending at `0ff38ffa`.  That points at a remaining MMU/TLB/cache aliasing problem or a still-inaccurate structure produced by earlier NAND/DMAC/peripheral setup.

The next productive work is not another broad opcode patch.  It is to replace the coarse `0x40000000 -> work RAM` shortcut with a real enough SH7709S UTLB/ITLB model, add cache-control register side effects, and improve DMAC channel modes instead of using a fixed-source byte-copy approximation.
