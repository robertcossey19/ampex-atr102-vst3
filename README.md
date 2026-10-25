# Ampex (ANALOGEXACT) — JUCE plugin, fixed build

ATR-102-style tape emulation. Native C++ DSP, original HTML front panel in a WebView.
Formats: **VST3** (what Cubase loads), AU (Logic), Standalone app.

## Straight talk about formats and signing

- **Cubase 15 on an Apple Silicon Mac = VST3 only.** In native (non-Rosetta) mode Cubase 14/15 cannot
  load VST2 at all, and Steinberg stopped issuing the VST2 SDK license in 2018, so a "VST2 build" is not a
  thing anyone can legally produce today. This project builds VST3; forget VST2.
- **Signing.** Apple Silicon will not load unsigned code. The build here is **ad-hoc signed** by default
  (`codesign -s -`), which is all a plugin you compile on your own Mac needs. A paid Apple Developer ID
  plus notarization is only required if you want to hand the `.vst3` to other people without them
  having to clear Gatekeeper. Hooks for that are in `build-mac.sh` and the GitHub workflow (env vars /
  repo secrets); nothing to do if it's just for your machine.
- A Mac `.vst3` can only be compiled on a Mac (or Apple's cloud runners). The two ways to get one are below.

## Option A — build on your Mac (one command)

Needs Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).

```bash
cd Ampex_Build
chmod +x build-mac.sh
./build-mac.sh
```

Native-only (faster, M-series only): `AMPEX_ARCHS=arm64 ./build-mac.sh`

First run downloads JUCE 8.0.4 (~100 MB) and compiles; expect 5–10 min on an M4 Max.
The script configures, builds Release, ad-hoc signs, verifies the signature, strips quarantine,
and copies to:

- `~/Library/Audio/Plug-Ins/VST3/Ampex.vst3`  ← Cubase
- `~/Library/Audio/Plug-Ins/Components/Ampex.component`  ← Logic/GarageBand (Cubase ignores AU)
- `build/Ampex_artefacts/Release/Standalone/Ampex.app`

## Option B — let GitHub build it (no Xcode on your Mac)

1. Create a new GitHub repo (public = free Actions minutes), push this folder to it.
2. Actions tab → "Build Ampex VST3 (macOS universal)" → Run workflow (it also runs on every push).
3. When it goes green, open the run and download the **Ampex-macOS-VST3** artifact.
4. Unzip, then install:

```bash
cd ~/Downloads
unzip Ampex.vst3.zip
xattr -dr com.apple.quarantine Ampex.vst3
mkdir -p ~/Library/Audio/Plug-Ins/VST3
rm -rf ~/Library/Audio/Plug-Ins/VST3/Ampex.vst3
mv Ampex.vst3 ~/Library/Audio/Plug-Ins/VST3/
```

(or `./install-mac.sh ~/Downloads/Ampex.vst3`, which does the same plus a signature check.)

## Loading it in Cubase 15 Pro

1. Quit and relaunch Cubase, or **Studio → VST Plug-in Manager → Rescan All** (the circular-arrow button).
2. In the manager, search "Ampex". It should show under **VST3 Plug-ins** with a green/valid status.
   If it's in the **Blocklist** tab, select it and click **Reactivate**, then rescan.
3. On an audio track: Inserts → **Distortion → Ampex** (or type "Ampex" in the plugin selector search).
4. Default state: 456 tape, 15 ips, 250 nWb/m, NAB, transformer on, monitor Input. Switch Monitor to
   **Repro** to hear the tape path; hiss and wow/flutter are off until you turn them on.

If Cubase ever refuses the plugin after a rebuild: Studio → VST Plug-in Manager → remove it from the
blocklist, rescan; if still stuck, delete `~/Library/Preferences/Cubase 15/Vst3 Cache` (Cubase's VST3
scan cache) and relaunch.

## What was fixed vs. the generated files

Editor (would have crashed Cubase the moment the GUI opened):

- `timerCallback` polled parameter IDs that don't exist in the APVTS (`bias`, `btnexport`, `power`,
  `calnow`, `toggletech`, `eqlegendtoggle`, `autocal`, `tapehiss`, `wowflutter`, `eq`, `tape`).
  `getRawParameterValue` returns `nullptr` for those and the code dereferenced it. Replaced with a loop
  over the real 17 IDs with null checks.
- Speed and flux switches: the web UI sends raw 7.5/15/30 and 185/250/370/500; the host wanted a 0-based
  choice index and clamped anything ≥ 2 (or ≥ 3) to the top choice, so 7.5 ips selected 30 ips and 185 nWb/m
  selected 500. Now mapped properly.
- Change gestures added so Cubase automation write/undo sees knob moves.

DSP (same six fixes as the CodePen patch, plus two port-specific ones):

- Tape and bias saturators: continuous asymmetric curve `tanh(k·(x + ½·asym·x²))` instead of a different
  slope on each side of zero (that split caused a fixed ~5 % THD at any level, i.e. crossover distortion).
- DC blocker (R = 0.9995) after the bias stage, since even-order shaping generates DC.
- Hiss gain no longer clamped by the −60 dB floor; rescaled so 456 / 250 nWb/m / 15 ips sits near
  −83 dBFS and actually tracks tape, speed and flux.
- Wow/flutter delay amplitude = depth / (2π·rate). Previously ~1000× too shallow to hear.
- **Bias knob** now triggers the filter/asymmetry update. It was missing from the change check, so turning
  BIAS did nothing until you changed tape type.
- Removed the hidden −5 dB input pad before the saturator.
- All filter, delay and DC-blocker state is reset in `prepareToPlay`.

Not changed (your call later): PLUGIN_MANUFACTURER_CODE `MYPL` / PLUGIN_CODE `Mypl` / bundle id
`com.ampex.plugin` are generic placeholders and "Ampex" is a live trademark; fine for private use, change
before you ship anything. VIBE mode in the C++ port is a parameter tweak, not the Jiles-Atherton hysteresis
model from the browser version, and Preserve is a plain wet/dry blend, not a match-EQ.

## Build on Linux (for testing only)

`./build-linux.sh` → `build/Ampex_artefacts/Release/VST3/Ampex.vst3` → copy to `~/.vst3/`.
