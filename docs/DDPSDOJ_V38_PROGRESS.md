# DDPSDOJ v38 verification

v38 continues the MAME-matching pass for SH7709S DMAC behavior. It does not reach the DDPSDOJ title screen.

Changes:

- DMAC start checks are now closer to MAME's `sh4_dmac_check`: a transfer is checked only after CHCR writes or DMAOR writes, DMAOR.DME must be set, CHCR.TE and DMAOR.AE/NMIF block transfer, and CHCR.RS must be in MAME's accepted range.
- DMAOR at `0x04000060-0x04000061` now reads back as the stored controller register, not a forced zero ready value.
- Added `--mame-full-dmatcr`, an opt-in diagnostic that uses MAME's full `0x1000000` transfer count for zero DMATCR with a fast NAND-data-port DMA path. The conservative default keeps the v37 finite bound because the standalone core still lacks MAME's asynchronous DMA timer/scheduler model.
- For RS > 3 memory-to-memory DMA, SAR/DAR are no longer written back after the immediate transfer path, matching MAME's local-copy behavior in `sh4_dmac_check`; DMATCR completion and CHCR.TE are still reflected.
- Save states are now `CV1KSS38`.

Default DDPSDOJ endpoint:

```text
model=CV1000-D pc=0c30b6ca sr=40000100 frames=10000 cycles=216392318 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=7115757 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3245 dma_bytes=7112510 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=252031228/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=20940434/124/0/0/0/0 tlb=0/0/484017754:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

MAME-speedup endpoint:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 fulldma=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=20873981/308/0/0/0/0 tlb=0/0/152174817:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Full-DMATCR diagnostic endpoint:

```text
model=CV1000-D pc=0a2126bc sr=40000101 frames=10000 cycles=116537139 illegal=4726 last_illegal=0a2126ba:ffff irq_ack=0 nand=138412032B nand_r=1075896454 nand_w=0 blit_ops=0 up=0 draw=0 clip=0 unk=0/0000 bns=0 hpen=0 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=65 dma_bytes=1075896387 last_dma=b0000000>0c191000/1075761219/00004421/m01/00004422 np=29440-29440 nb=460-460 nc=1347-582 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=141649347/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=1 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=29440 col=582 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=20781792/100/0/0/0/0 tlb=0/0/144456162:00000000>00000000 unmapped_r=9452 last_r=0a2126bb unmapped_w=0 last_w=00000000:00
```

IRQ/vblank diagnostic endpoint:

```text
model=CV1000-D pc=0c1d1d28 sr=700000f0 frames=3000 cycles=70338875 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 clip=0 unk=0/0000 bns=0 hpen=0 of=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=3000/4/00000000 irqcpu=2/4/0430 ex=0/00000000/00000000 assists=0 fpga_bits=189475 fpga_done=0 fpga_sum=50 fpga_fw=-1 icache=78379928/8 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/0 fulldma=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=14364821/38/0/0/0/0 tlb=0/0/79673167:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Result: default v38 remains equivalent to v37's later allocator/list loop endpoint rather than a title screen. The exact full-DMATCR diagnostic regresses into an illegal high/blank fetch in this standalone scaffold, which is evidence that the remaining missing piece is MAME's asynchronous DMA/timer/scheduler and coherency interaction rather than the raw transfer count alone.
