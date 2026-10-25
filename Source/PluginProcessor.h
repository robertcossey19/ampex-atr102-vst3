/*
  Ampex - PluginProcessor.h
  MANUAL PORT of AnalogExact Ampex 102 Tape Machine Emulation
*/
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <algorithm>
#include <functional>
#include <cmath>
#include <vector>
#include <cstring>
#include <atomic>

// Forward declarations for DSP classes (defined in .cpp)
class Biquad;
class XorShift;
class DelayLine;
class OS4;

class AmpexAudioProcessor : public juce::AudioProcessor
{
public:
    AmpexAudioProcessor();
    ~AmpexAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override {
        if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;
        if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
            return false;
        return true;
    }
    
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void parameterChanged (const juce::String& parameterID, float newValue);
    
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    
    // ========================================
    // VU Meter Levels (thread-safe access)
    // ========================================
    std::atomic<float> meterLevelL { 0.0f };
    std::atomic<float> meterLevelR { 0.0f };
    float getMeterLevelL() const { return meterLevelL.load(); }
    float getMeterLevelR() const { return meterLevelR.load(); }

private:
    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    double currentSampleRate = 48000.0;
    
    // ========================================
    // DSP State - Biquad Filters
    // ========================================
    // Using simple struct to avoid forward declaration issues
    struct BiquadState {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        float sr = 44100.0f;
        
        void setSampleRate(float sampleRate) { sr = sampleRate; }
        void setCoeffs(const char* type, float freq, float Q, float gainDb);
        float process(float x);
        void reset() { z1 = 0.0f; z2 = 0.0f; }
    };
    
    BiquadState tapeHFShelf[2];
    BiquadState biasHF[2];
    BiquadState headBump[2];
    BiquadState deemph[2];
    BiquadState xfPrimaryHP[2];
    BiquadState xfLFBoost[2];
    BiquadState xfLFCut[2];
    BiquadState xfCoreLoss[2];
    BiquadState xfLeakageLP[2];
    BiquadState lpOut[2];
    BiquadState noiseHP[2];
    BiquadState noiseHF[2];
    
    // ========================================
    // DSP State - Delay Line for Wow/Flutter
    // ========================================
    struct DelayLineState {
        std::vector<float> bufL, bufR;
        int len = 0, w = 0;
        float sr = 44100.0f;
        
        void init(float sampleRate, float maxSec);
        void write(float l, float r);
        void read(float delaySamples, float& outL, float& outR);
        void reset();
    };
    
    DelayLineState delay;
    
    // ========================================
    // DSP State - Oversampling
    // ========================================
    struct OS4State {
        float prevL = 0.0f, prevR = 0.0f;
        std::function<float(float)> fn;
        
        OS4State() : fn([](float x) { return x; }) {}
        float processL(float x);
        float processR(float x);
    };
    
    OS4State tapeOS;
    OS4State xfOS;
    
    // ========================================
    // DSP State - Noise Generator
    // ========================================
    struct XorShiftState {
        uint32_t x;
        XorShiftState(uint32_t seed = 123456789) : x(seed ? seed : 123456789) {}
        float next() {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            return (static_cast<float>(x) / 2147483648.0f) - 1.0f;
        }
    };
    
    XorShiftState rngL{0x12345678};
    XorShiftState rngR{0x87654321};
    
    // ========================================
    // DSP State - Tape Saturation Parameters
    // ========================================
    float currentTapeK = 1.6f;
    float currentTapeAsym = 0.0f;      // continuous even-order asymmetry (replaces kNeg split)
    float currentTapeSoftness = 0.75f;
    float biasKPos = 1.6f;
    float biasAsym = 0.0f;             // bias-stage asymmetry (replaces biasKNeg)
    // DC blocker state after the bias stage (asymmetric shaping generates DC)
    float dcxL = 0.0f, dcyL = 0.0f, dcxR = 0.0f, dcyR = 0.0f;
    
    // Crosstalk
    float ctLin = 0.01f;
    float mainLin = 0.99f;
    
    // Wow/flutter phase accumulators
    float phWow = 0.0f;
    float phFl = 0.0f;
    float phFl2 = 0.0f;
    
    // Cache for filter updates
    int lastTapeType = -1;
    int lastSpeed = -1;
    int lastEQ = -1;
    bool lastTransformer = true;
    bool lastVibe = false;
    float lastBiasDb = -999.0f;
    
    // Helper methods
    float tapeSaturation(float x);
    void updateFilters(int tapeTypeIdx, int speedIdx, int fluxIdx, int eqIdx, 
                       float biasDb, bool transformerOn, bool vibeOn);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpexAudioProcessor)
};
