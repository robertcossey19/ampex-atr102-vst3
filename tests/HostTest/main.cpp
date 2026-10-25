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

    printf("== Audio: Input monitor path (defaults) ==\n");
    { auto y = tail(h.run(1000, -20, 0.5), 16384); CHECK(std::isfinite(rmsDb(y)) && rmsDb(y) > -30, "passes audio, 1 kHz -20 dBFS -> %.2f dBFS", rmsDb(y)); }

    printf("== Audio: Repro (tape) path ==\n");
    h.setNorm("monitor", 1.0f);
    double thd40, thd20, thd12;
    { auto y = tail(h.run(1000, -40, 1.0), 16384); thd40 = thdPct(y, h.fs, 1000); printf("  -40 dBFS: out %.2f dBFS THD %.3f%%\n", rmsDb(y), thd40); }
    { auto y = tail(h.run(1000, -20, 1.0), 16384); thd20 = thdPct(y, h.fs, 1000); printf("  -20 dBFS: out %.2f dBFS THD %.3f%%\n", rmsDb(y), thd20); }
    { auto y = tail(h.run(1000, -12, 1.0), 16384); thd12 = thdPct(y, h.fs, 1000); printf("  -12 dBFS: out %.2f dBFS THD %.3f%%\n", rmsDb(y), thd12); }
    CHECK(thd40 < 0.3 && thd20 > thd40 && thd12 > thd20, "tape saturation is level-dependent (no crossover kink): %.3f%% < %.3f%% < %.3f%%", thd40, thd20, thd12);

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
    test("mix", 1.0f, 0.0f, 1000, "wet -> dry (level shift)", 0.3); h.setNorm("mix", 1.0f);
    {   // speed moves the head bump (50 / 80 / 120 Hz): compare 7.5 vs 30 ips at 50 Hz and 120 Hz
        h.setNorm("speed", 0.0f); double a50 = level(50), a120 = level(120);
        h.setNorm("speed", 1.0f); double b50 = level(50), b120 = level(120);
        h.setNorm("speed", 0.5f);
        double tilt = (a50 - a120) - (b50 - b120);
        CHECK(std::abs(tilt) > 0.5, "speed          7.5 vs 30 ips head bump: 50/120 Hz tilt changes by %.2f dB (7.5: %.2f/%.2f, 30: %.2f/%.2f dBFS)", tilt, a50, a120, b50, b120);
    }
    test("transformer", 1.0f, 0.0f, 40, "on -> off @40 Hz", 0.3); h.setNorm("transformer", 1.0f);
    test("tapeType", 0.2f, 0.4f, 1000, "456 -> 499 (level/sat)", 0.1); h.setNorm("tapeType", 0.2f);
    {   // flux = +6 dB drive into the tape with matching repro compensation -> level ~same, THD up
        h.setNorm("flux", 0.33f); double t250 = thdPct(tail(h.run(1000, -20, 0.6), 16384), h.fs, 1000);
        h.setNorm("flux", 1.0f);  double t500 = thdPct(tail(h.run(1000, -20, 0.6), 16384), h.fs, 1000);
        h.setNorm("flux", 0.33f);
        CHECK(t500 > t250 * 1.5, "flux           250 -> 500 nWb/m: THD %.2f%% -> %.2f%% at -20 dBFS", t250, t500);
    }
    test("eqStd", 0.0f, 1.0f, 10000, "NAB -> IEC @10 kHz", 0.1); h.setNorm("eqStd", 0.0f);
    {
        h.setNorm("tapeHiss", 1.0f); double hiss = rmsDb(tail(h.run(1000, -200, 1.0, true), 32768)); h.setNorm("tapeHiss", 0.0f);
        double quiet = rmsDb(tail(h.run(1000, -200, 1.0, true), 32768));
        CHECK(hiss > -95 && hiss < -70 && quiet < -120, "tapeHiss       on: %.1f dBFS, off: %.1f dBFS", hiss, quiet);
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
    test("vibe", 0.0f, 1.0f, 1000, "standard -> VIBE", 0.1); h.setNorm("vibe", 0.0f);

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
