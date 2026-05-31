# DDPSDOJ v12 progress

v12 still does not reach the real title screen.

Default run:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --probe-title --run-frames 10000 --dump-ppm docs/DDPSDOJ_V12_PROGRESS.ppm
```

Observed default status:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 dcache=0/0 stale=0 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

New v12 experiment:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --dcache --run-frames 10000
```

Observed data-cache status:

```text
model=CV1000-D pc=0c30b71a sr=40000101 frames=10000 cycles=286912055 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=43183104 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=20447 dma_bytes=43183102 last_dma=b0000000>0ea9e3be/2112/00004421 assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=314426267/96 dcache=31692791/162150 stale=28864 unmapped_r=0 last_r=00000000 unmapped_w=27913214 last_w=0ea9ebfd:ff
```

Interpretation: the copied-RAM blocker is sensitive to data-cache coherency. Preserving stale CPU data-cache lines allows the boot stream to reach much more NAND/DMAC traffic, but the current broad non-coherent model then diverges and produces bad write targets. This is useful evidence for the next implementation pass, not a working title-screen path.
