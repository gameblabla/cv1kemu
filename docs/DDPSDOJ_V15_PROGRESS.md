# DDPSDOJ v15 progress note

v15 is a coherency/diagnostics pass over the v14 blocker, not a title-screen milestone.

Default run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 dcache=0/0 stale=0 cachectl=0 widep0=0 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

The default path still reaches the copied-RAM status loop at `0c1fb3dc-0c1fb42c`.  The loop executes from the fetch-side cache while the backing RAM at the same addresses has already been overwritten by NAND/DMAC payload.  The loaded status word comes through `R10=0x4b72c4cc`; the current coarse alias resolves that to `0x0c72c4d4`, where the word is payload-looking data (`c298a0bf`) rather than a compact status value.

v15 adds two debugging/accuracy experiments:

- `--wide-p0-alias` maps broader SH P0 virtual aliases onto the CV1000-D work-RAM window.  It is off by default because the exact SH7709S TLB/cache behavior is still not implemented.
- `--dump-ram`, `--dump-ram-addr`, and `--dump-ram-size` dump a work-RAM slice after a run.  This was added to preserve the high-alias/status-loop evidence without parsing a full save state.

With `--dcache --wide-p0-alias`, the run reaches much more NAND/DMAC traffic and no longer reports unmapped writes:

```text
model=CV1000-D pc=0c30b71a sr=40000101 frames=10000 cycles=286912055 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=43183104 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=20447 dma_bytes=43183102 last_dma=b0000000>0ea9e3be/2112/00004421 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=314426267/96 dcache=31692791/162150 stale=290400 cachectl=0 widep0=1 tlb=0/0/398534496:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

That experimental path advances from 3,244 DMA transfers / 6,850,366 bytes to 20,447 DMA transfers / 43,183,102 bytes.  It then reaches a tight self-loop at `0c30b71a`, which looks like a later software stop/failure path rather than a missing opcode.  The result is useful because it narrows the active issue to SH7709S cache/TLB/DMAC coherency and NAND/OOB reconstruction, but it is still not a working title screen.

The new `widep0=` field in the status line records whether the broader alias experiment was enabled.
