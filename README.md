# RetroGo

**RetroArch cross-compilation and configuration for the Denon DJ PRIME GO.**

This project packages [RetroArch](https://github.com/libretro/RetroArch) — the official reference frontend for the libretro API — for the Denon PRIME GO embedded DJ controller. It provides Docker-based cross-compilation, a MIDI-to-keyboard input bridge for the DJ control surface, and a ready-to-use configuration with hardware-accelerated OpenGL ES 3.2 rendering via the Mali-T760 GPU, ALSA audio, and 2-player gamepad support.

All emulator cores are standard libretro cores from their respective upstream repositories. RetroGo is a build/deployment wrapper and does not include any copyrighted game data.

## Supported Systems

| Core | System | Cross-Compile Platform |
|------|--------|----------------------|
| **Gearboy** | Game Boy / GBC | `platform=unix` |
| **Snes9x** | SNES / Super Famicom | `platform=classic_armv7_a7` |
| **PCSX-ReARMed** | PlayStation | `platform=unix ARCH=arm BUILTIN_GPU=unai` |

## Hardware Requirements

- Denon DJ PRIME GO (model JP11)
- Root access and SSH enabled on the device
- Network access via Ethernet-over-USB (default IP: `192.168.52.235`)
- ~6 GB free on `/data` partition

> **Getting root and SSH:** Follow the instructions at **[icedream/denon-prime4](https://github.com/icedream/denon-prime4)** to enable SSH and obtain root access on your PRIME GO. Root access must be set up before deploying RetroGo.

### Device Specs

| Component | Detail |
|-----------|--------|
| SoC | Rockchip RK3288 (4x Cortex-A17 @ 1.6 GHz) |
| GPU | Mali-T760 MP4, OpenGL ES 3.2 (`libmali-r1p0.so`) |
| RAM | 2 GB |
| Display | 800×1280 portrait DSI LCD, `/dev/fb0`, 32 bpp XR24 |
| Audio | ALSA `hw:1,0` (JP11 codec) |
| Input | USB MIDI Control Surface (`/dev/snd/midiC0D0`) |
| OS | Buildroot 2023.02.11, glibc 2.36, kernel 6.1.111-rt |
| Storage | `/data` RW (6.6 GB), `/` RO (466 MB) |

## Quick Start

From the launcher menu, select any game. Use PLAY to launch. Press VIEW to open the RetroArch menu or exit back to the launcher.

```
RETROARCH MENU → /data/retrogo.sh
DOOM           → /data/doomprimego
TETRIS         → Gearboy + Tetris.gb
STREET FIGHTER → Snes9x + SF2.sfc
TEKKEN 3       → PCSX-ReARMed + Tekken.cue
```

## 2-Player Controls

The DJ controller's left and right decks are mapped to Player 1 and Player 2 respectively.

### Player 1 — Left Deck (MIDI channel 2)

| DJ Button | MIDI Code | Keyboard | RetroPad | PSX | SNES |
|-----------|-----------|----------|----------|-----|------|
| PLAY | ch2 note 10 | `X` | A | Cross | LP |
| CUE | ch2 note 9 | `Z` | B | Circle | MP |
| Pitch − | ch2 note 29 | `S` | X | Square | LK |
| Pitch + | ch2 note 30 | `A` | Y | Triangle | MK |
| HC4 (4th pad) | ch2 note 18 | `Q` | L1 | L1 | HP |
| ROLL (Pad Mode) | ch2 note 13 | `W` | R1 | R1 | HK |
| FX Assign | ch4 note 11 | `E` | L2 | L2 | — |
| FX Time | ch4 note 12 | `R` | R2 | R2 | — |
| LOOP (Pad Mode Loops) | ch2 note 12 | `↑` | D-Up | D-Up | D-Up |
| HC1 (1st pad) | ch2 note 15 | `←` | D-Left | D-Left | D-Left |
| HC2 (2nd pad) | ch2 note 16 | `↓` | D-Down | D-Down | D-Down |
| HC3 (3rd pad) | ch2 note 17 | `→` | D-Right | D-Right | D-Right |
| Jog wheel | CC 55+77 | `←/→` | D-Left/Right | — | — |
| SweepFX A | ch0 note 14 | `RShift` | Select | Select | Select |
| SweepFX B | ch0 note 15 | `Enter` | Start | Start | Start |
| VIEW | ch15 note 7 | `Backspace` | Menu | Menu | Menu |

### Player 2 — Right Deck (MIDI channel 3, same notes)

| DJ Button | Keyboard | RetroPad |
|-----------|----------|----------|
| PLAY | `C` | P2 A |
| CUE | `D` | P2 B |
| Pitch − | `V` | P2 X |
| Pitch + | `H` | P2 Y |
| HC4 | `T` | P2 L1 |
| ROLL | `B` | P2 R1 |
| Mic 1 | `U` | P2 L2 |
| Mic 2 | `N` | P2 R2 |
| LOOP | `I` | P2 Up |
| HC1 | `J` | P2 Left |
| HC2 | `K` | P2 Down |
| HC3 | `L` | P2 Right |
| SweepFX A/B | shared | P2 Select / Start |

## Project Structure

| File | Purpose |
|------|---------|
| `Dockerfile` | ARM cross-compilation toolchain image |
| `Dockerfile.retroarch` | Builds RetroArch from source with Mali GLES patches applied |
| `retroarch-gl2-fbo.patch` | Patch for RetroArch's GL2 driver — prevents default framebuffer binding so the Mali context driver can use an offscreen FBO for rotation |
| `mali_fbdev_ctx.c` | Replacement for RetroArch's `gfx/drivers_context/mali_fbdev_ctx.c` — adds offscreen FBO with 90° rotation for portrait displays |
| `midigamepad.c` | Standalone daemon: reads raw MIDI from `/dev/snd/midiC0D0` and creates a uinput keyboard |
| `build.sh` | One-command RetroArch build |
| `build_cores.sh` | Batch builds Gearboy, Snes9x, PCSX-ReARMed libretro cores |
| `.github/workflows/build.yml` | CI: builds RetroArch and MIDI bridge on every push |

## Build Instructions

All builds use Docker for cross-compilation. No native ARM toolchain is needed on the host.

### Prerequisites

- Docker
- `sshpass` (for deployment to the device)

### 1. Build the Toolchain Image

```bash
docker build -t primego-toolchain .
```

This creates a Debian Bookworm image with `arm-linux-gnueabihf-gcc`, Mali GPU headers, ALSA, and all dependencies needed for cross-compilation.

### 2. Build RetroArch

```bash
./build.sh
```

Or manually:

```bash
docker build -t retrogo-builder -f Dockerfile.retroarch .
docker run --rm -v "$(pwd):/output" retrogo-builder cp /retroarch /output/
```

This applies our patches on top of upstream RetroArch (`retroarch-gl2-fbo.patch`, `mali_fbdev_ctx.c`) and produces a 12 MB stripped `retroarch` ARM binary.

### 3. Build the MIDI Gamepad Bridge

```bash
docker run --rm -v "$(pwd):/work" primego-toolchain sh -c '
  arm-linux-gnueabihf-gcc -O2 -static -o /work/midigamepad /work/midigamepad.c -lpthread
  arm-linux-gnueabihf-strip /work/midigamepad
'
```

### 4. Build Libretro Cores

```bash
chmod +x build_cores.sh
./build_cores.sh
```

Cores are output to `../cores/`. The script automatically builds the toolchain image if needed.

Or build individually:

```bash
# Gearboy (Game Boy/GBC)
docker run --rm -v "$(pwd):/out" primego-toolchain sh -c '
  cd /tmp && git clone --depth 1 https://github.com/drhelius/Gearboy.git
  cd Gearboy/platforms/libretro
  make -f Makefile platform=unix CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ -j4
  arm-linux-gnueabihf-strip gearboy_libretro.so && cp gearboy_libretro.so /out/
'

# Snes9x
docker run --rm -v "$(pwd):/out" primego-toolchain sh -c '
  cd /tmp && git clone --depth 1 https://github.com/libretro/snes9x.git
  cd snes9x/libretro
  make -f Makefile platform=classic_armv7_a7 CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ -j4
  arm-linux-gnueabihf-strip snes9x_libretro.so && cp snes9x_libretro.so /out/
'

# PCSX-ReARMed
docker run --rm -v "$(pwd):/out" primego-toolchain sh -c '
  cd /tmp && git clone --depth 1 https://github.com/libretro/pcsx_rearmed.git
  cd pcsx_rearmed
  make -f Makefile.libretro CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ \
    HAVE_NEON=0 BUILTIN_GPU=unai ARCH=arm -j4
  arm-linux-gnueabihf-strip pcsx_rearmed_libretro.so && cp pcsx_rearmed_libretro.so /out/
'
```

## Deploy

All binaries go to `/data/` on the Prime Go:

```
/data/
├── retroarch                    # RetroArch binary
├── retroarch.cfg                # RetroArch configuration
├── midigamepad                  # MIDI-to-keyboard bridge
├── retrogo.sh                   # Launcher script
├── evtest                       # Input debug tool
├── cores/
│   ├── gearboy_libretro.so
│   ├── snes9x_libretro.so
│   └── pcsx_rearmed_libretro.so
└── roms/
    ├── snes/
    └── psx/
```

### Deploy RetroArch

```bash
sshpass -p 'denonprime4' scp retroarch root@192.168.52.235:/data/
```

### Deploy Cores

```bash
sshpass -p 'denonprime4' ssh root@192.168.52.235 mkdir -p /data/cores
sshpass -p 'denonprime4' scp *.so root@192.168.52.235:/data/cores/
```

### Deploy ROMs

```bash
sshpass -p 'denonprime4' ssh root@192.168.52.235 mkdir -p /data/roms/snes /data/roms/psx
sshpass -p 'denonprime4' scp game.sfc root@192.168.52.235:/data/roms/snes/
```

## Running a Game

Always stop Engine DJ first, then start the MIDI bridge and RetroArch:

```bash
systemctl stop engine.service
killall -9 engine

# Start MIDI bridge
/data/midigamepad &

# Run game
cd /data && ./retroarch --config /data/retroarch.cfg \
  -L /data/cores/snes9x_libretro.so \
  "/data/roms/snes/Street Fighter II (USA).sfc"
```

Or use the convenience script:

```bash
/data/retrogo.sh /data/cores/snes9x_libretro.so "/data/roms/snes/game.sfc"
```

## Launcher Integration

The `primego-launcher` (separate project) provides a graphical menu for game selection. The launcher config at `/data/launcher.conf` uses the format:

```
LABEL | COMMAND
```

Entries call `/data/retrogo.sh` which handles stopping Engine, starting the MIDI bridge, and launching RetroArch. When RetroArch exits, the launcher restarts automatically.

## Display Orientation

The Prime Go has a portrait 800×1280 display. RetroArch renders everything at 1280×800 (landscape) internally and uses `video_rotation = 1` (90° clockwise) to present correctly on the portrait screen.

Game content fills the viewport correctly. The menu is rendered by the OZONE driver in its own coordinate system and appears in portrait orientation — this is a known limitation. The launcher handles game selection, so the menu is only needed for in-game settings.

## Debugging

### Check MIDI Events

```bash
/data/mididebug3
```
Press any button — raw hex MIDI data is printed.

### Check Keyboard Events

```bash
/data/midigamepad &
/data/evtest /dev/input/event2
```

Shows all keyboard events generated from DJ controller input.

### Verify Process Status

```bash
ps | grep -E "retroarch|midigamepad|launcher"
fuser /dev/snd/midiC0D0  # Check who owns the MIDI device
```

### Kill Everything

```bash
killall -9 retroarch midigamepad launcher
fuser -k /dev/snd/midiC0D0
```

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| No input works | MIDI bridge not running | `./midigamepad &` |
| Game too fast | `video_vsync = false` | Set `video_vsync = true` in config |
| Fast forward triggers | RetroArch hotkey conflict | Add `input_toggle_fast_forward = "nul"` to config |
| Menu is portrait | OZONE doesn't respect rotation | Known limitation; use launcher for game selection |
| P2 buttons don't work | Old MIDI bridge binary | Kill old process, redeploy, restart |
| MIDI device in use | Stale process holding /dev/snd/midiC0D0 | `fuser -k /dev/snd/midiC0D0` |

## Credits

- **[ghuntley](https://github.com/ghuntley)** and **[icedream](https://github.com/icedream)** — Pioneered SSH and root access on the Denon PRIME series
- **[icedream/denon-prime4](https://github.com/icedream/denon-prime4)** — Required prerequisite for enabling SSH and root on the PRIME GO

## Acknowledgments

- **[RetroArch](https://github.com/libretro/RetroArch)** — The official libretro frontend
- **[primego-launcher](https://github.com/erhan-/primelauncher)** — Graphical game launcher for the PRIME GO
- **[Gearboy](https://github.com/drhelius/Gearboy)** — Game Boy / GBC emulator (libretro core)
- **[Snes9x](https://github.com/libretro/snes9x)** — SNES emulator (libretro core)
- **[PCSX-ReARMed](https://github.com/libretro/pcsx_rearmed)** — PlayStation emulator (libretro core)

## License

MIT

## Disclaimer

This project is not affiliated with, endorsed by, or connected to Denon DJ, inMusic, Nintendo, Sony, or any other trademark holder. All trademarks and registered trademarks are the property of their respective owners.

This project does not distribute any copyrighted ROMs, BIOS files, or game data. Users must supply their own legally obtained game files. The libretro cores included are open-source emulators and do not contain proprietary code.

RetroGo is a community project for educational purposes and personal use on legally owned hardware.

