# SDL 1.2 backend

This revision adds a dedicated SDL 1.2 frontend target while preserving the existing headless build and SDL3 source file.

## Build SDL 1.2 from the supplied source

Example local build used for validation in the sandbox:

```sh
cd SDL
./configure --prefix="$PWD/_install" --disable-shared --enable-static
make depend
make -j2
make install
```

On minimal CI/sandbox systems without X11/ALSA/PulseAudio headers, SDL can be configured with fewer host drivers and still produce a usable compile target:

```sh
./configure --prefix="$PWD/_install" --disable-shared --enable-static \
  --disable-alsa --disable-pulseaudio --disable-esd --disable-arts --disable-nas --disable-sndio \
  --disable-video-x11 --disable-video-dga --disable-video-fbcon --disable-video-directfb --disable-video-svga \
  --disable-video-opengl --disable-input-tslib --disable-cdrom
make depend
make -j2
make install
```

## Build the emulator SDL 1.2 binary

```sh
make sdl12 SDL12_CONFIG=/path/to/sdl-config
```

This produces `cv1k_sandbox_sdl12`.  In this binary, `--sdl` and `--sdl12` both select the SDL 1.2 frontend.

## Run

```sh
./cv1k_sandbox_sdl12 --model d --romset ddpsdoj.zip --mame-speedup --sdl
```

## Controls

Keyboard defaults are the same logical inputs as the headless mapper:

- Movement: arrow keys or W/A/S/D
- P1 buttons: J/K/L/I, with Space/Ctrl/Alt/Shift aliases for common arcade layouts
- P1 start: 1
- Coin 1: 5
- P2 start: 2
- Coin 2: 6
- Service: 9/T/Y
- Save/load quick state: F5/F8
- Quit: Escape

SDL 1.2 joystick support maps axes/hats to P1 directions, buttons 0-3 to P1 buttons, buttons 7/9 to P1 start, and buttons 6/8 to coin.

## Audio path

The SDL 1.2 audio callback drains a stereo signed-16-bit ring buffer.  The emulator thread fills that ring once per emulated frame using the YMZ770C sample clock (`16.384 MHz / 1024 = 16 kHz`) and a fractional frame-rate accumulator based on the CV1000 refresh rate.

`src/ymz770_mame_audio.cpp` supplies the stereo YMZ770C mixer.  It uses the loaded U23/U24 sound ROM bytes, MAME-style phrase and sequence tables, and the standalone MAME-derived AMM/MPEG decoder in `src/mame_mpeg_audio.cpp`.

The existing C register scaffold remains in `src/sound_ymz770.c` for the default/headless build.  The C++ decoder/mixer is linked only by the SDL 1.2 target.
