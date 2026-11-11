# Digi 003 on Apple Silicon

A userspace driver that brings the **Digidesign 003 Rack** (FireWire) back to life on **Apple Silicon Macs running macOS 15 Sequoia**: no kernel extension and no SIP changes. Avid never shipped an Apple Silicon driver for the 003 family.

It turns the 003 into:

- a **Core Audio interface**: 18 inputs / 18 outputs at 44.1 or 48 kHz, usable in any app (Logic, Ableton, …);
- a **preamp expander** for another interface: the 003's analog inputs go out its ADAT port by themselves, with no app running, and arrive in your main interface (tested with a Universal Audio Apollo x8p);
- a **MIDI interface**: the 003's MIDI IN/OUT appear as Core MIDI ports.

> ⚠️ **macOS 15 only.** macOS 26 Tahoe removes Apple's FireWire stack, and this driver stops working there. For Tahoe, look at [ASFireWire](https://www.asfw.net/) (DriverKit). Porting this protocol to it is a possible future step.

> **Disclaimer.** This is an independent, community project. It is not affiliated with, endorsed by, or supported by Avid Technology. "Digidesign", "003" and "Pro Tools" are trademarks of their respective owners. It comes with **no warranty** (see [LICENSE](LICENSE)). It talks to your hardware at a low level; use it at your own risk.

## Status

| | |
|---|---|
| Tested hardware | Digidesign **003 Rack**, Mac Studio M4 Max, macOS 15.5 |
| Should work (same protocol, untested) | 002, 002 Rack, 003 (console), 003 Rack+ |
| Sample rates | 44.1 and 48 kHz (88.2/96 kHz not implemented yet) |
| Verified | duplex audio, ADAT clock lock, preamps → ADAT, MIDI in/out, sleep/wake, reboot, hot-plug, 2-minute latency stability (0 late refills) |
| UI language | the menu bar app is currently in **Spanish** (English welcome as a contribution) |

## Requirements

- An Apple Silicon Mac on **macOS 15**.
- Apple's **Thunderbolt 3 (USB-C) to Thunderbolt 2** adapter, Apple's **Thunderbolt to FireWire** adapter, and a FW800→FW400 cable. Plug the adapter into a **real Thunderbolt port**. On a Mac Studio M4 Max the front USB-C ports are *not* Thunderbolt. Docks may not pass Thunderbolt 2 devices through.
- Xcode Command Line Tools (`xcode-select --install`) to build.

## Build & install

```bash
git clone https://github.com/jonathanbeiko/digi003-macos.git
cd digi003-macos
./build-driver.sh          # -> build/Digi003.driver (AudioServerPlugIn)
./install-driver.command   # copies it to /Library/Audio/Plug-Ins/HAL and restarts Core Audio (asks for your password)
./build-menubar.sh         # -> ~/Applications/Digi 003.app
open ~/Applications/"Digi 003.app"
```

"Digi 003 Rack" should now appear in Audio MIDI Setup. In the menu bar app, enable **"Abrir al iniciar sesión"** (open at login).

**Keep the menu bar app running.** It provides the MIDI ports. It also holds a silent connection to the driver: without any client, macOS throttles the driver's host process and the 003's real-time stream would miss deadlines, which shows up as latency jumps and clicks.

To remove everything: `./uninstall-driver.command`.

## Using it as a preamp expander (ADAT)

1. Connect **003 OPTICAL OUT → your interface's ADAT IN**, and **interface ADAT OUT → 003 OPTICAL IN**. The second cable carries the clock.
2. In the menu bar app set **Reloj (clock) → ADAT** and **Salida ADAT → Previos 1-8**. The 003 then follows your interface's sample rate automatically.
3. Record the 003's inputs from your interface's ADAT 1-8.

Measured offset of the 003 preamps arriving over ADAT, versus the main interface's own inputs (Apollo x8p, cross-correlation, constant across runs):

| | Safe latency | Low latency |
|---|---|---|
| 44.1 kHz | 363 samples (8.23 ms) | 274 samples (6.21 ms) |
| 48 kHz | 384 samples (8.00 ms) | 288 samples (6.00 ms) |

In Logic, set the region **Delay** of tracks recorded from ADAT 1-8 to −8.2 ms (safe) or −6.2 ms (low).

## Menu bar app

Shows status, input meters, ADAT clock lock and errors. It controls:

- **ADAT routing:** from the DAW, preamps 1-8, or preamps 1-4 with ADAT 5-8 left to the DAW;
- **latency:** safe or low;
- **clock:** internal, S/PDIF, ADAT or word clock;
- **sample rate** and **optical mode** (ADAT or S/PDIF);
- **restart**, **notifications**, **prevent sleep** and **MIDI**.

It also tells apart "no FireWire adapter", "003 off", "**003 hung**" and "connected but not streaming", and tells you to power-cycle when needed.

## Known issues

- Restarting `coreaudiod` (e.g. by the installer) also stops Universal Audio's **UA Mixer Engine**, and then every Apollo input reads silence. The installer relaunches it.
- The 003 may power up with its outputs **muted** (front MUTE button). This doesn't affect the preamps → ADAT path.
- The FW400 connector on the 003 comes loose easily. Moving cables at the back can cause short bursts of packet errors.
- Twice during development the 003's firmware hung: it stayed on the bus but stopped answering. Both times it followed a crash in the stream shutdown, which is fixed now. If it happens anyway, power-cycle the 003; the app will tell you.

## How it works

| Directory | Contents |
|---|---|
| `engine/` | Isochronous streaming engine built on IOFireWireLib + NuDCL. It handles CIP/AM824, the 003's "double-oh-three" output scrambling, mirroring of the device's packet cadence (the 003 recovers its media clock from it), a host-time clock model for Core Audio timestamps, the ADAT preamp loopback and MIDI. It runs on a time-constraint (real-time) thread. |
| `driver/` | AudioServerPlugIn (`Digi003.driver`). It runs inside the sandboxed Core Audio driver host; `AudioServerPlugIn_IOKitUserClients` grants it the FireWire user client. It stores settings in HAL storage and handles sleep/wake and hot-plug, plus a watchdog for stalls, clock loss and rate changes. Custom properties `d3st`/`d3lv`/`d3rt`/`d3ll`/`d3op`/`d3rs`/`d3ka`/`d3mi`/`d3mo` talk to the app. |
| `menubar/` | Swift menu bar app, the Core MIDI bridge and the keep-alive client. |
| `tools/` | Diagnostics: `d003probe` (register dump), `d003test`/`d003loop` (engine without Core Audio), `halharness` (loads the plug-in with a fake host), `d3ctl` (driver properties), `latxc` (loop latency by cross-correlation), `tsdrift`, `clockcmp`, `chpeak`, `midimon`, `mkaggregate`. |
| `reference/linux-digi00x/` | The Linux driver sources this protocol comes from (GPL-2.0). |

Stream channel order at 44.1/48 kHz: **analog 1-8, S/PDIF L/R, ADAT 1-8**.

## Credits

The 003 protocol was reverse-engineered by others. This project stands on their work:

- **Takashi Sakamoto**: Linux `snd-firewire-digi00x` driver.
- **Robin Gareus and Damien Zammit**: the "double-oh-three" scrambling algorithm (2012).

## License

[GPL-2.0-only](LICENSE), because parts of the engine derive from the Linux driver. You may use, modify and redistribute this code, but distributed modifications must stay under the GPL-2.0 with their source available.
