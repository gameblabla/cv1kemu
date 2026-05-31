# DDPSDOJ v45 MAME-matching progress

v45 corrects another SH3/SH7709S interrupt-cause register mismatch against MAME.  Previous sandbox builds wrote the SH7709S `INTEVT2` source/event code at the physical INTC window, but left CCN `INTEVT` at `0xffffffd8` stale.  MAME updates both: `INTEVT2` receives the source event code, and `INTEVT` receives either the same internal event code or, for line-style IRQ events `>= 0x600`, the priority-coded value `0x3e0 - priority * 0x20`.

This matters for the optional timer/IRQ paths and for any boot code that reads the CCN interrupt-cause register after an accepted interrupt.  It does not change the conservative no-IRQ endpoint.

## Default 10,000-frame run

```text
model=CV1000-D pc=0c30b6ca sr=40000100 frames=10000 cycles=216392318 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=7115757 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3245 dma_bytes=7112510 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c tmu=3927/3934/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=252031228/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=22185473/124/0/0/0/0 tlb=0/0/482772715:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

## MAME-TRAPA plus MAME-speedup run

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c tmu=236/1342422/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=22119020/308/0/0/0/0 tlb=0/0/150929778:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

## TMU IRQ diagnostic run

```text
model=CV1000-D pc=0000008e sr=700000f0 frames=10000 cycles=116922668 illegal=1 last_illegal=fffffffe:0000 irq_ack=51 nand=138412032B nand_r=827469 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=269 dma_bytes=827198 last_dma=b0000000>0c1f96fe/2112/00004421/m01/00004422 np=3564-3564 nb=55-55 nc=256-256 irq_req=0/6/00000400 irqcpu=0/1/0430 ex=55/00000400/00000000 tmu=41/671137/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=142073975/69 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=1 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=30 pg=3565 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=0 alias=22058596/357/0/0/0/0 tlb=0/0/144357025:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Result: v45 still does not reach the real DDPSDOJ title screen.  The remaining blocker is the larger SH7709S platform model: full scheduler/interrupt timing, MMU/TLB/cache behavior, DMAC/cache coherency, plus remaining CV1000 FPGA/blitter/audio fidelity.
