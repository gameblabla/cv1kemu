# DDPSDOJ v13 progress

v13 does not reach a real title screen.

The main code change is a NAND command-state fix: DDPSDOJ issues command `0x50` once after a long `0x00/0x30` page-read scan. Earlier sandbox versions did not reset the NAND address-byte counter for `0x50`, so the next address write could be misinterpreted as the fifth address byte of the previous read. v13 resets the address phase for `0x50` and models it as a spare-area read command.

The default boot checkpoint remains:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421 assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829507/97 dcache=0/0 stale=0 cachectl=0 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

The new `--strict-cache-ops` experiment wires SH cache-control instructions and P4 cache-control writes to the simplified cache model. It regresses the boot path, which is useful diagnostic evidence: the sandbox's current fetch-cache behavior is a compatibility approximation, not an accurate SH7709S cache implementation.
