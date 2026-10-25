#include "PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <complex>
#include <chrono>

static double magAt (const std::vector<float>& y, double fs, double f, int skip)
{
    const int N = (int) y.size() - skip; std::complex<double> acc = 0;
    for (int n = 0; n < N; ++n) { const double w = 0.5 - 0.5 * cos (2 * M_PI * n / (N - 1)); acc += y[(size_t) (n + skip)] * w * std::exp (std::complex<double> (0, -2 * M_PI * f * n / fs)); }
    return std::abs (acc) * 4.0 / N;     // Hann-windowed amplitude estimate
}
static double thdPct (const std::vector<float>& y, double fs, double f0, int skip, int nH = 10)
{
    const double fund = magAt (y, fs, f0, skip); double h = 0;
    for (int k = 2; k <= nH; ++k) if (f0 * k < fs / 2) { const double m = magAt (y, fs, f0 * k, skip); h += m * m; }
    return 100.0 * sqrt (h) / fund;
}
static double rmsDb (const std::vector<float>& y, int skip) { double s = 0; for (size_t i = (size_t) skip; i < y.size(); ++i) s += y[i] * y[i]; return 20 * log10 (sqrt (s / (y.size() - skip)) + 1e-30); }
static void setP (AmpexAudioProcessor& p, const char* id, float v) { auto* par = p.getAPVTS().getParameter (id); jassert (par); par->setValueNotifyingHost (par->getNormalisableRange().convertTo0to1 (v)); }

static std::vector<float> run (AmpexAudioProcessor& p, double fs, double f0, double dbfs, double secs, bool silence = false)
{
    const int block = 512; const int total = (int) (fs * secs); std::vector<float> out; out.reserve ((size_t) total);
    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi; double ph = 0; const double a = pow (10, dbfs / 20);
    for (int done = 0; done < total; done += block)
    {
        for (int n = 0; n < block; ++n) { const float s = silence ? 0.f : (float) (a * sin (ph)); ph += 2 * M_PI * f0 / fs; buf.setSample (0, n, s); buf.setSample (1, n, s); }
        p.processBlock (buf, midi);
        for (int n = 0; n < block; ++n) out.push_back (buf.getSample (0, n));
    }
    return out;
}
// small-signal response at f relative to input, dB (−30 VU)
static double respDb (AmpexAudioProcessor& p, double fs, double f) { auto y = run (p, fs, f, -48.0, 0.5); return 20 * log10 (magAt (y, fs, f, (int) (fs * 0.25)) / pow (10, -48.0 / 20)); }

int main (int argc, char** argv)
{
    const double fs = argc > 1 ? atof (argv[1]) : 48000;
    AmpexAudioProcessor p; p.setPlayConfigDetails (2, 2, fs, 512); p.prepareToPlay (fs, 512);
    setP (p, "monitor", 1); setP (p, "tapeHiss", 0); setP (p, "wowFlutter", 0); setP (p, "transformer", 1);
    setP (p, "tapeType", 1); setP (p, "speed", 1); setP (p, "eqStd", 0); setP (p, "flux", 2);   // 456, 15 ips NAB, 370 nWb/m
    printf ("fs=%.0f latency=%d samples\n", fs, p.getLatencySamples());
    const int skip = (int) (fs * 0.25);

    printf ("\n== small-signal response (dB re input), 456 / 370 nWb/m ==\n");
    const double freqs[] = { 20, 30, 40, 50, 60, 80, 100, 200, 500, 1000, 2000, 5000, 10000, 12000, 15000, 18000, 20000 };
    for (int sp = 0; sp < 3; ++sp) for (int eq = 0; eq < (sp == 2 ? 1 : 2); ++eq)
    {
        setP (p, "speed", sp); setP (p, "eqStd", eq); p.prepareToPlay (fs, 512);
        printf ("%5s %s:", sp == 0 ? "7.5" : sp == 1 ? "15" : "30", eq == 0 ? (sp == 2 ? "AES" : "NAB") : "IEC");
        for (double f : freqs) if (f < fs * 0.45) printf (" %5.0f:%+5.2f", f, respDb (p, fs, f));
        printf ("\n");
    }
    setP (p, "speed", 1); setP (p, "eqStd", 0);

    printf ("\n== 1 kHz THD vs level, 456 @ 370 nWb/m = 0 VU = -18 dBFS (sheet: 0.1%% @260, 3%% @1072 nWb/m => 0.2%% @0VU, 3%% @+9.2 VU) ==\n");
    for (double vu : { -20.0, -10.0, -6.0, 0.0, 3.0, 6.0, 9.0, 12.0, 15.0 })
    { p.prepareToPlay (fs, 512); auto y = run (p, fs, 1000, -18 + vu, 0.6); printf ("  %+5.1f VU: out %+6.2f dB re in, THD %.3f%%\n", vu, 20 * log10 (magAt (y, fs, 1000, skip) / pow (10, (-18 + vu) / 20)), thdPct (y, fs, 1000, skip)); }

    printf ("\n== all stocks: THD at 0 VU (370) and level for 3%% ==\n");
    for (int t = 0; t < 6; ++t)
    {
        setP (p, "tapeType", t); p.prepareToPlay (fs, 512);
        auto y0 = run (p, fs, 1000, -18, 0.6); const double thd0 = thdPct (y0, fs, 1000, skip);
        double lo = -18, hi = 0, mid = -9;
        for (int it = 0; it < 12; ++it) { mid = 0.5 * (lo + hi); p.prepareToPlay (fs, 512); auto y = run (p, fs, 1000, mid, 0.5); if (thdPct (y, fs, 1000, skip) > 3.0) hi = mid; else lo = mid; }
        printf ("  %-6s THD@0VU %.3f%%   3%% at %+5.1f VU\n", ampex::tapeTable()[(size_t) t].name, thd0, mid + 18);
    }
    setP (p, "tapeType", 1);

    printf ("\n== HF saturation (SOL): 10 kHz output vs 1 kHz output at same input level; 456, per speed ==\n");
    for (int sp = 0; sp < 3; ++sp)
    {
        setP (p, "speed", sp); 
        printf ("  %4s ips:", sp == 0 ? "7.5" : sp == 1 ? "15" : "30");
        for (double vu : { -10.0, 0.0, 6.0, 9.0 })
        { p.prepareToPlay (fs, 512); auto a = run (p, fs, 1000, -18 + vu, 0.5); p.prepareToPlay (fs, 512); auto b = run (p, fs, 10000, -18 + vu, 0.5);
          printf ("  %+3.0fVU: 1k %+5.2f / 10k %+5.2f dB", vu, 20 * log10 (magAt (a, fs, 1000, skip) / pow (10, (-18 + vu) / 20)), 20 * log10 (magAt (b, fs, 10000, skip) / pow (10, (-18 + vu) / 20))); }
        printf ("\n");
    }
    setP (p, "speed", 1);

    printf ("\n== bias knob, 456 15 ips: level change at 1 kHz / 10 kHz (0 VU) and THD @ 1 kHz ==\n");
    for (double b : { -5.0, -3.0, -1.0, 0.0, 1.0, 3.0, 5.0 })
    { setP (p, "biasDb", (float) b); p.prepareToPlay (fs, 512); auto a = run (p, fs, 1000, -18, 0.5); p.prepareToPlay (fs, 512); auto c = run (p, fs, 10000, -18, 0.5);
      printf ("  bias %+4.1f dB: 1k %+5.2f dB  10k %+5.2f dB  THD1k %.3f%%\n", b, 20 * log10 (magAt (a, fs, 1000, skip) / 0.12589), 20 * log10 (magAt (c, fs, 10000, skip) / 0.12589), thdPct (a, fs, 1000, skip)); }
    setP (p, "biasDb", 0);

    printf ("\n== transformers: 20 Hz THD at +4 dBu(0 VU) and +16/+20 dB over; with vs without ==\n");
    for (int tr = 1; tr >= 0; --tr)
    { setP (p, "transformer", tr); for (double vu : { 0.0, 12.0, 16.0, 20.0 }) { p.prepareToPlay (fs, 512); auto y = run (p, fs, 20, -18 + vu, 1.0); printf ("  xfmr=%d %+3.0f VU 20 Hz: THD %.3f%%  level %+5.2f dB\n", tr, vu, thdPct (y, fs, 20, (int) (fs * 0.5)), 20 * log10 (magAt (y, fs, 20, (int) (fs * 0.5)) / pow (10, (-18 + vu) / 20))); } }
    setP (p, "transformer", 1);

    printf ("\n== hiss: output noise (dB re 0 VU) per speed, 456, flux 370 (ATR spec 2-trk: 30ips 72, 15 NAB 69, 7.5 NAB 71 dB below 1040 nWb/m => -63/-60/-62 re 0VU) ==\n");
    setP (p, "tapeHiss", 1);
    for (int sp = 0; sp < 3; ++sp) { setP (p, "speed", sp); p.prepareToPlay (fs, 512); auto y = run (p, fs, 1000, -200, 1.0, true); printf ("  %4s ips: %.1f dB re 0 VU\n", sp == 0 ? "7.5" : sp == 1 ? "15" : "30", rmsDb (y, skip) + 18.0 - 20 * log10 (0.70710678)); }
    setP (p, "tapeHiss", 0); setP (p, "speed", 1);

    printf ("\n== wow & flutter: 3 kHz tone, instantaneous frequency deviation (rms %%) ==\n");
    setP (p, "wowFlutter", 1); p.prepareToPlay (fs, 512);
    { auto y = run (p, fs, 3000, -18, 3.0); std::vector<double> per; double last = -1; for (size_t i = (size_t) skip + 1; i < y.size(); ++i) if (y[i - 1] <= 0 && y[i] > 0) { const double t = i - y[i] / (y[i] - y[i - 1]); if (last >= 0) per.push_back (t - last); last = t; }
      double m = 0; for (double v : per) m += v; m /= per.size(); double s = 0; for (double v : per) s += (v - m) * (v - m); s = sqrt (s / per.size());
      // smooth over 20 cycles to suppress sampling jitter
      std::vector<double> sm; for (size_t i = 20; i < per.size(); ++i) { double a = 0; for (size_t k = i - 20; k < i; ++k) a += per[k]; sm.push_back (a / 20); } double m2 = 0; for (double v : sm) m2 += v; m2 /= sm.size(); double s2 = 0; for (double v : sm) s2 += (v - m2) * (v - m2); s2 = sqrt (s2 / sm.size());
      printf ("  raw %.4f %%, 20-cycle smoothed %.4f %% (spec NAB rms 0.04 %% @15 ips)\n", 100 * s / m, 100 * s2 / m2); }
    setP (p, "wowFlutter", 0);

    printf ("\n== CPU: seconds of audio per second (stereo, this machine) ==\n");
    { p.prepareToPlay (fs, 512); auto t0 = std::chrono::steady_clock::now(); run (p, fs, 1000, -18, 10.0); auto t1 = std::chrono::steady_clock::now(); const double el = std::chrono::duration<double> (t1 - t0).count(); printf ("  10 s processed in %.3f s → %.1f× real-time\n", el, 10.0 / el); }
    return 0;
}
