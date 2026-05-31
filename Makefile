CC ?= cc
CFLAGS ?= -std=c89 -pedantic -Wall -Wextra -O2
LDFLAGS ?=
ZLIB_CFLAGS ?= $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS ?= $(shell pkg-config --libs zlib 2>/dev/null || echo -lz)
SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null)

BASE_SRC = \
 src/main.c \
 src/platform.c \
 src/mame_cv1k_derived.c \
 src/cpu_sh7709s.c \
 src/bus.c \
 src/emu.c \
 src/nand.c \
 src/rtc9701.c \
 src/sound_ymz770.c \
 src/video.c \
 src/input.c \
 src/savestate.c \
 src/romset.c \
 src/ui_tui.c

NO_SDL_SRC = $(BASE_SRC) src/ui_sdl3.c
SDL_SRC = $(BASE_SRC) src/ui_sdl3.c
OBJ = $(NO_SDL_SRC:.c=.o)
SDL_OBJ = $(SDL_SRC:.c=.sdl3.o)
BIN = cv1k_sandbox
SDL_BIN = cv1k_sandbox_sdl3

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(ZLIB_LIBS)

.c.o:
	$(CC) $(CFLAGS) $(ZLIB_CFLAGS) -DCV1K_WITH_ZLIB -Isrc -c $< -o $@

%.sdl3.o: %.c
	$(CC) $(CFLAGS) $(ZLIB_CFLAGS) $(SDL3_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL3 -Isrc -c $< -o $@

sdl3: $(SDL_BIN)

$(SDL_BIN): $(SDL_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SDL_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(SDL3_LIBS)

clean:
	rm -f $(OBJ) $(SDL_OBJ) $(BIN) $(SDL_BIN) tools/mk_dummy_boot tools/mk_blit_demo dummy_u4.bin frame.ppm demo_ram.bin blit_demo.ppm test.ss ddpsdoj_probe.ppm quick.sav

run-dummy: $(BIN) tools/mk_dummy_boot
	./tools/mk_dummy_boot dummy_u4.bin
	./$(BIN) --boot dummy_u4.bin --run-frames 2 --dump-ppm frame.ppm

run-blit-demo: $(BIN) tools/mk_dummy_boot tools/mk_blit_demo
	./tools/mk_dummy_boot dummy_u4.bin
	./tools/mk_blit_demo demo_ram.bin
	./$(BIN) --boot dummy_u4.bin --ram demo_ram.bin --blit 0 --run-frames 2 --dump-ppm blit_demo.ppm

run-ddpsdoj-probe: $(BIN)
	./$(BIN) --model d --romset ../ddpsdoj.zip --probe-title --run-frames 0 --dump-ppm ddpsdoj_probe.ppm

tools/mk_dummy_boot: tools/mk_dummy_boot.c
	$(CC) $(CFLAGS) -o $@ tools/mk_dummy_boot.c

tools/mk_blit_demo: tools/mk_blit_demo.c
	$(CC) $(CFLAGS) -o $@ tools/mk_blit_demo.c
