# DDPSDOJ v46 progress

v46 is a MAME-derived DMAC completion-timing patch.  It does not reach the title screen.

Default run:

```text
model=CV1000-D pc=0c30b6ca sr=40000100 frames=10000 cycles=217404673 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=7115757 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3245 dma_bytes=7112510 dmat=0000 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c tmu=3956/3974/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=253928669/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=24461993/124/0/0/0/0 tlb=0/0/460890896:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

MAME-speedup diagnostic run:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=122546043 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 dmat=0000 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c tmu=671338/671357/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147786874/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4466 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=22217460/308/0/0/0/0 tlb=0/0/151937668:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Compared with v45, the endpoint remains effectively the same: the visible garbage is still the only meaningful framebuffer signal.  The new code removes an exact mismatch where RS 4-6 memory DMA exposed transfer completion immediately instead of after the MAME timer delay.
