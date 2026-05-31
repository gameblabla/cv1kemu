# DDPSDOJ v37 verification

v37 compares the standalone SH7709S DMAC path against MAME's `src/devices/cpu/sh/sh4dmac.cpp` SH-3 code.

Changes:

- Aligns source and destination addresses before 16-bit, 32-bit, and 16-byte DMA transfers, matching MAME's SH-3/SH-4 DMAC transfer helper.
- Treats zero `DMATCR` as a real DMA request rather than an error. MAME expands this to `0x1000000` transfers; the standalone harness executes a bounded `0x40000` transfer to keep the headless verification tractable while taking the MAME completion path.
- Clears `DMATCR` on completion and sets transfer-end in `CHCR`, matching MAME's completion callback semantics more closely than prior versions.

Default DDPSDOJ endpoint:

```text
model=CV1000-D pc=0c30b6ca sr=40000100 frames=10000 cycles=216392318 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=7115757 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3245 dma_bytes=7112510 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=252031228/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=20937189/124/0/0/0/0 tlb=0/0/498239529:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

MAME-TRAPA plus MAME-speedup endpoint:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=20873829/308/0/0/0/0 tlb=0/0/152789895:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

IRQ/vblank diagnostic endpoint:

```text
model=CV1000-D pc=0c1d1d28 sr=700000f0 frames=3000 cycles=70338875 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 clip=0 unk=0/0000 bns=0 hpen=0 of=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=3000/4/00000000 irqcpu=2/4/0430 ex=0/00000000/00000000 assists=0 fpga_bits=189475 fpga_done=0 fpga_sum=50 fpga_fw=-1 icache=78379928/8 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=14364821/38/0/0/0/0 tlb=0/0/79673167:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Result: the default path no longer stops at the previous `0c1fb3e0` loop; it reaches `0c30b6ca` and produces a noisy, non-title framebuffer. It is still not a correct title-screen boot. The remaining blockers are the same large fidelity gaps: full SH7709S MMU/TLB/cache/timer/interrupt scheduling, exact DMAC/device timing/coherency, and complete CV1000 FPGA/blitter/audio behavior.
