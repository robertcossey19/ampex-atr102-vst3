// AmpexHostTest: black-box test of the compiled Ampex.vst3 through a real VST3 host.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS  "); printf(__VA_ARGS__); printf("\n"); } else { ++failures; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Host {
    std::unique_ptr<juce::AudioPluginInstance> plug;
    double fs = 48000; int block = 512;

    // Across the VST3 boundary the host only sees parameter *names*; map our APVTS IDs to them.
    static juce::String nameFor(const juce::String& id) {
        static const std::map<juce::String, juce::String> m = {
            {"inDb","Input Gain"},{"outDb","Output Gain"},{"biasDb","Bias"},{"headroomDb","Headroom"},
            {"monitor","Monitor Repro"},{"headblock","Mono Headblock"},{"speed","Speed"},{"flux","Fluxivity"},
            {"eqStd","EQ Standard"},{"tapeType","Tape Type"},{"transformer","Transformer"},{"tapeHiss","Tape Hiss"},
            {"wowFlutter","Wow & Flutter"},{"vibe","VIBE Mode"},{"preserveAmount","Preserve Character"},
            {"postDb","Post Level"},{"mix","Mix"} };
        auto it = m.find(id); return it == m.end() ? id : it->second;
    }
    juce::AudioProcessorParameter* param(const juce::String& id) {
        const auto nm = nameFor(id);
        for (auto* p : plug->getParameters()) if (p->getName(64) == nm) return p;
        return nullptr;
    }
    void setNorm(const juce::String& id, float norm) { auto* p = param(id); if (!p) { printf("  !! no such parameter: %s\n", id.toRawUTF8()); ++failures; return; } p->setValueNotifyingHost(norm); }

    std::vector<float> run(double f0, double dbfs, double secs, bool silence = false) {
        int total = (int)(fs * secs); std::vector<float> out; out.reserve((size_t)total);
        juce::AudioBuffer<float> buf(2, block); juce::MidiBuffer midi; double ph = 0, a = std::pow(10.0, dbfs / 20);
        for (int done = 0; done < total; done += block) {
            for (int n = 0; n < block; n++) { float s = silence ? 0.f : (float)(a * std::sin(ph)); ph += 2 * M_PI * f0 / fs; buf.setSample(0, n, s); buf.setSample(1, n, s); }
            plug->processBlock(buf, midi);
            for (int n = 0; n < block; n++) out.push_back(buf.getSample(0, n));
        }
        return out;
    }
};

static std::vector<float> tail(const std::vector<float>& y, size_t n) { return { y.end() - (long)n, y.end() }; }
static double rmsDb(const std::vector<float>& y) { double s = 0; for (float v : y) s += v * v; return 20 * std::log10(std::sqrt(s / y.size()) + 1e-30); }
static double mag(const std::vector<float>& y, double fs, double f) {
    std::complex<double> acc = 0; size_t N = y.size();
    for (size_t n = 0; n < N; n++) { double w = 0.5 - 0.5 * std::cos(2 * M_PI * n / (N - 1)); acc += y[n] * w * std::exp(std::complex<double>(0, -2 * M_PI * f * n / fs)); }
    return std::abs(acc);
}
static double thdPct(const std::vector<float>& y, double fs, double f0) {
    double fund = mag(y, fs, f0), h = 0; for (int k = 2; k <= 10; k++) if (f0 * k < fs / 2) { double m = mag(y, fs, f0 * k); h += m * m; }
    return 100.0 * std::sqrt(h) / fund;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: AmpexHostTest path/to/Ampex.vst3 [--editor]\n"); return 2; }
    bool testEditor = argc > 2 && juce::String(argv[2]) == "--editor";
    juce::ScopedJuceInitialiser_GUI gui;
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();

    juce::AudioPluginFormatManager fm; fm.addDefaultFormats();
    juce::OwnedArray<juce::PluginDescription> descs;
    juce::KnownPluginList kpl;
    for (auto* f : fm.getFormats()) kpl.scanAndAddFile(argv[1], true, descs, *f);
    printf("== Load ==\n");
    CHECK(descs.size() > 0, "VST3 scanned: %d description(s)", descs.size());
    if (descs.isEmpty()) return 1;
    auto* d = descs[0];
    printf("  name=%s  manufacturer=%s  version=%s  format=%s  category=%s\n", d->name.toRawUTF8(), d->manufacturerName.toRawUTF8(), d->version.toRawUTF8(), d->pluginFormatName.toRawUTF8(), d->category.toRawUTF8());

    Host h; juce::String err;
    h.plug = fm.createPluginInstance(*d, h.fs, h.block, err);
    CHECK(h.plug != nullptr, "instantiate: %s", err.isEmpty() ? "ok" : err.toRawUTF8());
    if (!h.plug) return 1;
    h.plug->enableAllBuses();
    h.plug->setPlayConfigDetails(2, 2, h.fs, h.block);
    h.plug->prepareToPlay(h.fs, h.block);

    printf("== Parameters ==\n");
    const char* expected[] = { "inDb","outDb","biasDb","headroomDb","monitor","headblock","speed","flux","eqStd","tapeType","transformer","tapeHiss","wowFlutter","vibe","preserveAmount","postDb","mix" };
    int found = 0;
    for (auto* id : expected) { bool ok = h.param(id) != nullptr; found += ok; if (!ok) printf("  missing: %s\n", id); }
    CHECK(found == 17, "all 17 parameters exposed to the host (%d found, %d total params)", found, h.plug->getParameters().size());
    for (auto* p : h.plug->getParameters())
        printf("     %-16s = %s\n", p->getName(32).toRawUTF8(), p->getCurrentValueAsText().toRawUTF8());

    printf("== Audio: INPUT monitor = hard bypass ==\n");
    h.setNorm("monitor", 0.0f);
    { auto y = tail(h.run(1000, -20, 0.5), 16384); CHECK(std::abs(rmsDb(y) - (-23.01)) < 0.1, "unity: 1 kHz -20 dBFS sine -> %.2f dBFS rms (expect -23.01)", rmsDb(y)); }

    printf("== Audio: Repro (tape) path ==\n");
    h.setNorm("monitor", 1.0f);
    double thd40, thd20, thd12;
    { auto y = tail(h.run(1000, -40, 1.0), 16384); thd40 = thdPct(y, h.fs, 1000); printf("  -40 dBFS: out %.2f dBFS THD %.3f%%\n", rmsDb(y), thd40); }
    { auto y = tail(h.run(1000, -20, 1.0), 16384); thd20 = thdPct(y, h.fs, 1000); printf("  -20 dBFS: out %.2f dBFS THD %.3f%%\n", rmsDb(y), thd20); }
    { auto y = tail(h.run(1000, -12, 1.0), 16384); thd12 = thdPct(y, h.fs, 1000); printf("  -12 dBFS: out %.2f dBFS THD %.3f%%\n", rmsDb(y), thd12); }
    CHECK(thd40 < 0.3 && thd20 > thd40 && thd12 > thd20, "tape saturation is level-dependent (no crossover kink): %.3f%% < %.3f%% < %.3f%%", thd40, thd20, thd12);
    {   // 456 @ 15 ips NAB, 0 VU = 250 nWb/m: 3 % THD (MOL 1072 nWb/m) must land at +12.6 dB over 0 VU = -5.4 dBFS (+/-1 dB)
        h.setNorm("tapeType", 0.2f); h.setNorm("flux", 0.33f);
        double thdM = thdPct(tail(h.run(1000, -5.4, 0.8), 16384), h.fs, 1000);
        CHECK(thdM > 2.3 && thdM < 3.9, "456 MOL: THD at -5.4 dBFS (1072 nWb/m) = %.2f%% (data sheet 3 %%)", thdM);
    }
    {   // small-signal frequency response flat at 15 ips [ATR Table 1-4: +/-0.75 dB 100 Hz-15 kHz]
        double r100 = rmsDb(tail(h.run(100, -40, 0.6), 16384)), r1k = rmsDb(tail(h.run(1000, -40, 0.6), 16384)), r10k = rmsDb(tail(h.run(10000, -40, 0.6), 16384));
        CHECK(std::abs(r100 - r1k) < 0.75 && std::abs(r10k - r1k) < 0.75, "response 15 ips: 100 Hz %+.2f / 10 kHz %+.2f dB re 1 kHz", r100 - r1k, r10k - r1k);
        h.setNorm("speed", 0.0f); double s1k = rmsDb(tail(h.run(1000, -40, 0.6), 16384)), s10k = rmsDb(tail(h.run(10000, -40, 0.6), 16384)); h.setNorm("speed", 0.5f);
        CHECK(std::abs(s10k - s1k) < 2.0, "response 7.5 ips: 10 kHz %+.2f dB re 1 kHz (spec +/-2 dB)", s10k - s1k);
    }
    {   // HF saturation: a hot 10 kHz tone compresses more at 7.5 ips than at 15 ips (SOL10 is ~9 dB below MOL at 7.5 ips per the SM911 sheet)
        double a15 = rmsDb(tail(h.run(10000, -6, 0.6), 16384)); h.setNorm("speed", 0.0f); double a75 = rmsDb(tail(h.run(10000, -6, 0.6), 16384)); h.setNorm("speed", 0.5f);
        CHECK(a15 - a75 > 1.5, "HF saturation: 10 kHz at -6 dBFS is %.2f dB lower at 7.5 ips than at 15 ips", a15 - a75);
    }

    printf("== Knobs / switches change the audio ==\n");
    auto level = [&](double f = 1000, double db = -20) { return rmsDb(tail(h.run(f, db, 0.4), 16384)); };
    auto test = [&](const char* id, float a, float b, double f, const char* what, double minDeltaDb) {
        h.setNorm(id, a); double la = level(f); h.setNorm(id, b); double lb = level(f);
        CHECK(std::abs(lb - la) >= minDeltaDb, "%-14s %s: %.2f -> %.2f dBFS (delta %.2f dB)", id, what, la, lb, lb - la);
    };
    // inDb -12..+12 ; norm 0.5 = 0 dB
    test("inDb", 0.5f, 0.75f, 1000, "0 -> +6 dB input", 2.0); h.setNorm("inDb", 0.5f);
    test("outDb", 0.8f, 0.6f, 1000, "0 -> -6 dB output", 5.0); h.setNorm("outDb", 0.8f);
    test("biasDb", 0.5f, 1.0f, 10000, "0 -> +5 dB @10 kHz", 1.0); h.setNorm("biasDb", 0.5f);
    test("postDb", 0.5f, 0.25f, 1000, "post level down", 3.0); h.setNorm("postDb", 0.5f);
    {   // mix: 0 = exactly dry (latency-aligned), 1 = wet, 0.5 in between
        h.setNorm("mix", 0.0f); double dry = level(1000, -12); h.setNorm("mix", 0.5f); double half = level(1000, -12); h.setNorm("mix", 1.0f); double wet = level(1000, -12);
        CHECK(std::abs(dry - (-15.01)) < 0.05 && half < std::max(dry, wet) + 0.1 && half > std::min(dry, wet) - 3.1, "mix            dry %.2f / 50%% %.2f / wet %.2f dBFS", dry, half, wet);
    }
    {   // speed changes the LF contour: 30 ips AES rolls off below 35 Hz, 15 ips NAB holds to 20 Hz [ATR Table 1-4]
        h.setNorm("speed", 1.0f); double a20 = level(20, -40), a1k = level(1000, -40);
        h.setNorm("speed", 0.5f); double b20 = level(20, -40), b1k = level(1000, -40);
        double tilt = (b20 - b1k) - (a20 - a1k);
        CHECK(tilt > 1.0, "speed          20 Hz re 1 kHz: 30 ips %+.2f dB, 15 ips %+.2f dB (difference %.2f dB)", a20 - a1k, b20 - b1k, tilt);
    }
    {   // transformers: level flat, LF THD changes slightly when toggled (tape dominates at 20 Hz, as on the real machine)
        double lOn = level(20, -18), tOn = thdPct(tail(h.run(20, -18, 1.0), 32768), h.fs, 20);
        h.setNorm("transformer", 0.0f); double lOff = level(20, -18), tOff = thdPct(tail(h.run(20, -18, 1.0), 32768), h.fs, 20); h.setNorm("transformer", 1.0f);
        CHECK(std::abs(lOn - lOff) < 0.5 && std::abs(tOn - tOff) > 0.005, "transformer    on/off @20 Hz 0 VU: level %.2f/%.2f dBFS, THD %.3f/%.3f%%", lOn, lOff, tOn, tOff);
    }
    {   // tape type: 499 (MOL 1370 nWb/m) is cleaner than 456 (1072) at the same level
        double t456 = thdPct(tail(h.run(1000, -9, 0.6), 16384), h.fs, 1000); h.setNorm("tapeType", 0.4f); double t499 = thdPct(tail(h.run(1000, -9, 0.6), 16384), h.fs, 1000); h.setNorm("tapeType", 0.2f);
        CHECK(t499 < t456 * 0.8, "tapeType       456 -> 499 at +9 VU: THD %.2f%% -> %.2f%%", t456, t499);
    }
    {   // flux = +6 dB drive into the tape with matching repro compensation -> level ~same, THD up
        h.setNorm("flux", 0.33f); double t250 = thdPct(tail(h.run(1000, -20, 0.6), 16384), h.fs, 1000);
        h.setNorm("flux", 1.0f);  double t500 = thdPct(tail(h.run(1000, -20, 0.6), 16384), h.fs, 1000);
        h.setNorm("flux", 0.33f);
        CHECK(t500 > t250 * 1.5, "flux           250 -> 500 nWb/m: THD %.2f%% -> %.2f%% at -20 dBFS", t250, t500);
    }
    {
        h.setNorm("tapeHiss", 1.0f); double hiss = rmsDb(tail(h.run(1000, -200, 1.0, true), 32768));
        h.setNorm("eqStd", 1.0f); double hissIec = rmsDb(tail(h.run(1000, -200, 1.0, true), 32768)); h.setNorm("eqStd", 0.0f); h.setNorm("tapeHiss", 0.0f);
        double quiet = rmsDb(tail(h.run(1000, -200, 1.0, true), 32768));
        // [ATR Table 1-4] 15 ips NAB: 69 dB below 1040 nWb/m -> 0 VU (250 nWb/m) rms is -21 dBFS -> hiss = -21 - 69 + 20log10(1040/250) = -77.6 dBFS
        CHECK(hiss > -80.6 && hiss < -74.6 && quiet < -120, "tapeHiss       on: %.1f dBFS (spec -77.6), off: %.1f dBFS", hiss, quiet);
        CHECK(std::abs(hissIec - hiss) > 0.5, "eqStd          NAB -> IEC changes the noise floor: %.1f -> %.1f dBFS (spec 69 vs 70 dB)", hiss, hissIec);
    }
    {
        h.setNorm("wowFlutter", 1.0f); auto w = h.run(1000, -20, 3.0); h.setNorm("wowFlutter", 0.0f);
        std::vector<double> zc; for (size_t n = (size_t)h.fs + 1; n < w.size(); n++) if (w[n - 1] < 0 && w[n] >= 0) zc.push_back(n - 1 + w[n - 1] / (w[n - 1] - w[n]));
        double mn = 1e9, mx = 0; int W = 50; for (size_t i = W; i < zc.size(); i++) { double f = W * h.fs / (zc[i] - zc[i - W]); mn = std::min(mn, f); mx = std::max(mx, f); }
        double devPct = 100 * (mx - mn) / 2 / 1000;
        CHECK(devPct > 0.02 && devPct < 0.3, "wowFlutter     1 kHz swings %.2f..%.2f Hz (%.3f%% peak)", mn, mx, devPct);
    }
    {
        h.setNorm("headblock", 1.0f); juce::AudioBuffer<float> b(2, 512); juce::MidiBuffer m;
        double rmsR = 0; for (int blk = 0; blk < 40; blk++) { for (int n = 0; n < 512; n++) { b.setSample(0, n, 0.1f * std::sin(2 * M_PI * 1000 * (blk * 512 + n) / h.fs)); b.setSample(1, n, 0.f); } h.plug->processBlock(b, m); if (blk > 30) for (int n = 0; n < 512; n++) rmsR += b.getSample(1, n) * b.getSample(1, n); }
        h.setNorm("headblock", 0.0f);
        CHECK(rmsR > 1e-6, "headblock      mono: L-only input appears on R (%.1f dBFS)", 10 * std::log10(rmsR / (9 * 512) + 1e-30));
    }
    test("vibe", 0.0f, 1.0f, 15000, "standard -> VIBE @15 kHz (worn-head spacing loss)", 0.5); h.setNorm("vibe", 0.0f);

    printf("== State save/restore ==\n");
    { h.setNorm("inDb", 0.75f); juce::MemoryBlock mb; h.plug->getStateInformation(mb); h.setNorm("inDb", 0.5f); h.plug->setStateInformation(mb.getData(), (int)mb.getSize());
      float v = h.param("inDb") ? h.param("inDb")->getValue() : -1.f; CHECK(std::abs(v - 0.75f) < 0.01f, "preset recall restores inDb (%.3f)", v); h.setNorm("inDb", 0.5f); }

    if (testEditor) {
        printf("== Editor ==\n");
        CHECK(h.plug->hasEditor(), "plugin reports an editor");
        std::unique_ptr<juce::AudioProcessorEditor> ed(h.plug->createEditorIfNeeded());
        CHECK(ed != nullptr, "editor created (%dx%d)", ed ? ed->getWidth() : 0, ed ? ed->getHeight() : 0);
        if (ed) {
            juce::DocumentWindow win("Ampex", juce::Colours::black, juce::DocumentWindow::allButtons);
            win.setContentNonOwned(ed.get(), true); win.setVisible(true); win.toFront(true);
            // The old editor crashed inside the first timer tick after the web page said __ready__.
            // Pump the message loop for 6 s while audio keeps running; surviving this is the test.
            for (int i = 0; i < 12; i++) { juce::MessageManager::getInstance()->runDispatchLoopUntil(500); h.run(1000, -20, 0.1); }
            CHECK(true, "editor open 6 s with web UI + audio running: no crash");
            win.clearContentComponent();
            h.plug->editorBeingDeleted(ed.get()); ed.reset();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
        }
    }

    h.plug->releaseResources(); h.plug.reset();
    printf("\n%s  (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: ALL PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
