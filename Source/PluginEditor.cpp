/*
  Ampex - PluginEditor.cpp
  WebView-based UI with native JUCE DSP backend
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"

// Robust BinaryData include - handles different build configurations
#if __has_include("BinaryData.h")
  #include "BinaryData.h"
#elif __has_include(<BinaryData.h>)
  #include <BinaryData.h>
#endif

// ============================================================
// Constructor - Setup WebView
// ============================================================
AmpexAudioProcessorEditor::AmpexAudioProcessorEditor(AmpexAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setOpaque(true);
    addAndMakeVisible(webView);
    
    // Set initial size based on original HTML dimensions
    setSize(1152, 700);
    setResizable(true, true);
    setResizeLimits(400, 300, 1920, 1200);
    
    // Load the HTML UI
    loadWebUI();
    
    // Start timer for parameter sync (30 Hz)
    startTimerHz(30);
}

// ============================================================
// Destructor
// ============================================================
AmpexAudioProcessorEditor::~AmpexAudioProcessorEditor()
{
    stopTimer();
}

// ============================================================
// Load HTML UI from BinaryData (robust scan + data URL)
// ============================================================
void AmpexAudioProcessorEditor::loadWebUI()
{
#if defined(JUCE_TARGET_HAS_BINARY_DATA) && JUCE_TARGET_HAS_BINARY_DATA

    auto looksLikeHtml = [] (juce::String s)
    {
        s = s.trimStart();
        if (s.startsWithChar('<') && (s.containsIgnoreCase("<html") || s.containsIgnoreCase("<!doctype")))
            return true;
        // some generators embed url-encoded HTML
        if (s.startsWithIgnoreCase("%3c") || s.containsIgnoreCase("%3chtml") || s.containsIgnoreCase("%3c%21doctype"))
            return true;
        return false;
    };

    auto urlDecode = [] (const juce::String& in)
    {
        juce::String out;
        out.preallocateBytes((size_t) in.getNumBytesAsUTF8());

        for (int i = 0; i < in.length();)
        {
            auto ch = in[i];

            if (ch == '+') { out += ' '; ++i; continue; }

            if (ch == '%' && i + 2 < in.length())
            {
                auto h1 = in[i + 1];
                auto h2 = in[i + 2];

                auto hexVal = [] (juce::juce_wchar c) -> int
                {
                    if (c >= '0' && c <= '9') return (int)(c - '0');
                    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
                    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
                    return -1;
                };

                const int v1 = hexVal(h1);
                const int v2 = hexVal(h2);

                if (v1 >= 0 && v2 >= 0)
                {
                    out += (juce::juce_wchar)((v1 << 4) | v2);
                    i += 3;
                    continue;
                }
            }

            out += ch;
            ++i;
        }

        return out;
    };

    auto loadDataUrl = [this, &urlDecode] (const char* data, int size)
    {
        juce::String html = juce::String::fromUTF8((const char*)data, size);

        // decode if it looks url-encoded
        if (html.trimStart().startsWithChar('%') || html.containsIgnoreCase("%3c"))
            html = urlDecode(html);

        // data URL so hosts can't block file://
        auto b64 = juce::Base64::toBase64(html.toRawUTF8(), (size_t) html.getNumBytesAsUTF8());
        webView.goToURL("data:text/html;base64," + b64);
    };

    // [1] Try the most common JUCE names first
    {
        int sz = 0;
        if (auto* d = BinaryData::getNamedResource("web_index_html", sz)) { loadDataUrl(d, sz); return; }
        if (auto* d = BinaryData::getNamedResource("index_html", sz))     { loadDataUrl(d, sz); return; }
    }

    // [2] Scan everything and pick first thing that looks like HTML
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (auto* nm = BinaryData::namedResourceList[i])
        {
            int sz = 0;
            if (auto* d = BinaryData::getNamedResource(nm, sz))
            {
                juce::String txt = juce::String::fromUTF8((const char*)d, sz);
                if (looksLikeHtml(txt))
                {
                    loadDataUrl(d, sz);
                    return;
                }
            }
        }
    }

    // [3] Friendly fallback - never get a white screen
    {
        juce::String err = R"(<!doctype html><html><body style="background:#111;color:#f55;font-family:system-ui;padding:24px">
<h2>UI Load Error</h2>
<p>No embedded HTML was found in BinaryData. Expected <code>web/index.html</code>.</p>
</body></html>)";
        auto b64 = juce::Base64::toBase64(err.toRawUTF8(), (size_t) err.getNumBytesAsUTF8());
        webView.goToURL("data:text/html;base64," + b64);
    }

#else
    webView.goToURL("data:text/html,<h2 style='font-family:system-ui;color:#f55'>BinaryData disabled (JUCE_TARGET_HAS_BINARY_DATA=0)</h2>");
#endif
}

// ============================================================
// Handle parameter changes from Web UI
// ============================================================
void AmpexAudioProcessorEditor::handleParamFromWeb(const juce::String& paramId, float value)
{
    // Map web UI parameter IDs to APVTS parameter IDs
    juce::String mappedId = paramId;
    if (paramId == "preserve") mappedId = "preserveAmount";

    // FIX: the web UI sends raw engineering values for the choice switches
    // (speed 7.5/15/30 ips, flux 185/250/370/500 nWb/m). AudioParameterChoice
    // wants a 0-based index; convertTo0to1(30) just clamps to the top choice,
    // so 7.5 ips used to select 30 ips and 185 nWb/m used to select 500.
    if (mappedId == "speed")
    {
        if      (value < 11.0f) value = 0.0f;   // 7.5 ips
        else if (value < 22.0f) value = 1.0f;   // 15 ips
        else                    value = 2.0f;   // 30 ips
    }
    else if (mappedId == "flux")
    {
        if      (value < 217.0f) value = 0.0f;  // 185
        else if (value < 310.0f) value = 1.0f;  // 250
        else if (value < 435.0f) value = 2.0f;  // 370
        else                     value = 3.0f;  // 500
    }

    if (auto* param = audioProcessor.getAPVTS().getParameter(mappedId))
    {
        const float norm = param->getNormalisableRange().convertTo0to1(value);
        param->beginChangeGesture();
        param->setValueNotifyingHost(norm);
        param->endChangeGesture();
        // Remember what the UI already shows so the timer does not echo it straight back
        lastSent[mappedId] = *audioProcessor.getAPVTS().getRawParameterValue(mappedId);
    }
    else
    {
        DBG("[WEB] Unknown param: " + paramId + " (mapped: " + mappedId + ")");
    }
}

// ============================================================
// Send parameter value to Web UI
// ============================================================
void AmpexAudioProcessorEditor::sendParamToWeb(const juce::String& paramId, float value)
{
    if (!webReady)
        return;
    
    // Call JavaScript function to update UI
    juce::String js = "if(typeof juceSetParam==='function')juceSetParam('" + paramId + "'," + juce::String(value, 6) + ");";
    webView.evaluateJavascript(js, nullptr);
}

// ============================================================
// Timer callback - sync parameters to Web UI
// ============================================================
void AmpexAudioProcessorEditor::timerCallback()
{
    if (!webReady)
        return;

    // FIX: the generated version polled IDs that do not exist in the APVTS
    // ("bias", "btnexport", "power", "calnow", ...). getRawParameterValue()
    // returns nullptr for an unknown ID and the old code dereferenced it,
    // so the editor crashed the moment the web page reported __ready__.
    // Now we walk the real parameter list and null-check every lookup.
    static const char* const kParamIds[] = {
        "inDb", "outDb", "biasDb", "headroomDb", "monitor", "headblock",
        "speed", "flux", "eqStd", "tapeType", "transformer", "tapeHiss",
        "wowFlutter", "vibe", "preserveAmount", "postDb", "mix"
    };

    auto& apvts = audioProcessor.getAPVTS();
    for (auto* id : kParamIds)
    {
        auto* raw = apvts.getRawParameterValue(id);
        if (raw == nullptr)
            continue;

        const float v = raw->load();
        auto it = lastSent.find(id);
        if (it == lastSent.end() || std::abs(v - it->second) > 1e-6f)
        {
            lastSent[id] = v;
            sendParamToWeb(id, v);
        }
    }

    // ========================================
    // VU Meter Updates
    // ========================================
    {
        float meterL = audioProcessor.getMeterLevelL();
        float meterR = audioProcessor.getMeterLevelR();
        
        // Convert to dB for VU display
        float dbL = (meterL > 1e-10f) ? 20.0f * std::log10(meterL) : -60.0f;
        float dbR = (meterR > 1e-10f) ? 20.0f * std::log10(meterR) : -60.0f;
        
        // Send to JavaScript for VU needle animation
        juce::String jsVU = "if(typeof juceUpdateVU==='function')juceUpdateVU(" + 
            juce::String(dbL, 2) + "," + juce::String(dbR, 2) + ");";
        webView.evaluateJavascript(jsVU, nullptr);
    }
}

// ============================================================
// Paint - just fill black behind WebView
// ============================================================
void AmpexAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

// ============================================================
// Resized - WebView fills entire editor
// ============================================================
void AmpexAudioProcessorEditor::resized()
{
    webView.setBounds(getLocalBounds());
}
