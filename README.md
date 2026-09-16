# omarchy-sony

> **Fork for the WH-1000XM3.** This is a fork of
> [andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony),
> which targets the WH-1000XM5 and Sony's MDR **v2** protocol. The XM3 speaks
> MDR **v1**: different service UUID, different RFCOMM channel, different
> opcodes, and a mandatory handshake. A v2 client connects fine to an XM3 and
> then does nothing at all, because the headset ACKs unknown commands and
> silently ignores them.
>
> Every byte here was captured from a real WH-1000XM3 on firmware 4.5.2 and is
> documented in [`docs/xm3-protocol.md`](docs/xm3-protocol.md). Upstream is MIT
> licensed and the original copyright is preserved in [LICENSE](LICENSE).

A modern, lightweight Omarchy bar-widget plugin and headless C++20 background daemon to manage **Sony WH-1000XM3** (and compatible Sony MDR v1) headphones on Linux.

Provides real-time battery monitoring, Noise Cancellation (ANC / Ambient / Off) mode switching, granular ambient sound level controls, Equalizer presets, and smart feature toggles directly from the Omarchy desktop shell or command line.

---

## Features

- 🔋 **Live Battery & Codec Monitoring:** Real-time battery percentage, charging state, and active Bluetooth audio codec (e.g. LDAC, AAC, SBC).
- 🎧 **Noise Control Modes:** Seamless hardware switching between **ANC (Noise Canceling)**, **Ambient Sound**, and **Off** (passive).
- 🔊 **Ambient Sound Level Slider:** Granular adjustment of ambient passthrough (levels 0–20) with Focus on Voice support.
- 🎛️ **Equalizer Profiles:** Switch presets (*Off, Bright, Excited, Mellow, Relaxed, Vocal, Treble Boost, Bass Boost, Speech, Custom*) and configure 5-band custom EQ + Clear Bass.
- ⚙️ **Smart Features:** DSEE HX upscaling toggle.
- 🚫 **Not on the XM3:** Speak-to-Chat, Multipoint and wearing detection are XM4/XM5 features. The headset does not answer those commands, so the plugin no longer exposes them.
- ⌨️ **Keyboard Navigation:** Full vim-style navigation (`h`/`j`/`k`/`l`, `Enter`, `Esc`) inside the panel dropdown.
- 💻 **Standalone CLI (`sony-ctl`):** Full terminal and scripting interface for all headphone controls.
- ⚡ **Zero Polling & Lightweight:** Native BlueZ RFCOMM transport with reactive file-view event updates.

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   Omarchy Shell (QML)                  │
│   ┌───────────────┐ ┌─────────────┐ ┌──────────────┐   │
│   │   SonyIcon    │ │  Service    │ │    Panel     │   │
│   └───────▲───────┘ └──────▲──────┘ └──────▲───────┘   │
│           │                │               │           │
│           └────────────────┼───────────────┘           │
│                            │ watches                   │
│                            │ (FileView)                │
│                 ~/.local/state/sony-headphones/        │
│                           status.json                  │
│                                ▲                       │
│                                │ writes                │
└────────────────────────────────┼───────────────────────┘
                                 │
┌────────────────────────────────┼───────────────────────┐
│  Headless Daemon               │   Companion CLI       │
│  (sony-headphones-daemon)      │   (sony-ctl)          │
│                                │           │           │
│   UNIX Domain Socket ◄─────────┴───────────┘           │
│   (/run/user/$UID/sony-headphones.sock)                │
│                 │                                      │
│                 ▼                                      │
│   Bluetooth RFCOMM Stack (MDR v1 Protocol)             │
│                 │ (channel resolved over SDP)          │
│                 ▼                                      │
│       Sony WH-1000XM3 Headset                          │
└────────────────────────────────────────────────────────┘
```

- **`plugin/`**: Quickshell / QML plugin conforming to Omarchy manifest specification version 1.
- **`daemon/`**: Headless C++20 background daemon managing Bluetooth RFCOMM connection and UNIX socket IPC.
- **`cli/`**: Lightweight CLI tool (`sony-ctl`) for terminal queries and control scripts.

---

## Prerequisites

On Arch Linux / Omarchy:
```bash
sudo pacman -S --needed base-devel cmake ninja bluez bluez-libs dbus jq
```

On Debian / Ubuntu:
```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build libbluetooth-dev libdbus-1-dev jq
```

---

## Installation & Setup

Clone the repository and run the automated setup script:

```bash
git clone https://github.com/felixdacraft/omarchy-sony.git
cd omarchy-sony
./setup
```

The `./setup` script will:
1. Verify system dependencies.
2. Build the daemon and CLI binaries with CMake and Ninja.
3. Install `sony-headphones-daemon` and `sony-ctl` to `~/.local/bin/`.
4. Register and start the `sony-headphones.service` user systemd unit.
5. Deploy the QML plugin to `~/.config/omarchy/plugins/io.github.felixdacraft.omasony`.

---

## CLI Usage (`sony-ctl`)

You can query or control your headphones from anywhere via `sony-ctl`:

```bash
# Query full headphone status as JSON
sony-ctl status

# Switch Noise Cancellation Modes
sony-ctl noise anc          # Turn on Active Noise Cancellation
sony-ctl noise ambient      # Switch to Ambient Sound mode
sony-ctl noise off          # Turn off noise processing

# Adjust Ambient Sound Level (0 - 20)
sony-ctl ambient-level 12

# Equalizer Presets
sony-ctl eq bright
sony-ctl eq vocal
sony-ctl eq bass
sony-ctl eq custom 0 2 4 2 0 5  # 5 bands (-10..10) + Clear Bass (-10..10)

# Toggle Smart Features
sony-ctl dsee on
```

---

## Systemd Service Management

The background daemon is managed automatically via user systemd:

```bash
# Check daemon status and logs
systemctl --user status sony-headphones.service
journalctl --user -u sony-headphones.service -f

# Restart daemon
systemctl --user restart sony-headphones.service
```

---

## Testing

Run unit tests and end-to-end integration tests:

```bash
# C++ Protocol Unit Tests
cmake --build build --target test

# JavaScript Model Unit Tests (requires Deno or Node.js)
deno run --allow-read tests/model.test.js

# End-to-End Simulation Tests
./tests/integration.sh
```

---

## Documentation

The **WH-1000XM3 MDR v1 wire protocol** — service UUID, RFCOMM channel, handshake,
every verified opcode and the commands the XM3 does *not* answer — is documented in
[`docs/xm3-protocol.md`](docs/xm3-protocol.md).

[`docs/protocol-guide.md`](docs/protocol-guide.md) is upstream's survey of third-party
tooling for the WH-1000XM5, kept for reference.

---

## Acknowledgments & References

This project builds upon and draws inspiration from these open-source projects:

1. **[thisisgm/omarchy-pods](https://github.com/thisisgm/omarchy-pods)**:
   - Architecture reference for the Omarchy bar widget + headless background daemon + atomic state file design.
2. **[mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient)**:
   - Reverse-engineered protocol definitions and implementation reference for Sony MDR Bluetooth RFCOMM communication.
3. **[andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony)** by Roy Kevin De Jesus:
   - The upstream project this fork is based on (Omarchy plugin, daemon, CLI and test suite), MIT licensed.

---

## Disclaimer

This is an unofficial, independent community project developed for Linux desktop integration. It is not affiliated with, authorized, maintained, sponsored, or endorsed by Sony Corporation or any of its subsidiaries. "Sony", "WH-1000XM3", and related marks are registered trademarks of Sony Corporation.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
