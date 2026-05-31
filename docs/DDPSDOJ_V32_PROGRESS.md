# DDPSDOJ v32 verification

v32 continues matching the standalone ANSI C CV1000 sandbox against the uploaded MAME source tree.

## MAME-derived correction

The v32 patch aligns the CV1000 blitter timing/accounting path more closely with MAME's `cv1k_blitter_device` implementation in `src/mame/cave/cv1k_v.cpp` / `cv1k_v.h`:

- Corrected the clip idle-operation accounting constant to MAME's `CV1K_CLIP_OPERATION_SIZE_BYTES = 2`. The command stream still consumes the `0xc000` command word plus its parameter word; the corrected value is the amount passed to MAME's `idle_blitter()` timing model.
- Added MAME-style 64-byte idle-operation chunking with `OPERATION_READ_CHUNK_INTERVAL_NS = 700`.
- Added MAME-style `calculate_vram_accesses()` accounting for source/destination 32x32 VRAM row crossings in draw commands.
- Added MAME's horizontal-line contention adjustment: every 63,600 ns of blitter work adds a 2,160 ns line-fetch penalty.
- Added status fields `bns=`, `hpen=`, and `of=` for blitter nanoseconds, horizontal-line penalty, and over-frame delay count.
- Save states are bumped to `CV1KSS32` and preserve the new blitter timing counters.

This is a fidelity/accounting patch. It does not patch around the boot wait or force a title screen.

## Conservative 10,000-frame result

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=80 hpen=0 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

## MAME-speedup 10,000-frame diagnostic

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=21181444/308/0/0/0/0 tlb=0/0/152482280:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Result: still no DDPSDOJ title screen. The MAME-speedup path remains at the RAM/vblank tick wait near `0x0c1d1346`; the conservative path remains around `0x0c1fb3e0`. The remaining blockers are still the incomplete SH7709S MMU/TLB/cache/exception/timer model, interrupt scheduling, DMAC/cache coherency, and deeper FPGA/blitter/audio fidelity.
