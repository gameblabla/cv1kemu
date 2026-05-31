# DDPSDOJ v50 progress note

v50 still does **not** reach the real DDPSDOJ title screen.  The visible output remains noisy/non-title framebuffer content and the main diagnostic is still `draw=0`, meaning the game has not reached normal draw-list execution.

## Patch summary

v50 continues the copied-RAM helper work from v47-v49.  The blocked default path executes the exact helper at `0c30b6ae-0c30b6f6`, which reads a stride from the allocator/table structure associated with the literal at `0c7f83c0`.  On the sandbox path the table backing can be zero or erased-looking while the instruction/literal stream remains visible through the CPU fetch path.  That is a symptom of the still-incomplete SH7709S cache/MMU/DMAC coherency model.

The v50 helper guard now treats both zero and erased `0xffffffff` stride reads as incoherent table reads for this exact instruction sequence.  It falls back to the active stack stride if it is sane, otherwise to the observed CV1000 block stride `0x840`.  This is deliberately narrow: it only runs after verifying the copied-RAM opcode sequence via the fetch-side path.

The risky post-helper completion bridge at `0c30b71a` remains gated behind `--aggressive-assists` when the result range is outside the prior conservative guard.  The conservative/default path therefore stays non-illegal.

Save-state magic is now `CV1KSS50`.

## Verification

Default 10,000-frame endpoint:

```text
model=CV1000-D pc=0c30b71a sr=40000101 frames=10000 cycles=459083942 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=15483241 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=346938400 hpen=11394000 of=1 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=7207 dma_bytes=15476032 dmat=0000 last_dma=b0000000>007f7d00/2112/00004421/m01/00004422 np=11340-11340 nb=177-177 nc=256-256 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c tmu=13397/13414/0:00000420/1 assists=7151 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=310053958/62 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=11340 col=256 rnd=0 spr=4096 nmap=1024/454/568/36309 ce=1 alias=27317239/125/0/1971/0/0 tlb=0/0/329714284:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

MAME-TRAPA plus MAME-speedup 10,000-frame endpoint:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=122545295 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 dmat=0000 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c tmu=671338/268/0:00000400/6 assists=95 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147737796/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4469 fulldma=0 mtmu=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=22217460/308/0/0/0/0 tlb=0/0/151784142:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Compared with v49 default, v50 advances the conservative path from about 7.1 MB of NAND DMA traffic to about 15.5 MB before it reaches the next coherency/result loop at `0c30b71a`.  That is progress through the noisy-framebuffer boot path, but it is not a title-screen breakthrough.
