# DDPSDOJ v36 MAME-matching progress

v36 continues matching the standalone ANSI C scaffold against the uploaded MAME source tree. The concrete patch is in SH7709S interrupt plumbing, not a boot hack.

MAME separates the accepted interrupt event code from interrupt priority. The event code identifies the line, for example IRQ2 is `0x640`, while the priority comes from SH7709S INTC priority registers and is compared with `SR.IMASK`. Earlier sandbox builds stored one value as both line and priority. v36 adds an IRQ line field to the CPU state, keeps IRQ2 as line 2, and reads the IRQ2 priority from IPRC bits 8..11 at internal offset `0x04000016`.

MAME's `intc_7709_map` exposes `INTEVT2` at physical `0x04000000`. Earlier sandbox builds stored the accepted event at offset `0xfe00`, a diagnostic P4-style location that MAME does not use for this register. v36 now writes the event into offset zero of the SH I/O shadow so reads through the MAME-mapped register see the accepted line event.

Verification, default mode:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 bns=80 hpen=0 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 mspeed=0/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=20937233/124/0/0/60807112/1 tlb=0/0/320361194:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Verification, MAME-TRAPA plus MAME-speedup mode:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 bns=677040 hpen=151200 of=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 irqcpu=0/0/0430 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=147073632/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/4494 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=20873829/308/0/0/0/0 tlb=0/0/152789895:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

IRQ/vblank diagnostic shows that IRQ2 requests now carry line 2 and priority 4 from IPRC (`irqcpu=2/4/0430`), although the game is still at an early supervisor/masked state in that diagnostic path and does not accept the interrupt.

```text
model=CV1000-D pc=0c1d1d28 sr=700000f0 frames=3000 cycles=70338875 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 clip=0 unk=0/0000 bns=0 hpen=0 of=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=3000/4/00000000 irqcpu=2/4/0430 ex=0/00000000/00000000 assists=0 fpga_bits=189475 fpga_done=0 fpga_sum=50 fpga_fw=-1 icache=78379928/8 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 mspeed=1/0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=14364821/38/0/0/0/0 tlb=0/0/79673167:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Result: still not title-screen capable. The remaining blockers remain full SH7709S MMU/TLB/cache/timer/scheduler fidelity, DMAC/cache coherency, and remaining CV1000 FPGA/blitter/audio behavior.
