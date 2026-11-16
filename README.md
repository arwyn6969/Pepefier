# Pepefier

A procedural frog-croak instrument for Akai MPC standalone hardware, by Pepe Labs.

Pepefier models frog vocalisation — shaped glottal pulses through dual high-Q
formants — and adds Doppler, stereo chorus, depth filtering and a pond bed with
bubbles. Six modes (Bullfrog, Ribbit, Peeper, Trill, Honk, Pepe Special) plus
nine factory presets. The touch UI has three pages: Croak, Move and Pond.

Requires **MPC OS 3.10.1 or later**, with External Plugin Support enabled.

## Install on an MPC

1. Grab a release zip (or build one as below). Unzip it.
2. Copy the folder `Pepe Labs - VST - Pepefier` into a folder named exactly
   `VST2 Plugins` at the root of a USB drive, SD card, or the MPC's internal
   drive.
3. On the MPC: Preferences → Enhancements (Beta) → turn on **Enable
   Enhancements** and **External Plugin Support**.
4. Rescan / reinsert the drive if needed. Pepefier appears under External
   Plugins as an instrument.

A full parameter walkthrough is in [docs/USER_MANUAL.md](docs/USER_MANUAL.md).

## Build from source

This repo is an MPC Plugin SDK *project*, not the SDK itself. Clone the official
SDK and place (or symlink) this tree at `plugins/Pepefier`:

```bash
git clone https://github.com/inmusic-software/mpc-plugin-builder-sdk.git
cd mpc-plugin-builder-sdk
ln -s /path/to/Pepefier plugins/Pepefier

python3 tools/build.py plugins/Pepefier
python3 tools/package.py plugins/Pepefier
```

The first build downloads the Zig cross-compiler (about 50 MB). You can also
pass `--toolchain gcc` if you have `g++-arm-linux-gnueabihf` and
`g++-aarch64-linux-gnu`.

Useful targets:

- default (`all`) — native test build + 32-bit and 64-bit ARM `.so` files, plus
  a deployable `build/Pepe Labs - VST - Pepefier/` folder
- `--target desktop` — a generic VST2 for this computer (`build/desktop/Pepefier/`,
  copy into `~/.vst` or your host's VST2 folder; no MPC skin)

`package.py` writes `dist/pepefier-1.0.0-mpc.zip`.

## Project layout

- `plugin.json` — manifest (name, manufacturer, uid). The `uid` must match the
  C++ `uniqueId` (`PAI7`).
- `src/Pepefier.cpp` — parameters, MIDI, DSP.
- `ui.json` — page/control layout used if you regenerate skins.
- `skins/` — hand-written `TUI.json`, `Q-Links.json` and kit art (copied as-is
  at build time).
- `docs/` — user manual and screenshots.

## Licence

MIT. See [LICENSE](LICENSE).
