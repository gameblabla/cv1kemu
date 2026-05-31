# DDPSDOJ v25 progress

MAME source was decompressed from the uploaded `mame-master.tar.gz`; the v25 patch ports the MAME RTC9701 serial state machine and YMZ770 register-select/data behavior into ANSI C, with attribution in `NOTICE` and `docs/MAME_DERIVED.md`.

Conservative 10,000-frame run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9805 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Opt-in cache metadata 1,000-frame run:

```text
model=CV1000-D pc=0c002b16 sr=700000f0 frames=1000 cycles=23914569 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=0/0/00000000 ex=0/00000000/00000000 assists=0 fpga_bits=1 fpga_done=0 fpga_sum=00 icache=25424124/1 dcache=0/0 stale=0 mcache=1/26676711/30/0/25838997/833744/4000 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=5926065/26/0/0/0/0 tlb=0/0/26676741:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Save-state load smoke test excerpt:

```text
T 000000 pc=0c002b16 op=6794 r0=005a33c8 r1=000fe975 r2=0000000c r3=00000001 r4=ac2b00b0 r5=a0051000 r6=00000080 r7=00000062 r8=0000000d r9=ac2b004e ra=0000001f rb=00000080 rc=a012636b rd=a00622f1 re=00000000 rf=0d000000 sr=700000f0
T 000001 pc=0c002b18 op=2470 r0=005a33c8 r1=000fe975 r2=0000000c r3=00000001 r4=ac2b00b0 r5=a0051000 r6=00000080 r7=00000051 r8=0000000d r9=ac2b004f ra=0000001f rb=00000080 rc=a012636b rd=a00622f1 re=00000000 rf=0d000000 sr=700000f0
T 000002 pc=0c002b1a op=7401 r0=005a33c8 r1=000fe975 r2=0000000c r3=00000001 r4=ac2b00b0 r5=a0051000 r6=00000080 r7=00000051 r8=0000000d r9=ac2b004f ra=0000001f rb=00000080 rc=a012636b rd=a00622f1 re=00000000 rf=0d000000 sr=700000f0
model=CV1000-D pc=0c002b1c sr=700000f0 frames=1000 cycles=23914572 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=0 ymz_reg=0 ymz_key=0/0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=0/0/00000000 ex=0/00000000/00000000 assists=0 fpga_bits=1 fpga_done=0 fpga_sum=00 icache=25424127/1 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=5926067/26/0/0/0/0 tlb=0/0/26676750:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
set=ddpsdoj source=/mnt/data/ddpsdoj.zip zip=1 ok=1
u2 present=1 crc=668e4cd6 expected=668e4cd6 size=138412032 load=138412032 ignore=0
u4 present=1 crc=e2a4411c expected=e2a4411c size=4194560 load=4194304 ignore=256
u23 present=1 crc=ac94801c expected=ac94801c size=4194560 load=4194304 ignore=256
u24 present=1 crc=f593045b expected=f593045b size=4194560 load=4194304 ignore=256
ddpsdoj romset loaded and CRC matched MAME manifest
```

Result: still no real title screen. The verified endpoint remains the copied-RAM loop around `0c1fb3dc-0c1fb42c`; the package is a fidelity improvement rather than a completed emulator.
