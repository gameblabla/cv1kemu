# DDPSDOJ v18 progress

v18 does not reach the real title screen. It is a diagnostic pass over the v17 long experimental path.

Default conservative command:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --probe-title --run-frames 10000 --dump-ppm docs/DDPSDOJ_V18_PROGRESS.ppm
```

Default result:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829541/63 dcache=0/0 stale=0 cachectl=0 widep0=0 ndata=0 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

Experimental command:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --run-frames 10000
```

Experimental result:

```text
model=CV1000-D pc=0c1d928e sr=40000100 frames=10000 cycles=184948442 illegal=231 last_illegal=0c1d92aa:f6f3 irq_ack=0 nand=138412032B nand_r=134353922 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=14 dma=63616 dma_bytes=134353920 last_dma=b0000000>141913fe/2/00004421 assists=684 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=330012886/81 dcache=82493809/10075 stale=1048576 cachectl=0 widep0=1 ndata=0 tlb=0/0/585104569:00000000>00000000 unmapped_r=68 last_r=f337f138 unmapped_w=11 last_w=da2bde58:ff
```

The v18 change reduces the v17 payload-derived high-invalid-target failure (`df42d3c0`) to a later copied-RAM failure around `0c1d92aa`, with fewer illegal opcodes and far fewer unmapped reads. This is still not correct hardware behavior.
