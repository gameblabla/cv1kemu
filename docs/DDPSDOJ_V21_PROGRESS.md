# DDPSDOJ v22 progress

This revision still does not reach a real title screen. It adds targeted memory/NAND/CPU/IRQ diagnostics and fixes a NAND random-output address-cycle bug.

Conservative 10,000-frame run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829541/63 dcache=0/0 stale=0 cachectl=0 widep0=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Experimental 8,000-frame run:

```text
model=CV1000-D pc=0c1d7cfc sr=40000101 frames=8000 cycles=175642745 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=116064000 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=14 dma=54955 dma_bytes=116063998 last_dma=b0000000>1301f6be/2112/00004421/m01/00004422 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=548 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=311107447/62 dcache=71352699/10068 stale=815296 cachectl=0 widep0=1 dmasync=0 dmainv=0 ndata=0 nandcmd=30 pg=34508 col=0 rnd=0 spr=3517056 alias=139365186/124/530367881/0/0/0 tlb=0/0/531902183:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

The new `nandcmd`, `pg`, `col`, `rnd`, and `spr` fields expose the NAND command cursor and spare/OOB activity. v22 shows large spare/OOB traffic before the current blockers, so the next work should stay on NAND/OOB reconstruction and SH7709S cache/DMAC coherency.
