/*
  TapeDSP.h — physics-based ATR-100 series (ATR-102) record/reproduce chain.

  Every number that comes from a document is tagged with its source:
    [ATR]  Ampex ATR-100 Series Operation & Maintenance manual 4890407-01
           (Table 1-4 specifications, Section 4 theory, Section 5 alignment,
            schematic 4840421 input/output PWA).
    [456]  Quantegy 456 data sheet.          [406] Quantegy 406/407 data sheet.
    [499]  Quantegy 499 data sheet.          [GP9] Quantegy GP9 data sheet.
    [911]  RMGI/EMTEC SM911 data sheet.      [900] RMGI/EMTEC SM900 data sheet.
    [RMG]  RMG bias chart (ATR-100 = 0.25 mil record gap column).
    [BER]  Bertram, Theory of Magnetic Recording (anhysteretic remanence, mean-field).
    [MAL]  Mallinson, Foundations of Magnetic Recording (AC-bias, SFD, interactions).
    [WAL]  Wallace 1951 spacing law; standard gap / thickness loss terms.
    [JEN]  Jensen JT-11P-1 data (used as a *proxy* for the bridging input
           transformer core law; the Ampex part number is not legible in the
           manual scan).  Flagged as estimate where used.
    [EST]  Engineering estimate — flagged, not from a document.
*/
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>
#include <functional>
#include <algorithm>
#include <memory>
#include <atomic>

namespace ampex
{
static constexpr double kPi = 3.14159265358979323846;
static constexpr double kMu0 = 1.25663706212e-6;
inline double dB2lin (double db) { return std::pow (10.0, db / 20.0); }
inline double lin2dB (double g)  { return 20.0 * std::log10 (std::max (g, 1e-30)); }

// ───────────────────────────── tape stock table ─────────────────────────────
struct TapeSpec
{
    const char* name;
    double Hc_Am;        // coercivity A/m           (data sheet Oe × 79.577)
    double Br_T;         // retentivity tesla        (data sheet Gauss × 1e-4)
    double coating_um;   // oxide coating thickness  (data sheet)
    double refFlux;      // nWb/m reference level used by the data sheet
    double thdAtRefPct;  // 3rd harmonic at refFlux, 1 kHz, 15 ips
    double molFlux;      // nWb/m at 3% 3rd harmonic, 1 kHz, 15 ips
    double dS10_dB;      // recommended over-bias: 10 kHz drop from peak, 15 ips
    double noiseRel456;  // bias-noise relative to 456 (dB, + = noisier)
};

// Values: 456 [456] 320 Oe/1500 Gs sheet (zzounds listing says 330/1460 — older rev),
// coating 500 µin = 12.7 µm, THD 0.1 % @ 260 nWb/m, MOL +12.3 dB re 260 → 1072 nWb/m.
// 406 [406] 330 Oe, 1300 Gs, coating 10.1 µm; MOL/THD not legible in the scan → [EST]
//     from retentivity ratio vs 456 (−1.0 dB) and Ampex "406/407 operating level 260".
// 499 [499] 390 Oe, 1600 Gs, coating 15.2 µm, THD 0.20 % at (unstated) ref, MOL 17.4 dB
//     re 185 nWb/m → 1370 nWb/m [EST: reference assumed 185].
// GP9 [GP9] 350 Oe, THD 0.06 %, MOL 17.5 dB re 185 → 1387 nWb/m, coating [EST] 16 µm.
// SM900 [900] 370 Oe, 1540 Gs, MOL 14.0 dB re 320 → 1604, THD −58.5 dB (0.12 %), ΔS10 4.0 [RMG].
// SM911 [911] 320 Oe, 1450 Gs, MOL 12.5 dB re 320 → 1350, THD 0.10 %, ΔS10 3.0, BN −60.5 A.
// kappa (knee), drive and sens live in TapeCalibration.inc, produced by dsptest/calib.cpp.
inline const std::array<TapeSpec, 6>& tapeTable()
{
    static const std::array<TapeSpec, 6> t = {{
        //  name   Hc(A/m)  Br(T)  coat  ref  thd%  MOL    dS10 noise
        { "406",  26260.0, 0.130, 10.1, 260, 0.20,  810.0, 1.75, +1.0 },   // dS10 1.75 dB @1.5 mil [ATR Table 5-11]
        { "456",  25465.0, 0.150, 12.7, 260, 0.10, 1072.0, 2.75,  0.0 },   // dS10 2.75 dB @1.5 mil [ATR Table 5-11]
        { "499",  31035.0, 0.160, 15.2, 260, 0.06, 1370.0, 3.0, -2.0 },
        { "GP9",  27850.0, 0.165, 16.0, 260, 0.06, 1387.0, 3.0, -2.5 },
        { "SM900",29443.0, 0.154, 14.0, 320, 0.12, 1604.0, 4.0, -1.5 },
        { "SM911",25465.0, 0.145, 13.0, 320, 0.10, 1350.0, 3.0,  0.0 },
    }};
    return t;
}

// Per-tape calibration overrides (filled in by the calibration run; kept separate so the
// physical table above stays readable).  index = tape index.  {kappa (knee), drive, sens}
struct TapeCal { double kappa, drive, sens; };
inline const std::array<TapeCal, 6>& tapeCal()
{
    static const std::array<TapeCal, 6> c = {{
        #include "TapeCalibration.inc"
    }};
    return c;
}

// ───────────────────────────── machine constants ─────────────────────────────
struct Speed { double ips, v; };                       // v in m/s
inline Speed speedOf (int idx) { static const double ips[3] = { 7.5, 15.0, 30.0 }; return { ips[idx], ips[idx] * 0.0254 }; }

struct EqStd { double tauHF_us, tauLF_us; const char* name; };
// [ATR] Table 5-5 / Table 5-8:  AES 17.5/∞ (30), NAB 50/3180 (15, 7.5), IEC 35/∞ (15), IEC 70/∞ (7.5)
inline EqStd eqFor (int speedIdx, int eqIdx)
{
    if (speedIdx == 2) return { 17.5, 0.0, "AES" };
    if (eqIdx == 0)    return { 50.0, 3180.0, "NAB" };
    return { speedIdx == 1 ? 35.0 : 70.0, 0.0, "IEC" };
}

// Record gap 0.25 mil [RMG]; reproduce gap [EST] 0.1 mil (100 µin) — Ampex metal 2-track.
static constexpr double kRecordGap_um = 6.35;
static constexpr double kReproGap_um  = 2.5;
static constexpr double kSpacingNominal_um = 0.30;  // [EST] clean head, Wallace d
static constexpr double kSpacingWorn_um    = 1.00;  // [EST] VIBE/worn
static constexpr double kBiasFreqHz = 432000.0;     // [ATR] (informational; see JA stage)

// ───────────────────────────── loss physics [WAL] ─────────────────────────────
struct LossModel
{
    double v = 0.381;           // m/s
    double spacing_um = kSpacingNominal_um;
    double coating_um = 12.7;
    double reproGap_um = kReproGap_um;
    double biasZone_um = 2.09;  // record-zone loss as equivalent spacing: dS10·λ10/54.6

    static double sinc (double x) { return std::abs (x) < 1e-9 ? 1.0 : std::sin (x) / x; }
    double lambda_um (double f) const { return v / std::max (f, 1e-3) * 1e6; }

    // small-signal playback-only loss (noise sees this)
    double playback (double f) const
    {
        const double lam = lambda_um (f);
        const double k = 2.0 * kPi / lam;
        const double sp = std::exp (-k * spacing_um);
        const double x = k * coating_um;
        const double th = (1.0 - std::exp (-x)) / x;
        const double gp = std::abs (sinc (kPi * reproGap_um / lam));
        return sp * th * gp;
    }
    // record-zone (bias) loss — Wallace-type exponential, calibrated to the data-sheet ΔS10
    double recordZone (double f) const { return std::exp (-2.0 * kPi * biasZone_um / lambda_um (f)); }
    double total (double f) const { return playback (f) * recordZone (f); }
};

// Reproduce standard transfer (flux → output), relative to 1 kHz = 1.
inline double reproStdMag (double f, const EqStd& e)
{
    const double w = 2.0 * kPi * f;
    double m = std::sqrt (1.0 + std::pow (w * e.tauHF_us * 1e-6, 2.0));
    if (e.tauLF_us > 0.0) { const double a = w * e.tauLF_us * 1e-6; m *= a / std::sqrt (1.0 + a * a); }
    const double w1 = 2.0 * kPi * 1000.0;
    double m1 = std::sqrt (1.0 + std::pow (w1 * e.tauHF_us * 1e-6, 2.0));
    if (e.tauLF_us > 0.0) { const double a = w1 * e.tauLF_us * 1e-6; m1 *= a / std::sqrt (1.0 + a * a); }
    return m / m1;
}

// ───────────────────────────── minimum-phase FIR designer ─────────────────────────────
// Real-cepstrum method: log|H| → cepstrum → fold → exp → impulse response.
struct MinPhaseFIR
{
    static std::vector<float> design (const std::function<double (double)>& mag, double fs, int taps, int order = 13)
    {
        const int N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<std::complex<float>> buf (N), tmp (N);
        for (int k = 0; k < N; ++k)
        {
            const int kk = k <= N / 2 ? k : N - k;
            const double f = (double) kk * fs / N;
            buf[k] = { (float) std::log (std::max (mag (f), 1e-5)), 0.0f };
        }
        fft.perform (buf.data(), tmp.data(), true);               // cepstrum (inverse, scaled)
        for (int n = 0; n < N; ++n)
        {
            float w = 0.0f;
            if (n == 0 || n == N / 2) w = 1.0f; else if (n < N / 2) w = 2.0f;
            tmp[n] = { tmp[n].real() * w, 0.0f };
        }
        fft.perform (tmp.data(), buf.data(), false);
        for (int k = 0; k < N; ++k) buf[k] = std::exp (buf[k]);
        fft.perform (buf.data(), tmp.data(), true);
        std::vector<float> h (taps);
        const int fade = taps / 4;
        for (int n = 0; n < taps; ++n)
        {
            double w = 1.0;
            if (n >= taps - fade) { const double t = (double) (n - (taps - fade)) / fade; w = 0.5 + 0.5 * std::cos (kPi * t); }
            h[n] = (float) (tmp[n].real() * w);
        }
        return h;
    }
};

struct FIRState
{
    std::vector<float> h, z; int pos = 0;
    void set (std::vector<float> taps) { h = std::move (taps); z.assign (h.size() * 2, 0.0f); pos = 0; }
    void reset() { std::fill (z.begin(), z.end(), 0.0f); pos = 0; }
    inline float process (float x)
    {
        const int n = (int) h.size(); if (n == 0) return x;
        z[pos] = x; z[pos + n] = x;
        const float* zz = &z[pos + 1]; const float* hh = h.data();
        float acc = 0.0f;
        for (int i = 0; i < n; ++i) acc += hh[n - 1 - i] * zz[i];
        if (++pos >= n) pos = 0;
        return acc;
    }
    double magnitudeAt (double f, double fs) const
    {
        std::complex<double> a = 0; for (size_t n = 0; n < h.size(); ++n) a += (double) h[n] * std::exp (std::complex<double> (0, -2 * kPi * f * n / fs));
        return std::abs (a);
    }
};

// ───────────────────────────── small IIR helpers (double, TDF2) ─────────────────────────────
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
    void reset() { s1 = s2 = 0; }
    inline double process (double x) { const double y = b0 * x + s1; s1 = b1 * x - a1 * y + s2; s2 = b2 * x - a2 * y; return y; }
    void identity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    // bilinear from analog (B0 + B1 s + B2 s²)/(A0 + A1 s + A2 s²), s normalised by prewarp at fc
    void fromAnalog (double B0, double B1, double B2, double A0, double A1, double A2, double fs, double fcWarp)
    {
        const double K = fcWarp > 0 ? (2 * kPi * fcWarp) / std::tan (kPi * fcWarp / fs) : 2 * fs;
        const double k2 = K * K;
        const double a0 = A0 + A1 * K + A2 * k2;
        b0 = (B0 + B1 * K + B2 * k2) / a0; b1 = (2 * B0 - 2 * B2 * k2) / a0; b2 = (B0 - B1 * K + B2 * k2) / a0;
        a1 = (2 * A0 - 2 * A2 * k2) / a0;  a2 = (A0 - A1 * K + A2 * k2) / a0;
    }
    void lowpass (double fc, double Q, double fs) { const double w = 2 * kPi * fc; fromAnalog (w * w, 0, 0, w * w, w / Q, 1, fs, fc); }
    void highpass (double fc, double Q, double fs) { const double w = 2 * kPi * fc; fromAnalog (0, 0, 1, w * w, w / Q, 1, fs, fc); }
    void peak (double fc, double Q, double gDb, double fs)
    {
        const double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * fc;
        fromAnalog (w * w, w * A / Q, 1, w * w, w / (A * Q), 1, fs, fc);
    }
    // first-order (1 + s τz)/(1 + s τp)
    void shelf1 (double tauZ, double tauP, double fs, double gain = 1.0, double fwarp = 0.0)
    {
        // prewarp at the lowest in-band corner unless told otherwise (a corner above Nyquist would warp the whole band)
        const double fw = fwarp > 0 ? fwarp : 1.0 / (2 * kPi * std::max (std::max (tauZ > 0 ? tauZ : 1e9, tauP > 0 ? tauP : 1e9), 1e-7));
        fromAnalog (gain, gain * tauZ, 0, 1, tauP, 0, fs, std::min (fw, fs * 0.45));
    }
    double magnitudeAt (double f, double fs) const
    {
        const std::complex<double> z = std::exp (std::complex<double> (0, -2 * kPi * f / fs));
        return std::abs ((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z));
    }
};

// ───────────────────────────── AC-bias anhysteretic remanence [BER][MAL] ─────────────────────────────
// Bias recording leaves each tape element in its *anhysteretic remanence* state.  For an ensemble of
// single-domain particles with a switching-field distribution (SFD) and a mean interaction field,
// the anhysteretic remanence curve is (Mallinson, Foundations of Magnetic Recording; Bertram,
// Theory of Magnetic Recording):
//        m = erf[(h − κ·m)/√2],   m = M/M_r∞,  h = H_sig/σ_i,  κ = N_i·M_r∞/σ_i
// σ_i = interaction-field spread (sets the scale → absorbed into `drive`), κ = mean-field
// linearisation (sets how linear the curve is before it bends — calibrated to the data-sheet THD
// at reference level and the 3 % MOL point).  Under-bias: only particles whose switching field is
// below the bias amplitude take part → M_r∞ and κ scale with CDF_SFD(H_b).  (log-normal SFD.)
// We tried porting the browser's Jiles-Atherton core first; under a decaying AC bias it has no
// switching-field distribution, so its remanence scaled with bias amplitude instead of converging —
// see dsptest/anh.cpp.  The JA core is therefore not used for the tape.
struct AnhTable
{
    double Hmax = 1.0; int N = 0; std::vector<float> m;   // m[i] = M_r(Hmax·i/(N−1)), odd-symmetric
    inline double lookup (double H) const
    {
        if (N < 2) return 0.0;
        const double s = H < 0 ? -1.0 : 1.0; double x = std::abs (H) / Hmax * (N - 1);
        if (x >= N - 1) return s * m[(size_t) N - 1];
        const int i = (int) x; const double f = x - i;
        return s * ((1.0 - f) * m[(size_t) i] + f * m[(size_t) i + 1]);
    }
};

struct AnhParams
{
    double Mr = 1.0;      // A/m, saturation remanence of the fully participating ensemble
    double sigma = 1.0;   // A/m, interaction-field spread
    double kappa = 3.0;   // mean-field linearisation
    double Hc = 1.0;      // A/m, median switching field
    double sfd = 0.35;    // log-normal SFD width (ln units)
    double Hb = 2.0;      // A/m, bias field amplitude
    double participating() const    // CDF of the log-normal SFD at the bias amplitude
    { const double z = std::log (std::max (Hb, 1e-9) / Hc) / sfd; return 0.5 * (1.0 + std::erf (z / std::sqrt (2.0))); }
};

inline double anhSolve (double h, double kappa)
{
    // m − erf((h − κ m)/√2) is increasing in m → bisection on [−1, 1]
    double lo = -1.0, hi = 1.0;
    for (int i = 0; i < 48; ++i) { const double mid = 0.5 * (lo + hi); const double f = mid - std::erf ((h - kappa * mid) / std::sqrt (2.0)); if (f > 0) hi = mid; else lo = mid; }
    return 0.5 * (lo + hi);
}

inline AnhTable buildAnhTable (const AnhParams& p, double HmaxSigma = 24.0, int N = 769)
{
    AnhTable t; t.N = N; t.Hmax = HmaxSigma * p.sigma; t.m.resize ((size_t) N);
    const double part = p.participating();
    const double MrEff = p.Mr * part, kEff = p.kappa * part;
    for (int i = 0; i < N; ++i) t.m[(size_t) i] = (float) (MrEff * anhSolve ((t.Hmax * i / (N - 1)) / p.sigma, kEff));
    t.m[0] = 0.0f;
    return t;
}

// ───────────────────────────── transformer (flux-domain core law) ─────────────────────────────
// Primary magnetising current i_m(Φ) = Φ/L · (1 + κ (Φ/Φs)²) + hysteresis term; v_out = v_in − Rs·i_m.
// Φ is the integral of v_out.  Distortion ∝ Rs, ∝ (V/f)², i.e. the bass goes first [JEN][ATR].
struct Transformer
{
    double fs = 48000;
    double fL = 8.0;          // LF −3 dB corner set by Rs/Lp
    double phiSat = 1.0;      // flux (volt-seconds, normalised) at which THD = 1 % at 20 Hz
    double kappa = 1.0;
    double hyst = 0.0;        // coercive term (steel-ish cores) — 0 for nickel
    double fb = 1.0;          // 1/(1+NFB) distortion reduction (output xfmr tertiary loop) [ATR]
    double phi = 0, prevOut = 0;
    Biquad hf;                // leakage-L / winding-C resonance, well above band

    void prepare (double sampleRate, double lfCorner, double level1pct20Hz, double nfbDb, double hfRes, double hfQ)
    {
        fs = sampleRate; fL = lfCorner; fb = 1.0 / dB2lin (nfbDb);
        // flux amplitude for a sine of amplitude A at f:  Φ = A / (2π f)
        // 3rd-harmonic ratio of the Rs·i_m drop ≈ (wL_rel) · κ (Φ/Φs)² / 4 ... we normalise so that
        // THD = 1 % at `level1pct20Hz` (linear amplitude) at 20 Hz including the LF divider factor.
        const double A = level1pct20Hz, f = 20.0;
        const double phiA = A / (2 * kPi * f);
        const double lfDiv = fL / f;              // Rs/(ωL) at 20 Hz
        // THD3 = lfDiv * kappa * (phiA/phiSat)^2 / 4 * fb = 0.01  → choose phiSat with kappa=1
        kappa = 1.0;
        phiSat = phiA / std::sqrt (0.01 * 4.0 / std::max (lfDiv * kappa * fb, 1e-9));
        if (hfRes < 0.45 * fs) hf.lowpass (hfRes, hfQ, fs); else hf.identity();   // leakage resonance only matters at high fs
        reset();
    }
    void reset() { phi = 0; prevOut = 0; hf.reset(); }
    inline double process (double vin)
    {
        // v_out = v_in − a·g(φ),  dφ/dt = v_out,  g(φ) = φ(1 + κ(φ/φs)²)   (a = ωL·fb, Rs/L = 2π fL)
        // Solved implicitly (trapezoid + Newton) — the explicit form is unstable once φ ≫ φs.
        const double a = (2 * kPi * fL) * fb, h = 1.0 / fs;
        const double rhs = phi + 0.5 * h * (vin + prevOut);
        double pn = phi;
        for (int it = 0; it < 4; ++it)
        {
            const double r = pn / phiSat, r2 = r * r;
            const double g = pn * (1.0 + kappa * r2), dg = 1.0 + 3.0 * kappa * r2;
            const double F = pn - rhs + 0.5 * h * a * g, dF = 1.0 + 0.5 * h * a * dg;
            pn -= F / dF;
        }
        const double r = pn / phiSat;
        double vout = vin - a * pn * (1.0 + kappa * r * r);
        if (hyst > 0) vout -= a * hyst * phiSat * std::tanh (prevOut * 50.0);
        phi = pn; prevOut = vout;
        return hf.process (vout);
    }
};

// ───────────────────────────── configuration ─────────────────────────────
struct Config
{
    int tape = 1, speed = 1, eq = 0, flux = 1;
    double biasDb = 0.0;      // bias current relative to recommended, dB
    bool transformer = true, vibe = false, monoHeadblock = false;
    double fluxAt0VU() const { static const double f[4] = { 185, 250, 370, 500 }; return f[flux]; }
    bool operator== (const Config& o) const
    { return tape == o.tape && speed == o.speed && eq == o.eq && flux == o.flux && std::abs (biasDb - o.biasDb) < 1e-6
          && transformer == o.transformer && vibe == o.vibe && monoHeadblock == o.monoHeadblock; }
};

// ───────────────────────────── one channel of the machine ─────────────────────────────
// Shared ensemble constants: σ_i = 0.5·Hc (scale only; `drive` absorbs it), SFD width per class [EST]
// (oxide 456-class 0.35, high-energy/metal-class 0.25), recommended bias = Hc·exp(2·sfd) → ≈97 %
// of the particles switch at the recommended setting; the bias knob scales H_b in dB of current.
static constexpr double kSigmaMul = 0.5;
inline double sfdFor (int tape) { return (tape == 0 || tape == 1 || tape == 5) ? 0.35 : 0.25; }
inline AnhParams anhFor (const TapeSpec& ts, int tapeIdx, double kappa, double biasDb)
{
    AnhParams p; p.Mr = ts.Br_T / kMu0; p.Hc = ts.Hc_Am; p.sigma = kSigmaMul * ts.Hc_Am; p.kappa = kappa;
    p.sfd = sfdFor (tapeIdx); p.Hb = ts.Hc_Am * std::exp (2.0 * p.sfd) * dB2lin (biasDb); return p;
}

class TapeChannel
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        fs = sampleRate;
        osFactor = sampleRate <= 50000 ? 4 : 2;
        const int order = osFactor == 4 ? 2 : 1;
        os = std::make_unique<juce::dsp::Oversampling<float>> (1, order, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
        os->initProcessing ((size_t) maxBlock);
        fsOS = fs * osFactor;
        if (fs >= 88000.0) recHF.lowpass (28000.0, 0.707, fs); else recHF.identity();   // [ATR] electronics flat to 28 kHz
        configure (cfg, true);
        reset();
    }
    int latencySamples() const { return os ? (int) std::lround (os->getLatencyInSamples()) : 0; }
    void reset()
    {
        if (os) os->reset();
        pre.reset(); post.reset();
        recLF.reset(); recHFpole.reset(); recHF.reset(); repLF.reset(); repHF.reset(); bump1.reset(); bump2.reset(); bumpHP.reset();
        inX.reset(); outX.reset(); noiseLP.reset();
    }

    void configure (const Config& c, bool force = false)
    {
        if (!force && c == cfg) return;
        cfg = c;
        const TapeSpec& ts = tapeTable()[(size_t) c.tape];
        const TapeCal& tc = tapeCal()[(size_t) c.tape];
        const Speed sp = speedOf (c.speed);
        const EqStd eq = eqFor (c.speed, c.eq);
        // ---- anhysteretic remanence table ----
        table = buildAnhTable (anhFor (ts, c.tape, tc.kappa, c.biasDb - (c.vibe ? 1.0 : 0.0)));
        drive = tc.drive * c.fluxAt0VU();         // A/m per unit (1.0 = 0 VU)
        outScale = tc.sens / c.fluxAt0VU();
        // ---- losses ----
        loss.v = sp.v; loss.coating_um = ts.coating_um; loss.reproGap_um = kReproGap_um;
        loss.spacing_um = c.vibe ? kSpacingWorn_um : kSpacingNominal_um;
        // record-zone equivalent spacing: data-sheet ΔS10 at 15 ips (λ = 38.1 µm) scaled by bias current
        // [ATR Table 5-11] over-bias is set at a 1.5 mil wavelength at every speed (20k/10k/5k Hz) → one zone depth per stock.
        // [EST] zone depth ∝ (I_bias/I_0)^3 so the 10 kHz-vs-bias curve peaks ≈3 dB of current under operating bias.
        const double biasZoneNominal = ts.dS10_dB * 38.1 / 54.6;
        loss.biasZone_um = biasZoneNominal * std::pow (dB2lin (c.biasDb), 3.0);
        // ---- record side: standard EQ (IIR) × inverse losses (min-phase FIR, capped) ----
        const double tauFloor = 1.0 / (2 * kPi * 12.0);                                     // [EST] record-amp LF boost bottoms out ≈ +12 dB
        if (eq.tauLF_us > 0) recLF.shelf1 (eq.tauLF_us * 1e-6, tauFloor, fs, tauFloor / (eq.tauLF_us * 1e-6));   // ≈ (1+sτ)/(sτ)
        else recLF.identity();
        const double fHF = 1.0 / (2 * kPi * eq.tauHF_us * 1e-6);
        recHFpole.shelf1 (0.0, eq.tauHF_us * 1e-6, fs, 1.0, fHF);                            // 1/(1+sτ_hf)
        // record HF EQ is aligned at operating bias (Table 5-11) and left alone when the bias knob moves
        LossModel aligned = loss; aligned.biasZone_um = biasZoneNominal; aligned.spacing_um = kSpacingNominal_um;   // VIBE = worn machine, aligned when new
        const double capLin = dB2lin (30.0);               // [ATR Table 1-4] flat to 15 kHz @7.5 ips needs ≈26 dB at the record head
        auto preMag = [aligned, capLin] (double f) {
            const double L = aligned.total (std::max (f, 1.0));
            return 1.0 / std::sqrt (L * L + 1.0 / (capLin * capLin));
        };
        pre.set (MinPhaseFIR::design (preMag, fs, 192));
        post.set (MinPhaseFIR::design ([this] (double f) { return loss.total (std::max (f, 1.0)); }, fs, 192));
        // ---- reproduce side: standard EQ ----
        repHF.shelf1 (eq.tauHF_us * 1e-6, 1.0 / (2 * kPi * 150000.0), fs, 1.0, fHF);         // (1+sτ_hf), pole far out of band; same prewarp → exact inverse in-band
        if (eq.tauLF_us > 0) repLF.shelf1 (tauFloor, eq.tauLF_us * 1e-6, fs, (eq.tauLF_us * 1e-6) / tauFloor);   // ≈ sτ/(1+sτ), exact inverse of recLF
        else repLF.identity();
        // alignment: REC GAIN set so a 1 kHz 0 VU tone lands at exactly <flux> nWb/m on tape [ATR 5-46 step 33]
        recTrim = 1.0 / std::max (smallSignalPreGain (1000.0), 1e-6);
        repTrim = 1.0 / recTrim;
        // ---- head contour ("head bump") ----
        // [ATR] Fig 5-9: amplifier shelf −0.9 dB below ~300 Hz @15 ips compensates the secondary-gap
        // rise; residual ripple ≈ +0.5 dB near 60 Hz at 30 ips (Keane/ATR Service), scales with speed. [EST] shape.
        const double fBump = 60.0 * sp.v / 0.762;
        bump1.peak (fBump, 1.0, 0.6, fs);
        bump2.peak (fBump * 2.1, 1.4, -0.4, fs);
        bumpHP.highpass (fBump * 0.3, 0.6, fs);                       // [ATR] Table 1-4: within 2 dB at 35 Hz (30 ips) / 20 Hz (15 ips) / 30 Hz (7.5 ips)
        // ---- transformers [ATR schematic 4840421] ----
        // input T2: bridging 50 kΩ via 2×12.7 kΩ series → high source Z, nickel-core law [JEN proxy]
        //   1 % THD @ 20 Hz at +20 dBu  → in our units 0 VU = +4 dBu → 1 % at +16 dB re 0 VU
        inX.prepare (fs, 3.0, dB2lin (16.0), 0.0, 60000.0, 0.8);      // [JEN] LF −3 dB well below 10 Hz for a bridging input
        // output T1: low-Z drive, tertiary-winding NFB ≈ 8 dB [ATR]; +28 dBm/600 Ω max [ATR]
        //   [EST] 1 % THD @ 20 Hz at +24 dBu before feedback  → +20 dB re 0 VU
        outX.prepare (fs, 2.0, dB2lin (20.0), 8.0, 80000.0, 0.7);
        // ---- noise shaping & level ----
        double f3 = 20000.0; for (double f = 2000; f < 60000; f *= 1.02) { if (loss.playback (f) < 0.7071) { f3 = f; break; } }
        noiseLP.lowpass (std::min (f3, fs * 0.45), 0.6, fs);
        // [ATR] Table 1-4, 2-track, 30 Hz–18 kHz unweighted, re 1040 nWb/m
        double snr = 69.0;                                  // 15 NAB
        if (c.speed == 2) snr = 72.0; else if (c.speed == 1 && c.eq == 1) snr = 70.0;
        else if (c.speed == 0) snr = (c.eq == 0 ? 71.0 : 68.0);
        if (c.monoHeadblock) snr += 4.0;                    // full-track 73 vs 69 [ATR]
        snr -= ts.noiseRel456;
        if (c.vibe) snr -= 2.0;
        const double noiseFluxDb = lin2dB (1040.0) - snr;  // dB re 1 nWb/m
        const double targetRms = 0.70710678 * dB2lin (noiseFluxDb) / c.fluxAt0VU();   // SNR is rms tone / rms noise; 0 VU sine rms = 0.707
        double p = 0; const double df = 10.0;
        for (double f = 30; f <= std::min (18000.0, 0.49 * fs); f += df)
        { const double m = noiseLP.magnitudeAt (f, fs) * repHF.magnitudeAt (f, fs) * repLF.magnitudeAt (f, fs) * bump1.magnitudeAt (f, fs) * bumpHP.magnitudeAt (f, fs); p += m * m * df; }
        const double gainRms = std::sqrt (p / (fs / 2));   // unit-variance white input → band RMS
        noiseGain = targetRms / std::max (gainRms, 1e-9);
    }

    // ---- process one block (in place).  x in "0 VU = 1.0" units. ----
    void process (float* x, int n, bool hissOn, const float* hissSrc)
    {
        const AnhTable* t = &table;
        // record side (base rate)
        for (int i = 0; i < n; ++i)
        {
            double v = x[i];
            if (cfg.transformer) v = inX.process (v);
            v = recLF.process (v); v = recHFpole.process (v); v = recHF.process (v);
            x[i] = pre.process ((float) (v * recTrim));
        }
        // tape (oversampled, memoryless anhysteretic remanence)
        juce::dsp::AudioBlock<float> blk (&x, 1, (size_t) n);
        auto up = os->processSamplesUp (blk);
        float* u = up.getChannelPointer (0);
        const int m = (int) up.getNumSamples();
        for (int i = 0; i < m; ++i) u[i] = (float) (t->lookup (drive * (double) u[i]) * outScale);
        os->processSamplesDown (blk);
        // reproduce side (base rate)
        for (int i = 0; i < n; ++i)
        {
            double y = post.process (x[i]) * repTrim;
            if (hissOn && hissSrc) y += noiseGain * noiseLP.process (hissSrc[i]);
            y = repLF.process (y); y = repHF.process (y);
            y = bumpHP.process (bump2.process (bump1.process (y)));
            if (cfg.transformer) y = outX.process (y);
            x[i] = (float) y;
        }
    }

    const Config& config() const { return cfg; }
    const LossModel& lossModel() const { return loss; }
    double smallSignalPreGain (double f) const { return pre.magnitudeAt (f, fs) * recHFpole.magnitudeAt (f, fs) * recLF.magnitudeAt (f, fs); }

private:
    double fs = 48000, fsOS = 192000;
    int osFactor = 4;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    Config cfg;
    AnhTable table;
    double drive = 1, outScale = 1, noiseGain = 0, recTrim = 1, repTrim = 1;
    LossModel loss;
    FIRState pre, post;
    Biquad recLF, recHFpole, recHF, repLF, repHF, bump1, bump2, bumpHP, noiseLP;
    Transformer inX, outX;
};

// ───────────────────────────── wow & flutter [ATR Table 1-4] ─────────────────────────────
struct WowFlutter
{
    double fs = 48000; std::vector<float> buf; int w = 0, len = 0;
    double phWow = 0, phFl1 = 0, phFl2 = 0; double scrape = 0, scrapeLP = 0; uint32_t rng = 0x9E3779B9u;
    void prepare (double sampleRate) { fs = sampleRate; len = (int) (fs * 0.02) + 8; buf.assign ((size_t) len, 0.0f); w = 0; }
    inline float frand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return ((rng & 0xFFFFFF) / 8388608.0f) - 1.0f; }
    // returns delayed sample; depthPct = NAB rms flutter (0.04 → 0.04 %)
    inline float process (float x, double speedIps, double depthPct)
    {
        buf[(size_t) w] = x;
        // components: reel/capstan wow 0.6 Hz & capstan rotation (ATR capstan ≈ 1 rev per ~1.1 in → ~13 Hz @15 ips) [EST], scrape ~ 3 kHz
        const double fWow = 0.6, fCap = 13.0 * speedIps / 15.0, fFl2 = 0.44 * fCap;
        phWow += 2 * kPi * fWow / fs; phFl1 += 2 * kPi * fCap / fs; phFl2 += 2 * kPi * fFl2 / fs;
        scrape += 0.002 * (frand() - scrape);
        const double dev = depthPct * 0.01 * 1.4142 * 1.6 * (0.45 * std::sin (phWow) + 0.35 * std::sin (phFl1) + 0.2 * std::sin (phFl2 + 1.3) + 0.6 * scrape);
        // speed deviation integrates to a time offset; keep bounded by using the derivative form:
        // delay(t) = D0 + ∫dev dt  → approximate with each component's own integral
        const double tOff = depthPct * 0.01 * 1.4142 * 1.6 * ( 0.45 * -std::cos (phWow) / (2 * kPi * fWow)
                                                        + 0.35 * -std::cos (phFl1) / (2 * kPi * fCap)
                                                        + 0.2  * -std::cos (phFl2 + 1.3) / (2 * kPi * fFl2)) + 0.6 * dev * 1e-4;
        double d = 0.008 * fs + tOff * fs;      // centre 8 ms
        d = std::min (std::max (d, 1.0), (double) (len - 3));
        const int di = (int) d; const double fr = d - di;
        int r0 = w - di; if (r0 < 0) r0 += len; int r1 = r0 - 1; if (r1 < 0) r1 += len;
        const float y = (float) ((1 - fr) * buf[(size_t) r0] + fr * buf[(size_t) r1]);
        if (++w >= len) w = 0;
        return y;
    }
};

} // namespace ampex
