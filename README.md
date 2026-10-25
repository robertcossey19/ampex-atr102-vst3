# Ampex (ANALOGEXACT) — ATR-102 physics model, JUCE plugin

ATR-102 record/reproduce chain modelled from the Ampex ATR-100 service manual and the tape makers' data sheets. Native C++ DSP, original HTML front panel in a WebView.
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
4. Default state: 456 tape, 15 ips, 250 nWb/m, NAB, transformer on, monitor **Repro** (tape path).
   Monitor **Input** is a hard bypass (unity, bit-exact). Hiss and wow/flutter are off until you turn them on.
5. Calibration: **0 VU = −18 dBFS** at Headroom 0 (Headroom shifts that reference). So a −18 dBFS sine is
   250 nWb/m on tape with the default flux switch; push 9–12 dB harder and you are at the 3 % point.

If Cubase ever refuses the plugin after a rebuild: Studio → VST Plug-in Manager → remove it from the
blocklist, rescan; if still stuck, delete `~/Library/Preferences/Cubase 15/Vst3 Cache` (Cubase's VST3
scan cache) and relaunch.

## What is modelled (v2 — physics rebuild)

Signal path per channel, levels in "0 VU = 1.0" units (0 VU = −18 dBFS + Headroom):

```
in → input xfmr T2 → record EQ (NAB/IEC/AES) → record HF pre-emphasis (1/L, aligned at operating bias)
   → 4× oversampled AC-bias anhysteretic remanence (per stock, bias-dependent)
   → wavelength losses L (bias zone, spacing, thickness, repro gap) → bias noise
   → reproduce EQ (exact inverse of the record EQ) → head contour → output xfmr T1 (8 dB NFB) → W&F → out
```

| Block | Model | Source |
|---|---|---|
| Operating levels | 0 VU = 370 nWb/m (456) / 260 nWb/m (406); 185/250/370/500 switch | ATR-100 manual 4890407-01 Table 1-4, Table 5-6 |
| Record / repro EQ | AES 17.5 µs @30 ips; NAB 50 + 3180 µs @15 and 7.5; IEC 35 µs @15, 70 µs @7.5. Record LF boost (1+sτ)/sτ floored at 12 Hz [EST], exact inverse on repro | ATR-100 Table 5-5, Section 5 theory |
| Bias alignment | over-bias set at a 1.5 mil wavelength (20 k / 10 k / 5 kHz): 2.75 dB (456), 1.75 dB (406); other stocks use the sheet ΔS10 (3.0 dB; SM900 4.0 dB). Record HF EQ is aligned at that bias and stays put when you move the BIAS knob | ATR-100 Table 5-11; RMG/EMTEC/Quantegy sheets |
| Tape | mean-field anhysteretic remanence `m = erf((h − κm)/√2)` with a log-normal switching-field distribution; under-bias → fewer particles switch; κ, drive and sensitivity calibrated per stock so THD at reference level and the 3 % MOL match the data sheet exactly (456: 0.1 % @260 nWb/m, 3 % @1072 nWb/m) | Mallinson, *Foundations of Magnetic Recording*; Bertram, *Theory of Magnetic Recording*; tape sheets below |
| Losses | Wallace spacing `e^(−2πd/λ)` (d = 0.3 µm, 1.0 µm in VIBE), thickness `(1−e^(−kδ))/kδ` with the sheet coating thickness, repro gap sinc (2.5 µm [EST]), record-zone loss as an equivalent spacing fitted to ΔS10 at 1.5 mil (speed-independent, which is exactly what the SM911 sheet's 1.5 / 3.0 / 6.0 dB at 30 / 15 / 7.5 ips implies) | Wallace 1951; SM911 sheet |
| Head contour | +0.6 dB bump at 60 Hz × (v/30 ips), −0.4 dB dip at 2.1×, 2nd-order HP at 0.3× [EST shape]; meets Table 1-4 (±2 dB at 35 Hz @30 ips, 20 Hz @15 ips) | ATR-100 Fig. 5-9, Table 1-4; ATR Service tech notes |
| Transformers | flux-domain core law `i_m = Φ/L·(1 + (Φ/Φs)²)`, implicit trapezoid solve. T2 (input, bridging 2×12.7 kΩ): 1 % THD @20 Hz at +16 dB re 0 VU, fL 3 Hz. T1 (output, tertiary NFB 8 dB): 1 % @20 Hz at +20 dB re 0 VU [EST], fL 2 Hz | ATR-100 I/O PWA schematic 4840421; Jensen JT-11P-1 data as proxy |
| Noise | bias noise shaped by the playback loss corner, level from Table 1-4 (2-track, unweighted, re 1040 nWb/m): 30 ips 72 dB, 15 NAB 69, 15 IEC 70, 7.5 NAB 71, 7.5 IEC 68; +4 dB full-track (mono headblock); per-stock offsets (406 +1, 499 −2, GP9 −2.5, SM900 −1.5 dB) | ATR-100 Table 1-4; tape sheets |
| Wow & flutter | 0.06 / 0.04 / 0.03 % rms (7.5 / 15 / 30 ips), reel wow 0.6 Hz + capstan components + scrape [EST spectrum]; ×1.5 in VIBE | ATR-100 Table 1-4 |
| Electronics | 28 kHz record LP (only active ≥ 88.2 kHz), clip 26 dB above operating level is not reached before tape saturation | ATR-100 Table 1-4 |
| Not modelled | crosstalk (spec < −65 dB — below the noise), print-through, erase residue, azimuth error | |

Items marked **[EST]** are engineering estimates not read directly off a document. Everything else is traceable to the sources below.

### Measured on the build (48 kHz, 456, 370 nWb/m, 15 ips NAB unless stated)

- Small-signal response: 15 ips within ±0.6 dB 20 Hz–20 kHz (spec ±0.75 dB 100 Hz–15 kHz, ±2 dB 20 Hz–20 kHz);
  30 ips within ±0.75 dB 50 Hz–20 kHz, −1.4 dB at 30 Hz; 7.5 ips −0.4 dB at 10 kHz, −1.6 dB at 15 kHz (spec ±2 dB to 15 kHz).
- 1 kHz THD vs level: −10 VU 0.02 %, 0 VU 0.21 %, +6 VU 1.0 %, +9.2 VU 3.0 % (data sheet: 0.2 % at 370, 3 % at 1072 nWb/m), +15 VU 20 %.
- 3 % points per stock, re 370 nWb/m: 406 +6.6 dB (sheet 810 nWb/m → +6.8), 456 +9.2 (+9.2), 499 +11.4 (+11.4), GP9 +11.6 (+11.5), SM900 +12.9 (+12.7), SM911 +11.2 (+11.2).
- HF saturation: 10 kHz at +9 VU compresses 3.9 dB at 7.5 ips vs 0.4 dB at 15 ips (SM911 sheet: SOL10 is 9.5 dB under MOL at 7.5 ips, 2 dB at 15 ips).
- BIAS knob: 10 kHz peaks +1.3 dB at −3 dB bias, −5 dB at +3, −13 dB at +5; 1 kHz −1.0 dB at −5 dB bias with THD 0.21 → 0.56 %.
- Hiss: −62.1 / −60.0 / −63.0 dB re 0 VU at 7.5 / 15 / 30 ips (spec-derived −62 / −60 / −63).
- Wow & flutter: 0.039 % rms measured on a 3 kHz tone (spec 0.04 %).
- Latency 4 samples (3 at 96 kHz). CPU: ~20× real-time stereo on a 2-core CI box.

### About the browser version's Jiles-Atherton core

The CodePen worklet runs a Jiles-Atherton hysteresis loop. It was ported and tested under simulated AC bias
first (`dsptest/anh.cpp`). Result: with a constant-amplitude bias JA produces an arcsin/duty-cycle transfer
that only saturates when the signal approaches the bias amplitude; with a decaying bias its remanence scales
with the bias amplitude and the decay length because JA has no switching-field distribution. Neither
reproduces measured tape behaviour (THD ∝ level², MOL, ΔS10, under-bias sensitivity loss), so the tape core
here is the anhysteretic-remanence model instead. The JA code is kept in `dsptest/` for reference.

### How this compares to the UA ATR-102

Both are built from the same manual and tape data, so the spec-level behaviour (EQ standards, levels,
MOL/THD per stock, speed-dependent HF saturation, noise, W&F) is in the same place. Things UA has that this
build does not: per-machine measurements of an actual unit (head-bump fine structure, exact transformer
cores), crosstalk, and a decade of listening tests against the hardware. Things this build does that are
documented and checkable: every number above is in `tests/HostTest/main.cpp` and `dsptest/main.cpp` and
fails the build if it drifts.

## Sources

- Ampex ATR-100 Series Operation and Maintenance Manual 4890407-01 (via AnalogRules archive)
- ATR Service technical reference — https://www.atrservice.com/technical-reference
- RMG bias chart (ATR-100 record gap 0.25 mil) — https://www.technicalaudio.com/pdf/RMG_tape/RMG_BiasChart.pdf
- Quantegy 456 data — https://www.zzounds.com/item--AMX456173111
- Quantegy 406/407 data sheet — https://www.vintageaudiocart.com/files/datasheets/quantegy_tape_406_407.pdf
- Quantegy 499 data sheet — https://mediapedia.nla.gov.au/pdfs/sound/IASA/quantegy_499_datasheet.pdf
- EMTEC/RMG SM911 data sheet — https://www.fullcompass.com/common/files/6763-SM911.pdf
- Jensen JT-11P-1 (input transformer proxy) — https://www.jensen-transformers.com/wp-content/uploads/2014/08/jt-11p-1.pdf
- Flux-loop equalizing amplifier notes — https://www.uneeda-audio.com/ampex/flea/flux_loop_equalizing_amplifier.pdf
- Chowdhury, "Real-time physical modelling for analog tape machines", DAFx 2019 (JA tape) — https://dafx.de/paper-archive/2019/DAFx2019_paper_3.pdf
- UA Ampex ATR-102 manual — https://help.uaudio.com/hc/en-us/articles/32491139365140-Ampex-ATR-102-Mastering-Tape-Recorder-Manual

## Earlier fixes (v1, kept)

Editor: null-safe parameter polling, correct speed/flux index mapping, automation gestures. Plugin: state
save/restore, stereo bus layout, hard-bypass Input monitor, latency reporting.

Not changed (your call later): PLUGIN_MANUFACTURER_CODE `MYPL` / PLUGIN_CODE `Mypl` / bundle id
`com.ampex.plugin` are generic placeholders and "Ampex" is a live trademark; fine for private use, change
before you ship anything.

## Build on Linux (for testing only)

`./build-linux.sh` → `build/Ampex_artefacts/Release/VST3/Ampex.vst3` → copy to `~/.vst3/`.
