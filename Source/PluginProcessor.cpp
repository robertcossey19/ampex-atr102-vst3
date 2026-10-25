/*
  Ampex — PluginProcessor.cpp
  ATR-102 record/reproduce chain.  Signal path per channel (see TapeDSP.h):

    in → input gain → calibration (0 VU = −18 dBFS + Headroom) →
    [input transformer T2]  → record EQ (NAB/IEC/AES standard + inverse-loss pre-emphasis) →
    4× oversampled AC-bias anhysteretic remanence (per-stock, bias-dependent) →
    wavelength losses (bias-zone, spacing, thickness, gap) → bias noise → repro EQ →
    head contour → [output transformer T1 w/ tertiary NFB] → W&F → de-calibration → output gain

  Levels: 1.0 inside the model = 0 VU = +4 dBu at the I/O module = <flux> nWb/m on tape.
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
    inline float db2gain (float db) { return std::pow (10.0f, std::max (-60.0f, db) / 20.0f); }
}

AmpexAudioProcessor::AmpexAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
}

AmpexAudioProcessor::~AmpexAudioProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout AmpexAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("inDb", "Input Gain",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("outDb", "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("biasDb", "Bias",
        juce::NormalisableRange<float> (-5.0f, 5.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("headroomDb", "Headroom",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("monitor", "Monitor Repro", true));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("headblock", "Mono Headblock", false));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("speed", "Speed",
        juce::StringArray { "7.5 ips", "15 ips", "30 ips" }, 1));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("flux", "Fluxivity",
        juce::StringArray { "185", "250", "370", "500" }, 1));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("eqStd", "EQ Standard",
        juce::StringArray { "NAB", "IEC" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterChoice> ("tapeType", "Tape Type",
        juce::StringArray { "406", "456", "499", "GP9", "SM900", "SM911" }, 1));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("transformer", "Transformer", true));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("tapeHiss", "Tape Hiss", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("wowFlutter", "Wow & Flutter", false));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("vibe", "VIBE Mode", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("preserveAmount", "Preserve Character",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("postDb", "Post Level",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("mix", "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));

    return { params.begin(), params.end() };
}

void AmpexAudioProcessor::parameterChanged (const juce::String&, float) {}

const juce::String AmpexAudioProcessor::getName() const { return JucePlugin_Name; }
bool AmpexAudioProcessor::acceptsMidi() const { return false; }
bool AmpexAudioProcessor::producesMidi() const { return false; }
bool AmpexAudioProcessor::isMidiEffect() const { return false; }
double AmpexAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AmpexAudioProcessor::getNumPrograms() { return 1; }
int AmpexAudioProcessor::getCurrentProgram() { return 0; }
void AmpexAudioProcessor::setCurrentProgram (int) {}
const juce::String AmpexAudioProcessor::getProgramName (int) { return {}; }
void AmpexAudioProcessor::changeProgramName (int, const juce::String&) {}

ampex::Config AmpexAudioProcessor::currentConfig() const
{
    ampex::Config c;
    c.tape  = (int) *apvts.getRawParameterValue ("tapeType");
    c.speed = (int) *apvts.getRawParameterValue ("speed");
    c.eq    = (int) *apvts.getRawParameterValue ("eqStd");
    c.flux  = (int) *apvts.getRawParameterValue ("flux");
    c.biasDb = *apvts.getRawParameterValue ("biasDb");
    c.transformer = *apvts.getRawParameterValue ("transformer") > 0.5f;
    c.vibe = *apvts.getRawParameterValue ("vibe") > 0.5f;
    c.monoHeadblock = *apvts.getRawParameterValue ("headblock") > 0.5f;
    return c;
}

void AmpexAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int maxBlock = std::max (samplesPerBlock, 16);
    const ampex::Config c = currentConfig();
    for (int ch = 0; ch < 2; ++ch)
    {
        chans[(size_t) ch].configure (c, true);
        chans[(size_t) ch].prepare (sampleRate, maxBlock);
        wf[(size_t) ch].prepare (sampleRate);
        noiseBuf[ch].assign ((size_t) maxBlock, 0.0f);
        workBuf[ch].assign ((size_t) maxBlock, 0.0f);
    }
    const int lat = chans[0].latencySamples();
    setLatencySamples (lat);
    dryLen = std::max (1, lat + 1); dryPos = 0;
    for (int ch = 0; ch < 2; ++ch) dryDelay[ch].assign ((size_t) dryLen, 0.0f);
    meterLevelL.store (0.0f); meterLevelR.store (0.0f);
}

void AmpexAudioProcessor::releaseResources() {}

void AmpexAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0 || buffer.getNumChannels() < 2) return;
    if ((int) workBuf[0].size() < numSamples)
        for (int ch = 0; ch < 2; ++ch) { workBuf[ch].assign ((size_t) numSamples, 0.0f); noiseBuf[ch].assign ((size_t) numSamples, 0.0f); }

    const float inDb = *apvts.getRawParameterValue ("inDb");
    const float outDb = *apvts.getRawParameterValue ("outDb");
    const float headroomDb = *apvts.getRawParameterValue ("headroomDb");
    const bool  repro = *apvts.getRawParameterValue ("monitor") > 0.5f;
    const bool  hissOn = *apvts.getRawParameterValue ("tapeHiss") > 0.5f;
    const bool  wfOn = *apvts.getRawParameterValue ("wowFlutter") > 0.5f;
    const float preserve = *apvts.getRawParameterValue ("preserveAmount");
    const float postDb = *apvts.getRawParameterValue ("postDb");
    const float mix = *apvts.getRawParameterValue ("mix");
    const ampex::Config cfg = currentConfig();

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    if (! repro)      // INPUT monitor: hard bypass (unity)
    {
        meterLevelL.store (0.0f); meterLevelR.store (0.0f);
        return;
    }

    const float inGain = db2gain (inDb);
    const float calGain = db2gain (18.0f - headroomDb);        // 0 VU = (−18 + headroom) dBFS → 1.0
    const float decal = 1.0f / calGain;
    const float outGain = db2gain (outDb) * db2gain (postDb);
    const float wetMix = mix, dryMix = 1.0f - mix;          // linear: dry and wet are correlated (parallel-processing convention)
    const int lat = chans[0].latencySamples();

    for (int ch = 0; ch < 2; ++ch) chans[(size_t) ch].configure (cfg);

    const int nch = cfg.monoHeadblock ? 1 : 2;
    for (int ch = 0; ch < nch; ++ch)
    {
        float* w = workBuf[ch].data();
        const float* src = ch == 0 ? L : R;
        if (cfg.monoHeadblock) for (int i = 0; i < numSamples; ++i) w[i] = 0.5f * (L[i] + R[i]) * inGain * calGain;
        else                   for (int i = 0; i < numSamples; ++i) w[i] = src[i] * inGain * calGain;
        if (hissOn) { float* nz = noiseBuf[ch].data(); for (int i = 0; i < numSamples; ++i) nz[i] = gauss (ch); }
        chans[(size_t) ch].process (w, numSamples, hissOn, noiseBuf[ch].data());
    }
    if (cfg.monoHeadblock) std::copy (workBuf[0].begin(), workBuf[0].begin() + numSamples, workBuf[1].begin());

    // wow & flutter [ATR Table 1-4]: NAB rms 0.04 % @15, 0.06 % @7.5, 0.03 % @30 (+50 % in VIBE)
    if (wfOn)
    {
        static const double depth[3] = { 0.06, 0.04, 0.03 };
        const double d = depth[cfg.speed] * (cfg.vibe ? 1.5 : 1.0);
        const double ips = ampex::speedOf (cfg.speed).ips;
        for (int ch = 0; ch < 2; ++ch)
        {
            float* w = workBuf[ch].data();
            for (int i = 0; i < numSamples; ++i) w[i] = wf[(size_t) ch].process (w[i], ips, d);
        }
    }

    double sumL = 0, sumR = 0;
    for (int i = 0; i < numSamples; ++i)
    {
        // latency-aligned dry (the host delays everything else by `lat`, so the dry path must match)
        dryDelay[0][(size_t) dryPos] = L[i]; dryDelay[1][(size_t) dryPos] = R[i];
        int rd = dryPos - lat; if (rd < 0) rd += dryLen;
        const float dL = dryDelay[0][(size_t) rd], dR = dryDelay[1][(size_t) rd];
        if (++dryPos >= dryLen) dryPos = 0;
        float yL = workBuf[0][(size_t) i], yR = workBuf[1][(size_t) i];
        sumL += yL * yL; sumR += yR * yR;                    // meter on tape-side level (0 VU units)
        yL *= decal; yR *= decal;
        if (preserve > 0.0f) { yL += preserve * (dL * inGain - yL); yR += preserve * (dR * inGain - yR); }
        yL = (dL * dryMix + yL * wetMix) * outGain;
        yR = (dR * dryMix + yR * wetMix) * outGain;
        if (! std::isfinite (yL)) yL = 0.0f;
        if (! std::isfinite (yR)) yR = 0.0f;
        L[i] = yL; R[i] = yR;
    }
    // VU: RMS in 0 VU units → a 0 VU sine (rms 0.707) must read −18 dBFS: × √2 × 10^(−18/20)
    const float k = 1.41421356f * 0.12589254f;
    const float rmsL = (float) std::sqrt (sumL / numSamples) * k;
    const float rmsR = (float) std::sqrt (sumR / numSamples) * k;
    const float sm = 0.8f;
    meterLevelL.store (sm * meterLevelL.load() + (1.0f - sm) * rmsL);
    meterLevelR.store (sm * meterLevelR.load() + (1.0f - sm) * rmsR);
}

void AmpexAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void AmpexAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

bool AmpexAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* AmpexAudioProcessor::createEditor() { return new AmpexAudioProcessorEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AmpexAudioProcessor(); }
