# Pepefier

An instrument plugin for Akai MPC standalone hardware, by Pepe Labs.

_Describe what the plugin does here. This file is included in the distributable zip._

## Build

```
python tools/build.py plugins/Pepefier
python tools/package.py plugins/Pepefier
```

## Project layout

- `plugin.json` - manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/Pepefier.cpp` - the plugin source (parameters, MIDI handling, DSP).
- `ui.json` - which parameters appear on the MPC screen, in which order.
- `build/` - build output (the deployable `Pepe Labs - VST - Pepefier` folder lives here).
- `dist/` - zips produced by `package.py`.
