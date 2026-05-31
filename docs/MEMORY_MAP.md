# CV1000 memory map implemented by this sandbox

| Range | Implemented behavior |
| --- | --- |
| `0x00000000-0x003fffff` | U4 boot/program flash, read-only, big-endian fetches. |
| `0x0c000000-0x0c7fffff` | CV1000-B work RAM, 8 MiB. |
| `0x0c000000-0x0cffffff` | CV1000-D work RAM, 16 MiB. |
| `0x10000000-0x10000007` | U2 NAND data/command/address latch shim. Offset 0=data, 1=command, 2=address. |
| `0x10400000-0x10400007` | YMZ770 write latch / command FIFO placeholder. |
| `0x10c00000-0x10c00007` | RTC9701 EEPROM/RTC serial-bit shim. Offset 1 returns `0xfe | read_bit`. |
| `0x18000000-0x18000057` | CV1000 blitter register window. Writing the low byte of register 0 executes a RAM operation list at that register value. |
| `0xf0000000-0xf0ffffff` | SH-3 cache/internal RAM placeholder. |

Port reads mirror MAME's PORT_C, PORT_D, PORT_E, PORT_F, and PORT_L concepts. PORT_E returns a ready bit based on NAND busy state.
