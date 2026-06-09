CC ?= cc
CXX ?= c++
CFLAGS ?= -std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -g3 -DNDEBUG -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden
CXXFLAGS ?= -std=gnu++23 -Wall -Wextra -O3 -g3 -DNDEBUG -fno-semantic-interposition -fipa-pta -fno-math-errno -fno-rtti -fvisibility=hidden
DYNAREC_CXXFLAGS ?=
LDFLAGS ?= -flto
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
override CXXFLAGS += -DCV1K_CACHE_ACCURATE=1
endif

BASE_SRC = \
 src/main.c \
 src/platform.c \
 src/mame_cv1k_derived.c \
 src/bus.c \
 src/emu.c \
 src/nand.c \
 src/rtc9701.c \
 src/sound_ymz770.c \
 src/video.c \
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
# C++ SH-3 core and YMZ770 audio implementation shared by every target
CORE_CPP = src/sh3_core.cpp src/mame_mpeg_audio.cpp src/ymz770_mame_audio.cpp
SDL12_CPP_SRC = $(CORE_CPP)

OBJ = $(NO_SDL_SRC:.c=.o) $(CORE_CPP:.cpp=.o)
SDL3_OBJ = $(SDL3_SRC:.c=.sdl3.o) $(CORE_CPP:.cpp=.sdl3.o)
SDL12_C_OBJ = $(SDL12_C_SRC:.c=.sdl12.o)
SDL12_CPP_OBJ = $(SDL12_CPP_SRC:.cpp=.sdl12.o)
BIN = cv1k_sandbox
SDL3_BIN = cv1k_sandbox_sdl3
SDL12_BIN = cv1k_sandbox_sdl12

all: $(BIN)

# Final link uses the C++ driver because the SH-3 core is C++.
$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(THREAD_LIBS)

.c.o:
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) -DCV1K_WITH_ZLIB -Isrc -c $< -o $@

src/sh3_core.o: src/sh3_core.cpp
	$(CXX) $(CXXFLAGS) $(DYNAREC_CXXFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) -DCV1K_WITH_ZLIB -Isrc -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DYNAREC_CXXFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) -DCV1K_WITH_ZLIB -Isrc -c $< -o $@

%.sdl3.o: %.c
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) $(SDL3_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL3 -Isrc -c $< -o $@

%.sdl3.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DYNAREC_CXXFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) $(SDL3_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL3 -Isrc -c $< -o $@

sdl3: $(SDL3_BIN)

$(SDL3_BIN): $(SDL3_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(SDL3_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(SDL3_LIBS) $(THREAD_LIBS)

%.sdl12.o: %.c
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) $(ZLIB_CFLAGS) $(SDL12_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL12 -DCV1K_DEFAULT_SDL12 -Isrc -c $< -o $@

%.sdl12.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DYNAREC_CXXFLAGS) $(THREAD_CFLAGS) $(SDL12_CFLAGS) -DCV1K_WITH_ZLIB -DCV1K_WITH_SDL12 -DCV1K_DEFAULT_SDL12 -Isrc -c $< -o $@

sdl12: $(SDL12_BIN)

$(SDL12_BIN): $(SDL12_C_OBJ) $(SDL12_CPP_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(SDL12_C_OBJ) $(SDL12_CPP_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(SDL12_LIBS) $(THREAD_LIBS)

PGO_DIR ?= build/pgo
PGO_ROMSET ?= ../ddpsdoj.zip
PGO_FRAMES ?= 1000

pgo-generate:
	$(MAKE) clean
	rm -rf $(PGO_DIR)
	mkdir -p $(PGO_DIR)
	$(MAKE) THREADS=$(THREADS) CFLAGS="-std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -DNDEBUG -fprofile-generate=$(PGO_DIR) -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden" CXXFLAGS="-std=gnu++23 -Wall -Wextra -O3 -DNDEBUG -fprofile-generate=$(PGO_DIR) -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fno-rtti -fvisibility=hidden" LDFLAGS="-fprofile-generate=$(PGO_DIR)" $(BIN)
	./$(BIN) --model d --romset $(PGO_ROMSET) --run-frames $(PGO_FRAMES) --headless-present-check --c23-jit >/dev/null

pgo-use:
	$(MAKE) clean
	$(MAKE) THREADS=$(THREADS) CFLAGS="-std=gnu23 -Wall -Wextra -Wno-gnu-case-range -O3 -DNDEBUG -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-missing-profile -flto -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fvisibility=hidden" CXXFLAGS="-std=gnu++23 -Wall -Wextra -O3 -DNDEBUG -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-missing-profile -flto -fomit-frame-pointer -fno-semantic-interposition -fipa-pta -fno-math-errno -fno-rtti -fvisibility=hidden" LDFLAGS="-flto -fprofile-use=$(PGO_DIR) -fprofile-correction" $(BIN)

clean:
	rm -f $(OBJ) $(SDL3_OBJ) $(SDL12_C_OBJ) $(SDL12_CPP_OBJ) $(BIN) $(SDL3_BIN) $(SDL12_BIN) tools/mk_dummy_boot tools/mk_blit_demo dummy_u4.bin frame.ppm demo_ram.bin blit_demo.ppm test.ss ddpsdoj_probe.ppm quick.sav

run-dummy: $(BIN) tools/mk_dummy_boot
	./tools/mk_dummy_boot dummy_u4.bin
	./$(BIN) --boot dummy_u4.bin --run-frames 2 --dump-ppm frame.ppm

run-blit-demo: $(BIN) tools/mk_dummy_boot tools/mk_blit_demo
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
	$(CXX) $(CXXFLAGS) -o $@ tools/jit_difftest.o $(DIFFTEST_OBJ) $(LDFLAGS) $(ZLIB_LIBS) $(THREAD_LIBS)
jit-difftest: tools/jit_difftest
	./tools/jit_difftest

tools/mk_dummy_boot: tools/mk_dummy_boot.c
	$(CC) $(CFLAGS) -o $@ tools/mk_dummy_boot.c

tools/mk_blit_demo: tools/mk_blit_demo.c
	$(CC) $(CFLAGS) -o $@ tools/mk_blit_demo.c
