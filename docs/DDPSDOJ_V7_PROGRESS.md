# DDPSDOJ v7 progress

v7 still does not reach a real title screen.  It advances beyond the v6 allocator/index failure path and reaches later boot code at `0c1d7cfc` / `0c30c7xx` during the 20,000-frame probe.

Observed 20,000-frame status:

```text
model=CV1000-D pc=0c1d7cfc sr=40000100 frames=20000 cycles=448442050 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=135935 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=65 dma_bytes=135933 last_dma=b0000000>0c191000/765/00004421 assists=719974 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Material changes from v6:

- Added SH7709S banked R0-R7 support for SR.RB transitions and saved it in the state format (`CV1KSS07`).
- Added last-DMA diagnostics (`SAR`, `DAR`, `TCR`, `CHCR`) to the status line and save state.
- Added a narrow DDPSDOJ allocator-scan assist for the specific P0 work-RAM pointer form seen at `0c30b6ae`.  This is not a hardware-accurate fix; it is a documented bridge around the incomplete SH7709S MMU/TLB/cache model.
- Preserved zero illegal opcodes and zero unmapped bus accesses for the 20,000-frame probe.

Current blocker:

The 20,000-frame trace is no longer stuck at the v6 `0c30b6b0` loop. It reaches a short byte-copy/countdown helper at `0c1d7cfc`, returns into `0c30c720`, and begins setting up another NAND/DMAC descriptor. Further progress requires more exact DMAC channel semantics, SH internal register side effects, P0/TLB/cache behavior, and NAND OOB/bad-block handling.
