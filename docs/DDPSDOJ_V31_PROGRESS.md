# DDPSDOJ v31 verification

v31 continues matching the standalone ANSI C sandbox against the uploaded MAME source.

## MAME-derived corrections

1. Added an opt-in `--mame-speedup` path for MAME's DDPSDOJ-class speedup install. MAME's `init_ddpdfk()` installs `install_speedups(0x02310, 0x0c1d1346, true)`, and the read handler calls `spin_until_interrupt()` when the SH7709S PC is the idle-loop PC. The sandbox has no MAME scheduler, so `--mame-speedup` detects the same PC and breaks to the frame/vblank tick instead of burning the full frame budget in the idle loop. The option also enables `--mame-trapa`, because this path is only reached with MAME-style TRAPA vectoring.

2. Matched another CV1000 blitter detail from `cv1k_v.cpp`: each blit execution pass now resets the active clip rectangle from the 0x40/0x44 clip registers with the 32-pixel CV1000 clip margin before parsing the command list. Unknown blitter opcodes now terminate the list like MAME's `popmessage(...); return` path rather than advancing by one word and continuing.

3. Save states are bumped to `CV1KSS31` and persist the MAME-speedup enable flag and spin counter.

## Conservative 10,000-frame result

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

This remains the conservative behavior and still does not reach title.

## MAME-speedup 10,000-frame diagnostic

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=21181444/308/0/0/0/0 tlb=0/0/152482280:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

The MAME-speedup path reduces wasted idle-loop execution substantially and records scheduler spin events, but it still does not reach the real DDPSDOJ title screen. The endpoint remains the vblank/RAM-tick wait around `0x0c1d1346` with seven upload blits observed and no draw commands yet.

Remaining blockers: the standalone SH7709S/MMU/TLB/cache/exception/timer model is still not MAME-equivalent, the IRQ scheduler is still skeletal, and the full FPGA/blitter/audio behavior remains incomplete.
