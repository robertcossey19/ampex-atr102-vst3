/*
  Ampex — PluginProcessor.h
  ATR-102 record/reproduce chain, physics model (see TapeDSP.h for sources).
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
#include <atomic>
#include <cmath>
#include <vector>

#include "TapeDSP.h"

class AmpexAudioProcessor : public juce::AudioProcessor
{
public:
    AmpexAudioProcessor();
    ~AmpexAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
        if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo()) return false;
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

    // VU meter (linear, dBFS-referenced: a 0 VU sine reads −18 dBFS)
    std::atomic<float> meterLevelL { 0.0f };
    std::atomic<float> meterLevelR { 0.0f };
    float getMeterLevelL() const { return meterLevelL.load(); }
    float getMeterLevelR() const { return meterLevelR.load(); }

    // Introspection for tests
    const ampex::TapeChannel& channel (int i) const { return chans[(size_t) i]; }
    ampex::Config currentConfig() const;

private:
    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    double currentSampleRate = 48000.0;
    std::array<ampex::TapeChannel, 2> chans;
    std::array<ampex::WowFlutter, 2> wf;
    std::vector<float> noiseBuf[2];
    std::vector<float> workBuf[2];
    std::vector<float> dryDelay[2]; int dryPos = 0, dryLen = 1;   // aligns dry with the reported latency for mix/preserve
    uint32_t rng[2] { 0x12345678u, 0x9ABCDEF1u };

    inline float gauss (int ch)
    {
        // sum of four uniforms, unit variance
        float s = 0.0f;
        for (int k = 0; k < 4; ++k) { uint32_t& r = rng[ch]; r ^= r << 13; r ^= r >> 17; r ^= r << 5; s += ((r & 0xFFFFFF) / 8388608.0f) - 1.0f; }
        return s * 0.8660254f;   // 4 × var(1/3) = 4/3 → ×√(3/4)
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpexAudioProcessor)
};
