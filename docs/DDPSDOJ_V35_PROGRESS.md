# DDPSDOJ v35 MAME-matching progress

v35 corrects one concrete SH7709S CPU-core mismatch against MAME: RTE now executes its delay-slot instruction before restoring `SR` from `SSR` and branching to `SPC`.  Earlier sandbox builds restored `SR` before the delay slot, which is not how MAME's SH3/SH7709S generated path orders RTE.

This patch mostly affects optional IRQ/vblank experiments.  The conservative default boot path is unchanged and still stops at the copied-RAM wait loop.

## Default 10,000-frame run

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=80 hpen=0 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=20937233/124/0/0/60807112/1 tlb=0/0/320361194:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

## MAME-speedup 10,000-frame run

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=20873829/308/0/0/0/0 tlb=0/0/152789895:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

## MAME-speedup + IRQ/vblank diagnostic, 3,000-frame run

```text
model=CV1000-D pc=0c1d1d28 sr=700000f0 frames=3000 cycles=70338875 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 clip=0 unk=0/0000 bns=0 hpen=0 of=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=3000/2/00000000 ex=0/00000000/00000000 assists=0 fpga_bits=189475 fpga_done=0 fpga_sum=50 fpga_fw=-1 icache=78379928/8 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=14364821/38/0/0/0/0 tlb=0/0/79673167:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

The game still does not reach the title screen.  Remaining high-risk gaps are the full MAME SH7709S MMU/TLB/cache/timer/interrupt model, exact DMAC/cache coherency, and the rest of the CV1000 FPGA/blitter/audio behavior.
