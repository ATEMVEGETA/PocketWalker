# PocketWalker for Android

This Android port reuses the PocketWalker C++ emulation core and `.pwsav`
container. It runs the emulator with an Android foreground health service,
holds a partial wake lock while active, and feeds the phone accelerometer into
the emulated BMA150 accelerometer used by the original Pokewalker firmware.

The foreground service detects sustained phone motion and supplies a bounded,
zero-centered acceleration waveform to the emulated BMA150. A hardware wake-up
step detector keeps that waveform active for background walking on phones whose
continuous accelerometer sleeps with the screen. The emulated sensor handles
sleep, wake-up delay, conversion latching, data-ready interrupts, and 10-bit
data registers. The original firmware owns its 64-sample history, step
classification, and step pacing.

## ROM and save files

On first launch, select the folder containing `rom.bin`. The app automatically
loads `rom.pwsav` from the same folder. If it does not exist, PocketWalker starts
with a new unsynced save and creates a valid `rom.pwsav` there when progress is
first saved. The folder can be changed from the settings button in the top-right
corner.

The emulator works from a private local mirror for performance. It atomically
loads the selected files into that mirror, saves a complete `rom.pwsav` there,
and then copies the result back to the selected save document. The emulator
keeps running when the app is in the background or the screen is off. Closing
the app saves the current state to the selected `rom.pwsav` and stops the
background service. A checkpoint is also written every 60 seconds while active.

## RTC behavior

The Android build uses the same `.pwsav` RTC metadata and firmware rollover
replay as the desktop build. If continuous emulation was stopped, the next
launch compares the saved RTC metadata with the phone's current date and time
and shows catch-up progress before enabling the controls.

## Infrared networking

The main screen provides separate Connect melonDS and Connect Peer Play
buttons. The settings dialog accepts an optional melonDS PC address override
and provides a Find melonDS PC button for local-network discovery:

- Connect melonDS makes the phone a TCP client. It first checks `127.0.0.1`
  for melonDS running on the same Android device, then tries the optional saved
  PC address and local-network discovery on port 8081. Manual address changes
  apply on Enter or when settings closes. Find melonDS PC clears the fixed
  address, scans the current local network, and fills in the discovered host.
- Automatic Peer Play discovers other Android PocketWalker instances on the
  same local network. Two devices elect their roles automatically and exchange
  the original emulated IR byte stream. Once paired, their busy advertisements
  keep a third instance out of that session.
- Tapping the active connection button again disables local IR networking.

The PC can be connected to the router over Ethernet while the phone uses Wi-Fi.
Both devices only need to be reachable on the same local network.

## Current test build

- Android 10 or newer
- ARM64 phone
- Physical Activity permission used by the continuous health service
- Persistent notification while continuous emulation is active
- Native emulator core compiled with Release optimizations
- Batched Android CPU scheduling with cycle-accurate real-time pacing
- Thread-safe LCD snapshots and a responsive Pokewalker-shaped interface
- Phone acceleration exposed as native 10-bit BMA150 samples without firmware RAM writes
- Step detection performed by the original Pokewalker firmware
- TCP connection to an IR-capable melonDS build over the local network
- Automatic two-phone discovery and Peer Play role selection

Some Android manufacturers can still freeze foreground services when the app's
battery setting is Restricted. Use the system default or Unrestricted setting
if background execution is paused by the phone.

This remains a hardware-test build. Verify button responsiveness, RTC catch-up,
and walking behavior on a physical phone before relying on it for a long
walking session.
