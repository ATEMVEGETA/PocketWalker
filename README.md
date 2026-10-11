<div align="center">

# pocketwalker - An H8/38606F emulator built for the PokéWalker

[![Release](https://img.shields.io/github/release/ATEMVEGETA/pocketwalker?color=CCC)](https://github.com/ATEMVEGETA/pocketwalker/releases)
[![Downloads](https://img.shields.io/github/downloads/ATEMVEGETA/pocketwalker/total?color=CCC)](https://github.com/ATEMVEGETA/pocketwalker/releases)

<img alt="PocketWalker" src=".github/images/cover.gif" width="400" />

</div>

---

## About This Fork

This is an unofficial fork of
[h4lfheart/pocketwalker](https://github.com/h4lfheart/pocketwalker), extended
with custom Windows and Android builds. The original emulator and its authors
remain credited through the upstream repository and project history.

### Custom Features

- Android frontend with background and screen-off walking support
- Phone motion converted into emulated BMA150 accelerometer samples
- Step counting handled by the original Pokewalker firmware, without direct
  step or Watt RAM edits
- `.pwsav` files combining EEPROM data, emulator state, and RTC metadata
- Firmware-based RTC catch-up that replays missed midnights
- RTC initialization only after the first Nintendo DS sync
- TCP IR connection with compatible desktop and Android melonDS builds
- Automatic Android-to-Android Peer Play discovery and role selection
- NT7508 LCD controller support adapted from the upstream implementation
- H8 CPU fixes for `MULXS.W` and `BSR d:16`

Prebuilt Android and standalone Windows versions are available from
[Releases](https://github.com/ATEMVEGETA/pocketwalker/releases). See the
[Android documentation](android/README.md) for setup and behavior details.

## Features

### H8/300H CPU
- Full instruction set used by the PokéWalker
- Support for interrupts

### H8/38606F
- Memory Bus with 48KB ROM, 2KB RAM, and IO
- Synchronous Serial Communication Unit (SSU)
- Serial Communication Interface 3 (SCI3)
- Timer B1
- Timer W
- Real-Time Clock (RTC)
- A/D Converter (ADC)

### PokéWalker
- Accelerometer (BMA150)
- EEPROM (M95512)
- LCD (NT7508)
- Buzzer
- IR Communications over TCP with [melonDS-IR](https://github.com/DaveuRrr/melonDS-IR)


## Building From Source

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 MinGW64** terminal from the Start menu
2. Update packages and reopen the terminal if prompted:
```bash
pacman -Syu
```
3. Install git and clone the repository:
```bash
pacman -S git
git clone https://github.com/ATEMVEGETA/pocketwalker.git --recursive
cd pocketwalker
```

### Dynamic

4. Install dependencies:
```bash
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base,qt6-multimedia}
```
5. Build:
```bash
./build.sh
```

### Static (Standalone)

4. Install build tools:
```bash
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja}
```
5. Install and set up [vcpkg](https://vcpkg.io/) with `VCPKG_ROOT` set in your environment
6. Build:
```bash
./build.sh --static
```

The output binary will be located in `./release/`.
