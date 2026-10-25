/*
  Ampex - PluginProcessor.cpp
  MANUAL PORT of AnalogExact Ampex 102 Tape Machine Emulation
  
  Features:
  - Tape saturation with 6 tape formulas (406, 456, 499, GP9, SM900, SM911)
  - Transformer I/O coloration
  - Head bump EQ by speed
  - NAB/IEC EQ curves
  - Wow & flutter
  - Tape hiss
  - VIBE mode (enhanced character)
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <cstring>

// ============================================================
// Global Helper Functions
// ============================================================
namespace {
    inline float clampVal(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    
    inline float db2gain(float db) {
        return std::pow(10.0f, std::max(-60.0f, db) / 20.0f);
    }
    // Unfloored version for noise gains (db2gain's -60 dB floor pinned hiss at a fixed level)
    inline float db2gainExact(float db) {
        return std::pow(10.0f, db / 20.0f);
    }
}

// ============================================================
// BiquadState Implementation
// ============================================================
void AmpexAudioProcessor::BiquadState::setCoeffs(const char* type, float freq, float Q, float gainDb) {
    float f = clampVal(freq, 5.0f, 0.49f * sr);
    float w0 = 2.0f * 3.14159265359f * f / sr;
    float c = std::cos(w0), s = std::sin(w0);
    float q = std::max(0.05f, Q);
    float A = std::pow(10.0f, gainDb / 40.0f);
    float sqrtA = std::sqrt(A);
    float alpha = s / (2.0f * q);
    
    float b0_, b1_, b2_, a0_, a1_, a2_;
    
    if (std::strcmp(type, "lowpass") == 0) {
        b0_ = (1.0f - c) / 2.0f; b1_ = 1.0f - c; b2_ = (1.0f - c) / 2.0f;
        a0_ = 1.0f + alpha; a1_ = -2.0f * c; a2_ = 1.0f - alpha;
    } else if (std::strcmp(type, "highpass") == 0) {
        b0_ = (1.0f + c) / 2.0f; b1_ = -(1.0f + c); b2_ = (1.0f + c) / 2.0f;
        a0_ = 1.0f + alpha; a1_ = -2.0f * c; a2_ = 1.0f - alpha;
    } else if (std::strcmp(type, "peaking") == 0) {
        b0_ = 1.0f + alpha * A; b1_ = -2.0f * c; b2_ = 1.0f - alpha * A;
        a0_ = 1.0f + alpha / A; a1_ = -2.0f * c; a2_ = 1.0f - alpha / A;
    } else if (std::strcmp(type, "lowshelf") == 0) {
        float S = 1.0f;
        float alphaS = (s / 2.0f) * std::sqrt(((A + 1.0f/A) * (1.0f/S - 1.0f)) + 2.0f);
        b0_ = A * ((A + 1.0f) - (A - 1.0f) * c + 2.0f * sqrtA * alphaS);
        b1_ = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c);
        b2_ = A * ((A + 1.0f) - (A - 1.0f) * c - 2.0f * sqrtA * alphaS);
        a0_ = (A + 1.0f) + (A - 1.0f) * c + 2.0f * sqrtA * alphaS;
        a1_ = -2.0f * ((A - 1.0f) + (A + 1.0f) * c);
        a2_ = (A + 1.0f) + (A - 1.0f) * c - 2.0f * sqrtA * alphaS;
    } else if (std::strcmp(type, "highshelf") == 0) {
        float S = 1.0f;
        float alphaS = (s / 2.0f) * std::sqrt(((A + 1.0f/A) * (1.0f/S - 1.0f)) + 2.0f);
        b0_ = A * ((A + 1.0f) + (A - 1.0f) * c + 2.0f * sqrtA * alphaS);
        b1_ = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * c);
        b2_ = A * ((A + 1.0f) + (A - 1.0f) * c - 2.0f * sqrtA * alphaS);
        a0_ = (A + 1.0f) - (A - 1.0f) * c + 2.0f * sqrtA * alphaS;
        a1_ = 2.0f * ((A - 1.0f) - (A + 1.0f) * c);
        a2_ = (A + 1.0f) - (A - 1.0f) * c - 2.0f * sqrtA * alphaS;
    } else {
        b0_ = 1.0f; b1_ = 0.0f; b2_ = 0.0f; a0_ = 1.0f; a1_ = 0.0f; a2_ = 0.0f;
    }
    
    float inv = 1.0f / a0_;
    b0 = b0_ * inv; b1 = b1_ * inv; b2 = b2_ * inv;
    a1 = a1_ * inv; a2 = a2_ * inv;
}

float AmpexAudioProcessor::BiquadState::process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

// ============================================================
// DelayLineState Implementation
// ============================================================
void AmpexAudioProcessor::DelayLineState::init(float sampleRate, float maxSec) {
    sr = sampleRate;
    len = std::max(8, static_cast<int>(maxSec * sr) + 4);
    bufL.resize(len, 0.0f);
    bufR.resize(len, 0.0f);
    w = 0;
}

void AmpexAudioProcessor::DelayLineState::write(float l, float r) {
    bufL[w] = l; bufR[w] = r;
    w = (w + 1) % len;
}

void AmpexAudioProcessor::DelayLineState::read(float delaySamples, float& outL, float& outR) {
    float d = clampVal(delaySamples, 0.0f, static_cast<float>(len - 3));
    float idx = static_cast<float>(w) - d;
    while (idx < 0) idx += static_cast<float>(len);
    int i0 = static_cast<int>(idx);
    float frac = idx - static_cast<float>(i0);
    int i1 = (i0 + 1) % len;
    outL = bufL[i0] + (bufL[i1] - bufL[i0]) * frac;
    outR = bufR[i0] + (bufR[i1] - bufR[i0]) * frac;
}

void AmpexAudioProcessor::DelayLineState::reset() {
    std::fill(bufL.begin(), bufL.end(), 0.0f);
    std::fill(bufR.begin(), bufR.end(), 0.0f);
    w = 0;
}

// ============================================================
// OS4State Implementation
// ============================================================
float AmpexAudioProcessor::OS4State::processL(float x) {
    float p = prevL, d = (x - p) * 0.25f;
    float y = 0.25f * (fn(p + d) + fn(p + 2.0f * d) + fn(p + 3.0f * d) + fn(x));
    prevL = x;
    return y;
}

float AmpexAudioProcessor::OS4State::processR(float x) {
    float p = prevR, d = (x - p) * 0.25f;
    float y = 0.25f * (fn(p + d) + fn(p + 2.0f * d) + fn(p + 3.0f * d) + fn(x));
    prevR = x;
    return y;
}

// ============================================================
// Tape Profile Data
// ============================================================
namespace {
    struct TapeProfile {
        float saturationK;
        float saturationSoftness;
        float asymmetry;
        float hfShelfGain;
        float hfShelfFreq;
    };

    const TapeProfile tapeProfiles[6] = {
        { 2.2f, 0.70f, 0.15f, -1.5f, 8000.0f },   // 406
        { 1.6f, 0.75f, 0.12f, -0.5f, 10000.0f },  // 456
        { 0.9f, 0.60f, 0.10f, +0.5f, 12000.0f },  // 499
        { 0.8f, 0.90f, 0.08f, 0.0f, 14000.0f },   // GP9
        { 1.0f, 0.95f, 0.10f, +1.0f, 16000.0f },  // SM900
        { 1.4f, 0.80f, 0.12f, +0.3f, 12000.0f }   // SM911
    };

    struct HeadBump { float freq; float gain; float Q; };
    const HeadBump headBumpBySpeed[3] = {
        { 50.0f, 2.8f, 1.0f },   // 7.5 ips
        { 80.0f, 2.2f, 1.1f },   // 15 ips
        { 120.0f, 1.6f, 1.2f }   // 30 ips
    };

    struct WowFlutterSpec { float wowDepth; float flutterDepth; float wowRate; float flutterRate; };
    const WowFlutterSpec wfSpecs[3] = {
        { 0.0008f, 0.0003f, 0.8f, 7.5f },   // 7.5 ips
        { 0.0005f, 0.0002f, 1.0f, 9.0f },   // 15 ips
        { 0.0003f, 0.0001f, 1.2f, 12.0f }   // 30 ips
    };

    const float fluxDbOver185[4] = { 0.0f, 2.6f, 6.0f, 8.6f }; // 185, 250, 370, 500
    const float speedNoiseOffset[3] = { 6.0f, 3.0f, 0.0f }; // 7.5, 15, 30 ips
    const float tapeNoiseDb[6] = { -58.0f, -62.0f, -65.0f, -73.0f, -62.0f, -60.5f };
}

// ============================================================
// Constructor
// ============================================================
AmpexAudioProcessor::AmpexAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
}

AmpexAudioProcessor::~AmpexAudioProcessor() {}

// ============================================================
// Parameter Layout
// ============================================================
juce::AudioProcessorValueTreeState::ParameterLayout AmpexAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>("inDb", "Input Gain", 
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("outDb", "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("biasDb", "Bias",
        juce::NormalisableRange<float>(-5.0f, 5.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("headroomDb", "Headroom",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>("monitor", "Monitor Repro", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("headblock", "Mono Headblock", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("speed", "Speed",
        juce::StringArray{"7.5 ips", "15 ips", "30 ips"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("flux", "Fluxivity",
        juce::StringArray{"185", "250", "370", "500"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("eqStd", "EQ Standard",
        juce::StringArray{"NAB", "IEC"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("tapeType", "Tape Type",
        juce::StringArray{"406", "456", "499", "GP9", "SM900", "SM911"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>("transformer", "Transformer", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("tapeHiss", "Tape Hiss", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("wowFlutter", "Wow & Flutter", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("vibe", "VIBE Mode", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("preserveAmount", "Preserve Character",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("postDb", "Post Level",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("mix", "Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    
    return { params.begin(), params.end() };
}

void AmpexAudioProcessor::parameterChanged(const juce::String&, float) {}

// ============================================================
// Standard Plugin Methods
// ============================================================
const juce::String AmpexAudioProcessor::getName() const { return JucePlugin_Name; }
bool AmpexAudioProcessor::acceptsMidi() const { return false; }
bool AmpexAudioProcessor::producesMidi() const { return false; }
bool AmpexAudioProcessor::isMidiEffect() const { return false; }
double AmpexAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AmpexAudioProcessor::getNumPrograms() { return 1; }
int AmpexAudioProcessor::getCurrentProgram() { return 0; }
void AmpexAudioProcessor::setCurrentProgram(int) {}
const juce::String AmpexAudioProcessor::getProgramName(int) { return {}; }
void AmpexAudioProcessor::changeProgramName(int, const juce::String&) {}

// ============================================================
// Prepare To Play
// ============================================================
void AmpexAudioProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = sampleRate;
    
    // Initialize all biquad filters
    for (int ch = 0; ch < 2; ch++) {
        tapeHFShelf[ch].setSampleRate(static_cast<float>(sampleRate));
        biasHF[ch].setSampleRate(static_cast<float>(sampleRate));
        headBump[ch].setSampleRate(static_cast<float>(sampleRate));
        deemph[ch].setSampleRate(static_cast<float>(sampleRate));
        xfPrimaryHP[ch].setSampleRate(static_cast<float>(sampleRate));
        xfLFBoost[ch].setSampleRate(static_cast<float>(sampleRate));
        xfLFCut[ch].setSampleRate(static_cast<float>(sampleRate));
        xfCoreLoss[ch].setSampleRate(static_cast<float>(sampleRate));
        xfLeakageLP[ch].setSampleRate(static_cast<float>(sampleRate));
        lpOut[ch].setSampleRate(static_cast<float>(sampleRate));
        noiseHP[ch].setSampleRate(static_cast<float>(sampleRate));
        noiseHF[ch].setSampleRate(static_cast<float>(sampleRate));
    }
    
    // Initialize delay line for wow/flutter
    delay.init(static_cast<float>(sampleRate), 0.10f);
    
    // Initialize oversampling
    tapeOS.fn = [this](float x) { return tapeSaturation(x); };
    xfOS.fn = [](float x) { return std::tanh(x * 0.5f) * 0.997f; };
    
    // Reset phase accumulators and filter/DC-blocker state
    phWow = 0.0f; phFl = 0.0f; phFl2 = 0.0f;
    dcxL = dcyL = dcxR = dcyR = 0.0f;
    delay.reset();
    for (int ch = 0; ch < 2; ch++) {
        tapeHFShelf[ch].reset(); biasHF[ch].reset(); headBump[ch].reset(); deemph[ch].reset();
        xfPrimaryHP[ch].reset(); xfLFBoost[ch].reset(); xfLFCut[ch].reset(); xfCoreLoss[ch].reset();
        xfLeakageLP[ch].reset(); lpOut[ch].reset(); noiseHP[ch].reset(); noiseHF[ch].reset();
    }
    
    // Initialize noise generators
    rngL = XorShiftState(0x12345678);
    rngR = XorShiftState(0x87654321);
    
    // Crosstalk
    ctLin = db2gain(-40.0f);
    mainLin = 1.0f - ctLin;
    
    lastTapeType = -1; // Force filter update
}

void AmpexAudioProcessor::releaseResources() {}

// ============================================================
// Tape Saturation Function
// ============================================================
float AmpexAudioProcessor::tapeSaturation(float x)
{
    // FIX: continuous slope through zero. The old kPos/kNeg split put a derivative
    // discontinuity at x=0 (crossover distortion: constant ~5% THD at any level).
    // Asymmetry is now an even-order term inside the shaper, so even harmonics
    // scale with level the way tape does.
    const float k = currentTapeK;
    const float asym = currentTapeAsym;
    const float softness = currentTapeSoftness;
    const float mix = softness * 0.30f;

    const float u = k * (x + 0.5f * asym * x * x);
    const float hard = std::tanh(u);
    const float soft = (u / (1.0f + std::abs(u * 0.5f))) / k;
    float y = hard * (1.0f - mix) + soft * mix;
    y *= 0.997f;
    return clampVal(y, -0.997f, 0.997f);
}

// ============================================================
// Update Filters Based on Parameters
// ============================================================
void AmpexAudioProcessor::updateFilters(int tapeTypeIdx, int speedIdx, int fluxIdx, int eqIdx, 
                                               float biasDb, bool transformerOn, bool vibeOn)
{
    const TapeProfile& tp = tapeProfiles[tapeTypeIdx];
    const HeadBump& bump = headBumpBySpeed[speedIdx];
    
    // Hysteresis parameters per tape type (from original worklet)
    // k values in A/m, c is coercivity factor
    const float hystK[6] = { 26260.0f, 25465.0f, 31035.0f, 29443.0f, 29443.0f, 25465.0f };
    const float hystC[6] = { 0.22f, 0.17f, 0.12f, 0.10f, 0.14f, 0.15f };
    
    float baseAsym = tp.asymmetry;
    
    if (!vibeOn) {
        // Standard mode
        currentTapeK = tp.saturationK;
        currentTapeAsym = baseAsym;
        currentTapeSoftness = tp.saturationSoftness;
    } else {
        // VIBE mode - exact port from original worklet
        float kFactor = hystK[tapeTypeIdx] / 30000.0f;
        float cFactor = hystC[tapeTypeIdx];
        float kBase = tp.saturationK;
        float k = kBase * (0.94f + 0.12f * kFactor);
        
        float softness = tp.saturationSoftness;
        softness += (cFactor - 0.14f) * 0.12f;
        softness = std::min(0.98f, std::max(0.10f, softness));
        
        float asym = baseAsym * (1.0f + (1.0f - kFactor) * 0.15f);
        
        currentTapeK = k;
        currentTapeAsym = asym;
        currentTapeSoftness = softness;
    }
    
    // Bias asymmetry modification (FIX: continuous, no +/- slope split)
    float biasAsymMod = clampVal(biasDb * 0.01f, -0.05f, 0.05f);
    biasKPos = 1.6f;
    biasAsym = clampVal(tp.asymmetry + biasAsymMod, -0.3f, 0.3f);
    
    // Tape HF shelf
    for (int ch = 0; ch < 2; ch++) {
        tapeHFShelf[ch].setCoeffs("highshelf", tp.hfShelfFreq, 0.707f, tp.hfShelfGain);
        
        // Bias HF tilt
        float biasTilt = clampVal(-0.6f * biasDb, -3.0f, 3.0f);
        biasHF[ch].setCoeffs("highshelf", 8000.0f, 0.707f, biasTilt);
        
        // Head bump
        headBump[ch].setCoeffs("peaking", bump.freq, bump.Q, bump.gain);
        
        // De-emphasis based on EQ standard and speed
        float deemphFreq = (eqIdx == 0) ? 3180.0f : ((speedIdx == 0) ? 2270.0f : (speedIdx == 1) ? 4550.0f : 9100.0f);
        float deemphGain = (eqIdx == 0) ? ((speedIdx == 0) ? -1.25f : (speedIdx == 1) ? -0.75f : -0.5f) 
                                        : ((speedIdx == 0) ? -0.75f : (speedIdx == 1) ? -0.25f : 0.0f);
        deemph[ch].setCoeffs("highshelf", deemphFreq, 0.707f, deemphGain);
        
        // Transformer filters
        xfPrimaryHP[ch].setCoeffs("highpass", 30.0f, 0.707f, 0.0f);
        xfLFBoost[ch].setCoeffs("lowshelf", 150.0f, 0.707f, 4.0f);
        xfLFCut[ch].setCoeffs("lowshelf", 150.0f, 0.707f, -4.0f);
        xfCoreLoss[ch].setCoeffs("highshelf", 12000.0f, 0.707f, -1.0f);
        xfLeakageLP[ch].setCoeffs("lowpass", 18000.0f, 0.6f, 0.0f);
        
        // Output lowpass
        float lpFreq = transformerOn ? 20000.0f : 22050.0f;
        lpOut[ch].setCoeffs("lowpass", lpFreq, 0.707f, 0.0f);
        
        // Noise shaping
        noiseHP[ch].setCoeffs("highpass", 400.0f, 0.5f, 0.0f);
        noiseHF[ch].setCoeffs("highshelf", 2000.0f, 0.707f, 6.0f);
    }
}

// ============================================================
// Process Block
// ============================================================
void AmpexAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    
    // Get parameters
    float inDb = *apvts.getRawParameterValue("inDb");
    float outDb = *apvts.getRawParameterValue("outDb");
    float biasDb = *apvts.getRawParameterValue("biasDb");
    float headroomDb = *apvts.getRawParameterValue("headroomDb");
    bool isRepro = *apvts.getRawParameterValue("monitor") >= 0.5f;
    bool monoHB = *apvts.getRawParameterValue("headblock") >= 0.5f;
    int speedIdx = static_cast<int>(*apvts.getRawParameterValue("speed"));
    int fluxIdx = static_cast<int>(*apvts.getRawParameterValue("flux"));
    int eqIdx = static_cast<int>(*apvts.getRawParameterValue("eqStd"));
    int tapeTypeIdx = static_cast<int>(*apvts.getRawParameterValue("tapeType"));
    bool transformerOn = *apvts.getRawParameterValue("transformer") >= 0.5f;
    bool tapeHissOn = *apvts.getRawParameterValue("tapeHiss") >= 0.5f;
    bool wfOn = *apvts.getRawParameterValue("wowFlutter") >= 0.5f;
    bool vibeOn = *apvts.getRawParameterValue("vibe") >= 0.5f;
    float preserveAmount = *apvts.getRawParameterValue("preserveAmount");
    float postDb = *apvts.getRawParameterValue("postDb");
    float mixAmount = *apvts.getRawParameterValue("mix");
    
    // Update filters if tape type changed
    // Also update when vibeOn changes for immediate VIBE effect
    // FIX: bias knob also drives biasHF tilt + asymmetry; it was not in this check,
    // so turning BIAS did nothing until you changed tape type.
    if (tapeTypeIdx != lastTapeType || speedIdx != lastSpeed || eqIdx != lastEQ || 
        transformerOn != lastTransformer || vibeOn != lastVibe ||
        std::abs(biasDb - lastBiasDb) > 1e-4f) {
        updateFilters(tapeTypeIdx, speedIdx, fluxIdx, eqIdx, biasDb, transformerOn, vibeOn);
        lastTapeType = tapeTypeIdx;
        lastSpeed = speedIdx;
        lastEQ = eqIdx;
        lastTransformer = transformerOn;
        lastVibe = vibeOn;
        lastBiasDb = biasDb;
    }
    
    // Calculate gains
    const float INPUT_DB_OFFSET = 0.0f; // FIX: no hidden -5 dB pad before the saturator
    float inGain = db2gain(inDb + INPUT_DB_OFFSET);
    float fluxDb = fluxDbOver185[fluxIdx];
    float flGain = db2gain(fluxDb);
    if (transformerOn) flGain *= db2gain(-1.0f);
    float headroomGain = db2gain(headroomDb);
    float outMakeupDb = transformerOn ? 1.0f : 0.0f;
    float reproCompDb = -fluxDb;
    float outGain = db2gain(outDb + outMakeupDb + reproCompDb);
    
    // Noise gain calculation
    float noiseGain = 0.0f;
    if (tapeHissOn && isRepro) {
        float tapeNoise = tapeNoiseDb[tapeTypeIdx];
        float speedOff = speedNoiseOffset[speedIdx];
        // FIX: unfloored gain, rescaled so 456 / 250 nWb/m / 15 ips lands near -83 dBFS
        // (~64 dB S/N re 0 VU at -19 dBFS, per the Quantegy 456 sheet). Now tracks tape/speed/flux.
        float effectiveNoiseDb = tapeNoise - fluxDb + speedOff - 22.0f;
        noiseGain = db2gainExact(effectiveNoiseDb);
    }
    
    // Post gain (applied after all processing)
    float postGain = db2gain(postDb);
    
    // Mix gains (equal-power crossfade: dry = cos(mix*PI/2), wet = sin(mix*PI/2))
    float mixClamped = std::max(0.0f, std::min(1.0f, mixAmount));
    float dryMixGain = std::cos(mixClamped * 3.14159265f * 0.5f);
    float wetMixGain = std::sin(mixClamped * 3.14159265f * 0.5f);
    
    // Wow/flutter parameters
    const WowFlutterSpec& wfSpec = wfSpecs[speedIdx];
    // FIX: a peak pitch deviation d (fraction) at modulation rate f needs a delay
    // amplitude of d / (2*pi*f) seconds. The old *0.002 gave ~1 us = 0.0006% (inaudible).
    const float twoPi = 6.28318530718f;
    float wowDelayMod = wfSpec.wowDepth / (twoPi * wfSpec.wowRate);
    float flutterDelayMod = wfSpec.flutterDepth / (twoPi * wfSpec.flutterRate);
    float wowInc = wfSpec.wowRate / static_cast<float>(currentSampleRate);
    float flInc = wfSpec.flutterRate / static_cast<float>(currentSampleRate);
    float fl2Inc = (wfSpec.flutterRate * 0.6f) / static_cast<float>(currentSampleRate);
    
    float* leftChannel = buffer.getWritePointer(0);
    float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : leftChannel;
    
    int numSamples = buffer.getNumSamples();
    
    for (int i = 0; i < numSamples; ++i) {
        float L = leftChannel[i];
        float R = rightChannel[i];
        
        // Mono headblock sum
        if (monoHB) {
            float m = 0.5f * (L + R);
            L = m; R = m;
        }
        
        // INPUT monitor = true bypass
        if (!isRepro) {
            leftChannel[i] = L;
            rightChannel[i] = R;
            continue;
        }
        
        // === REPRO PATH ===
        
        // Store dry signal for Preserve Character blend
        float dryL = L;
        float dryR = R;
        
        // Input gain
        L *= inGain;
        R *= inGain;
        
        // Store post-input-gain dry for accurate preserve
        float dryLGained = L;
        float dryRGained = R;
        
        // Flux gain
        L *= flGain;
        R *= flGain;
        
        // Tape saturation (4x oversampled)
        L = tapeOS.processL(L);
        R = tapeOS.processR(R);
        
        // Tape HF shelf
        L = tapeHFShelf[0].process(L);
        R = tapeHFShelf[1].process(R);
        
        // Bias stage saturation (FIX: continuous asymmetry + DC blocker)
        L = std::tanh(biasKPos * (L + 0.5f * biasAsym * L * L));
        R = std::tanh(biasKPos * (R + 0.5f * biasAsym * R * R));
        {
            const float yl = L - dcxL + 0.9995f * dcyL; dcxL = L; dcyL = yl; L = yl;
            const float yr = R - dcxR + 0.9995f * dcyR; dcxR = R; dcyR = yr; R = yr;
        }
        
        // Bias HF
        L = biasHF[0].process(L);
        R = biasHF[1].process(R);
        
        // Headroom
        L *= headroomGain;
        R *= headroomGain;
        
        // Head bump
        L = headBump[0].process(L);
        R = headBump[1].process(R);
        
        // De-emphasis
        L = deemph[0].process(L);
        R = deemph[1].process(R);
        
        // Output gain
        L *= outGain;
        R *= outGain;
        
        // Transformer path
        if (transformerOn) {
            L = xfPrimaryHP[0].process(L);
            R = xfPrimaryHP[1].process(R);
            
            L = xfLFBoost[0].process(L);
            R = xfLFBoost[1].process(R);
            
            L = xfOS.processL(L);
            R = xfOS.processR(R);
            
            L = xfLFCut[0].process(L);
            R = xfLFCut[1].process(R);
            
            L = xfCoreLoss[0].process(L);
            R = xfCoreLoss[1].process(R);
            
            L = xfLeakageLP[0].process(L);
            R = xfLeakageLP[1].process(R);
        }
        
        // Output lowpass
        L = lpOut[0].process(L);
        R = lpOut[1].process(R);
        
        // Wow & flutter
        if (wfOn) {
            float wow = std::sin(2.0f * 3.14159265359f * phWow);
            float fl = std::sin(2.0f * 3.14159265359f * phFl);
            float t = phFl2 - std::floor(phFl2);
            float fl2 = 2.0f * std::abs(2.0f * (t - std::floor(t + 0.5f))) - 1.0f; // triangle
            
            float mod = wowDelayMod * wow + flutterDelayMod * fl + flutterDelayMod * 0.7f * fl2;
            float delaySec = clampVal(0.005f + mod, 0.0f, 0.095f);
            
            float dL, dR;
            delay.read(delaySec * static_cast<float>(currentSampleRate), dL, dR);
            delay.write(L, R);
            L = dL; R = dR;
            
            phWow += wowInc; if (phWow >= 1.0f) phWow -= 1.0f;
            phFl += flInc; if (phFl >= 1.0f) phFl -= 1.0f;
            phFl2 += fl2Inc; if (phFl2 >= 1.0f) phFl2 -= 1.0f;
        } else {
            delay.write(L, R);
        }
        
        // Crosstalk
        float finalL = mainLin * L + ctLin * R;
        float finalR = mainLin * R + ctLin * L;
        L = finalL; R = finalR;
        
        // Tape hiss
        if (noiseGain > 0.0f) {
            float nL = noiseHF[0].process(noiseHP[0].process(rngL.next())) * noiseGain;
            float nR = noiseHF[1].process(noiseHP[1].process(rngR.next())) * noiseGain;
            L += nL; R += nR;
        }
        
        // ========================================
        // PRESERVE CHARACTER
        // Blends wet signal toward dry to preserve original character
        // At 0%: full wet (tape processed)
        // At 100%: full dry (original signal)
        // ========================================
        if (preserveAmount > 0.0f) {
            // Scale dry to match approximate wet level for smooth blend
            float wetL = L;
            float wetR = R;
            L = wetL + preserveAmount * (dryLGained - wetL);
            R = wetR + preserveAmount * (dryRGained - wetR);
        }
        
        // Apply dry/wet mix (equal-power crossfade)
        // dryL/dryR are the original input before any processing (saved at start of repro path)
        L = dryL * dryMixGain + L * wetMixGain;
        R = dryR * dryMixGain + R * wetMixGain;
        
        // Apply post gain
        L *= postGain;
        R *= postGain;
        
        // Denormal protection
        if (!std::isfinite(L)) L = 0.0f;
        if (!std::isfinite(R)) R = 0.0f;
        
        leftChannel[i] = L;
        rightChannel[i] = R;
    }
    
    // ========================================
    // VU Meter Level Calculation
    // ========================================
    float sumL = 0.0f, sumR = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float sL = leftChannel[i];
        float sR = rightChannel[i];
        sumL += sL * sL;
        sumR += sR * sR;
    }
    float rmsL = std::sqrt(sumL / static_cast<float>(numSamples));
    float rmsR = std::sqrt(sumR / static_cast<float>(numSamples));
    
    // Smooth the meter levels (simple lowpass)
    float smoothing = 0.8f;
    meterLevelL.store(smoothing * meterLevelL.load() + (1.0f - smoothing) * rmsL);
    meterLevelR.store(smoothing * meterLevelR.load() + (1.0f - smoothing) * rmsR);
}

// ============================================================
// State Save/Load
// ============================================================
void AmpexAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void AmpexAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

// ============================================================
// Editor
// ============================================================
bool AmpexAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* AmpexAudioProcessor::createEditor()
{
    return new AmpexAudioProcessorEditor(*this);
}

// ============================================================
// Plugin Instance
// ============================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AmpexAudioProcessor();
}
