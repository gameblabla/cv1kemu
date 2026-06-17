CC ?= cc
CFLAGS ?= -std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -g3 -DNDEBUG -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden
LDFLAGS ?= -flto

# Performance-oriented local build default.  The emulator/JIT is CPU-bound and
# x64-host-focused; allowing the compiler to use the current host ISA and direct
# external calls consistently improves the SH-3 JIT benchmark.  Set
# HOST_TUNE=generic for portable distribution binaries.
HOST_TUNE ?= native
ifeq ($(HOST_TUNE),native)
override CFLAGS += -march=native -mtune=native -fno-plt
endif

ZLIB_CFLAGS ?= $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS ?= $(shell pkg-config --libs zlib 2>/dev/null || echo -lz)
SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null)
SDL12_CONFIG ?= sdl-config
SDL12_CFLAGS ?= $(shell $(SDL12_CONFIG) --cflags 2>/dev/null)
SDL12_LIBS ?= $(shell $(SDL12_CONFIG) --libs 2>/dev/null)
THREADS ?= 1
ifeq ($(THREADS),1)
THREAD_CFLAGS = -DCV1K_ENABLE_THREADS
THREAD_LIBS = -pthread
else
THREAD_CFLAGS =
THREAD_LIBS =
endif

# SH7709S cache-accuracy model (see CV1K_CACHE_ACCURATE in src/cv1k_config.h).
#   make ...                -> fast build, cache hooks compiled out (default)
#   make ... CACHE=accurate -> MAME 0.288-style accurate cache timing
CACHE ?= fast
ifeq ($(CACHE),accurate)
override CFLAGS += -DCV1K_CACHE_ACCURATE=1
endif

BASE_SRC = \
 src/main.c \
 src/platform.c \
 src/cv1k_frontend.c \
 src/mame_cv1k_derived.c \
 src/mame_mpeg_audio.c \
 src/ymz770_mame_audio.c \
 src/sh3_core.c \
 src/bus.c \
 src/emu.c \
 src/nand.c \
 src/rtc9701.c \
 src/sound_ymz770.c \
 src/video.c \
 src/zmbv_recorder.c \
 src/video_gles2.c \
 src/input.c \
 src/savestate.c \
 src/romset.c \
 src/sh3_jit/sh3_jit.c \
 src/sh3_jit/sh3_jit_x64.c \
 src/sh3_jit/cv1k_ir.c \
 src/threaded_runtime.c \
 src/ui_tui.c

NO_SDL_SRC = $(BASE_SRC) src/ui_sdl3.c src/ui_sdl12.c
SDL3_SRC = $(BASE_SRC) src/ui_sdl3.c src/ui_sdl12.c
SDL12_C_SRC = $(BASE_SRC) src/ui_sdl3.c src/ui_sdl12.c
OBJ = $(NO_SDL_SRC:.c=.o)
SDL3_OBJ = $(SDL3_SRC:.c=.sdl3.o)
SDL12_C_OBJ = $(SDL12_C_SRC:.c=.sdl12.o)
BIN = cv1k_sandbox
SDL3_BIN = cv1k_sandbox_sdl3
SDL12_BIN = cv1k_sandbox_sdl12

all: $(BIN)

wasm:
	$(MAKE) -f Makefile.wasm

win64:
	$(MAKE) -f Makefile.win64

qt6:
	$(MAKE) -f Makefile.qt6

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(THREAD_LIBS) -lm

.c.o:
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) -DCV1K_WITH_ZLIB -Isrc -c $< -o $@

%.sdl3.o: %.c
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) $(SDL3_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL3 -Isrc -c $< -o $@

sdl3: $(SDL3_BIN)

$(SDL3_BIN): $(SDL3_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SDL3_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(SDL3_LIBS) $(THREAD_LIBS) -lm

%.sdl12.o: %.c
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) $(SDL12_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL12 -DCV1K_DEFAULT_SDL12 -Isrc -c $< -o $@

sdl12: $(SDL12_BIN)

$(SDL12_BIN): $(SDL12_C_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SDL12_C_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(SDL12_LIBS) $(THREAD_LIBS) -lm

PGO_DIR ?= build/pgo
PGO_ROMSET ?= ../ddpsdoj.zip
PGO_FRAMES ?= 1000
VIDEO_CHECK_ROMSET ?= ../mmpork.zip
VIDEO_CHECK_FRAMES ?= 4000
VIDEO_CHECK_ARGS ?=

pgo-generate:
	$(MAKE) clean
	rm -rf $(PGO_DIR)
	mkdir -p $(PGO_DIR)
	$(MAKE) THREADS=$(THREADS) CFLAGS="-std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -DNDEBUG -fprofile-generate=$(PGO_DIR) -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden" LDFLAGS="-fprofile-generate=$(PGO_DIR)" $(BIN)
	./$(BIN) --model d --romset $(PGO_ROMSET) --run-frames $(PGO_FRAMES) --headless-present-check --c23-jit >/dev/null

pgo-use:
	$(MAKE) clean
	$(MAKE) THREADS=$(THREADS) CFLAGS="-std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -DNDEBUG -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-missing-profile -flto -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden" LDFLAGS="-flto -fprofile-use=$(PGO_DIR) -fprofile-correction" $(BIN)

clean:
	$(MAKE) -f Makefile.wasm clean >/dev/null 2>&1 || true
	$(MAKE) -f Makefile.win64 clean >/dev/null 2>&1 || true
	$(MAKE) -f Makefile.qt6 clean >/dev/null 2>&1 || true
	rm -f $(OBJ) $(SDL3_OBJ) $(SDL12_C_OBJ) $(BIN) $(SDL3_BIN) $(SDL12_BIN) tools/jit_difftest tools/jit_difftest.o tools/mk_dummy_boot tools/mk_blit_demo dummy_u4.bin frame.ppm demo_ram.bin blit_demo.ppm test.ss ddpsdoj_probe.ppm quick.sav
	rm -rf build/video_check build/bench build/check3000 build/video_smoke

run-dummy: $(BIN) tools/mk_dummy_boot
	./tools/mk_dummy_boot dummy_u4.bin
	./$(BIN) --boot dummy_u4.bin --run-frames 2 --dump-ppm frame.ppm

run-blit-demo: $(BIN) tools/jit_difftest tools/jit_difftest.o tools/mk_dummy_boot tools/mk_blit_demo
	./tools/mk_dummy_boot dummy_u4.bin
	./tools/mk_blit_demo demo_ram.bin
	./$(BIN) --boot dummy_u4.bin --ram demo_ram.bin --blit 0 --run-frames 2 --dump-ppm blit_demo.ppm

run-ddpsdoj-probe: $(BIN)
	./$(BIN) --model d --romset ../ddpsdoj.zip --probe-title --run-frames 0 --dump-ppm ddpsdoj_probe.ppm

# SH-3 JIT differential tester: JIT block vs interpreter equivalence.
DIFFTEST_OBJ = $(filter-out src/main.o,$(OBJ))
tools/jit_difftest.o: tools/jit_difftest.c
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) -DCV1K_WITH_ZLIB -Isrc -c $< -o $@
tools/jit_difftest: tools/jit_difftest.o $(DIFFTEST_OBJ)
	$(CC) $(CFLAGS) -o $@ tools/jit_difftest.o $(DIFFTEST_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(THREAD_LIBS) -lm
jit-difftest: tools/jit_difftest
	./tools/jit_difftest

tools/mk_dummy_boot: tools/mk_dummy_boot.c
	$(CC) $(CFLAGS) -o $@ tools/mk_dummy_boot.c

tools/mk_blit_demo: tools/mk_blit_demo.c
	$(CC) $(CFLAGS) -o $@ tools/mk_blit_demo.c


video-check: $(BIN)
	rm -rf build/video_check
	mkdir -p build/video_check
	./$(BIN) --model d --romset $(VIDEO_CHECK_ROMSET) --cpu-backend interp --run-frames $(VIDEO_CHECK_FRAMES) --dump-display-zmbv build/video_check/interp.mkv --dump-zmbv-checks build/video_check/interp.txt $(VIDEO_CHECK_ARGS)
	./$(BIN) --model d --romset $(VIDEO_CHECK_ROMSET) --c23-jit --run-frames $(VIDEO_CHECK_FRAMES) --dump-display-zmbv build/video_check/c23.mkv --dump-zmbv-checks build/video_check/c23.txt $(VIDEO_CHECK_ARGS)
	diff -u build/video_check/interp.txt build/video_check/c23.txt
	./$(BIN) --model d --romset $(VIDEO_CHECK_ROMSET) --ir-jit --run-frames $(VIDEO_CHECK_FRAMES) --dump-display-zmbv build/video_check/ir.mkv --dump-zmbv-checks build/video_check/ir.txt $(VIDEO_CHECK_ARGS)
	diff -u build/video_check/interp.txt build/video_check/ir.txt
	@echo "video-check passed: build/video_check/{interp,c23,ir}.mkv and .txt"

BENCH_ROMSET ?= ../mmpork.zip
BENCH_FRAMES ?= 3000
BENCH_ARGS ?= --headless-present-check
bench-jit: $(BIN)
	@mkdir -p build/bench
	@echo "benchmark: frames=$(BENCH_FRAMES) romset=$(BENCH_ROMSET) args=$(BENCH_ARGS)"
	@echo "-- c23-jit --"
	@/usr/bin/time -f 'elapsed=%e user=%U sys=%S' ./$(BIN) --model d --romset $(BENCH_ROMSET) --run-frames $(BENCH_FRAMES) --c23-jit $(BENCH_ARGS) >build/bench/c23.txt
	@echo "-- ir-jit --"
	@/usr/bin/time -f 'elapsed=%e user=%U sys=%S' ./$(BIN) --model d --romset $(BENCH_ROMSET) --run-frames $(BENCH_FRAMES) --ir-jit $(BENCH_ARGS) >build/bench/ir.txt
	@echo "bench logs: build/bench/{c23,ir}.txt"


.PHONY: qt6 pgo-ir
pgo-ir:
	$(MAKE) clean
	$(MAKE) THREADS=$(THREADS) CFLAGS="-std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -DNDEBUG -fprofile-generate=$(PGO_DIR) -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden -march=native -mtune=native -fno-plt" LDFLAGS="-fprofile-generate=$(PGO_DIR)" $(BIN)
	./$(BIN) --model d --romset $(PGO_ROMSET) --run-frames $(PGO_FRAMES) --ir-jit --profile-sh3-only >/dev/null
	$(MAKE) clean
	$(MAKE) THREADS=$(THREADS) CFLAGS="-std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -DNDEBUG -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-missing-profile -flto -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden -march=native -mtune=native -fno-plt" LDFLAGS="-flto -fprofile-use=$(PGO_DIR) -fprofile-correction" $(BIN)
