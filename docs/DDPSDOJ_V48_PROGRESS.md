# DDPSDOJ v48 progress

v48 corrects a concrete SH7709S CPU mismatch against MAME.

MAME's `sh34_base_device::SHAD` and `SHLD` handle negative shift counts that are exact multiples of 32 specially. v47 used a simple masked shift count, so `-32`, `-64`, etc. behaved like shift-by-zero. v48 now matches MAME:

- `SHAD`: negative multiple-of-32 produces all sign bits.
- `SHLD`: negative multiple-of-32 produces zero.

This does not change the 10,000-frame DDPSDOJ endpoint, but it removes a known CPU-core divergence in the copied-RAM helper area where SH variable shifts are actively executed.

## Default verification

```text
model=CV1000-D pc=0c30b6d2 sr=40000100 frames=10000 cycles=217994686 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=7115757 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3245 dma_bytes=7112510 dmat=0000 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c tmu=3979/3997/0:00000420/1 assists=743 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=253929892/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=24461993/124/0/0/0/0 tlb=0/0/469911707:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

## MAME-speedup verification

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=122545295 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 dmat=0000 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c tmu=671338/268/0:00000400/6 assists=95 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147736036/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4469 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=22217460/308/0/0/0/0 tlb=0/0/151785902:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Still no real title screen. The default path remains in the later copied-RAM helper family and the blitter still reports `draw=0`.
