# DDPSDOJ v39 verification

v39 continues the MAME-matching pass with an ANSI C adaptation of SH7709S TMU register/timer behavior from MAME's `src/devices/cpu/sh/sh4tmu.cpp`, `sh4.cpp`, and `sh4comn.cpp`. It still does not reach the DDPSDOJ title screen.

Changes:

- Added SH7709S TMU register behavior for TSTR, TCOR, TCNT, and TCR channel state.
- Added MAME's SH TMU divisor table `(4, 16, 64, 256, 1024, 1, 1, 1)` and per-channel underflow accounting.
- Added TCR.UNF flag setting and TCNT reload from TCOR on underflow.
- Added MAME event codes for TUNI0/TUNI1/TUNI2: `0x400`, `0x420`, and `0x440`.
- Added an opt-in `--mame-tmu-irq` diagnostic that raises TMU underflow interrupt events when TCR.TIE and the decoded IPRA priority are nonzero. It is not enabled by default because, without MAME's full scheduler/interrupt/timer context, it regresses into an illegal fetch.
- CPU interrupt plumbing now accepts literal internal event codes as well as external IRQ line events.
- Save states are now `CV1KSS39` and include the TMU diagnostic counters and flag.

Default DDPSDOJ endpoint:

```text
model=CV1000-D pc=0c30b6ca sr=40000100 frames=10000 cycles=216392318 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=7115757 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3245 dma_bytes=7112510 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c tmu=3927/3934/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=252031228/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=20940434/124/0/0/0/0 tlb=0/0/484017754:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

MAME-speedup endpoint:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c tmu=236/1342422/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=20873981/308/0/0/0/0 tlb=0/0/152174817:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

MAME TMU IRQ diagnostic endpoint:

```text
model=CV1000-D pc=0000008e sr=700000f0 frames=10000 cycles=116922668 illegal=1 last_illegal=fffffffe:0000 irq_ack=51 nand=138412032B nand_r=827469 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=269 dma_bytes=827198 last_dma=b0000000>0c1f96fe/2112/00004421/m01/00004422 np=3564-3564 nb=55-55 nc=256-256 irq_req=0/6/00000400 irqcpu=0/1/0430 ex=55/00000400/00000000 tmu=41/671137/0:00000420/1 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=142073975/69 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=1 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=30 pg=3565 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=0 alias=20813557/357/0/0/0/0 tlb=0/0/145602064:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Short MAME-speedup plus TMU IRQ diagnostic endpoint:

```text
model=CV1000-D pc=0c1d1d28 sr=700000f0 frames=3000 cycles=70338875 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 clip=0 unk=0/0000 bns=0 hpen=0 of=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=0/00000000/00000000 tmu=0/0/0:00000000/0 assists=0 fpga_bits=189475 fpga_done=0 fpga_sum=50 fpga_fw=-1 icache=78379928/8 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/0 fulldma=0 mtmu=1 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=14364821/38/0/0/0/0 tlb=0/0/79673167:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Result: v39 records real TMU underflow activity in the default and MAME-speedup runs, but enabling TMU IRQ delivery exposes the still-missing SH7709S interrupt/scheduler/vector fidelity and causes an illegal low-vector fetch rather than a title screen.
