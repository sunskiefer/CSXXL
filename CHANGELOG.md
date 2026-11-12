# Changelog

## 1.0.13
- Q-Links and on-screen faders now follow each other: turn a Q-Link and the fader of that page moves; touch a fader and its Q-Link name and value follow. Done per control through the wrapper's engine-changed report, no page redraw and nothing polled.
- Built against the current mpc-vst-plugins wrapper; momentary buttons keep their 1.0.12 tap behaviour (debounce 0).

## 1.0.12
First published release (the 1.0.11 build with catalog fields in its manifest: source repo and licence).

- Preset page rebuilt as a list picker: 8 named rows, tap a row then LOAD, PAGE UP/DOWN, -10/+10 pages, SAVE, INIT and a status line.
- Factory presets 1-640 converted from five bank files (120 controls matched and set from the bank; the remaining ~64 stay at CSXXL defaults), 641-768 written recipes. Room for up to 1500 presets.
- ARP GATE page: 8-step gate pattern per layer.
- 12 pages from an overview, Q-Links follow the visible page (two banks of 16), touch ribbon, pitch bar, per-key aftertouch, host-tempo arpeggiator, echo and LFO sync.
