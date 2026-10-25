/*
  Ampex - PluginEditor.h
  WebView-based UI - displays original HTML interface
  with native JUCE DSP backend
*/
#pragma once
#include "PluginProcessor.h"
#include <map>

class AmpexAudioProcessorEditor : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit AmpexAudioProcessorEditor(AmpexAudioProcessor&);
    ~AmpexAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    AmpexAudioProcessor& audioProcessor;
    
    // ========================================
    // WebView Component - displays HTML UI
    // ========================================
    class WebUI : public juce::WebBrowserComponent
    {
    public:
        WebUI(AmpexAudioProcessorEditor& e) : editor(e) 
        {
            setOpaque(false);
        }
        
        bool pageAboutToLoad(const juce::String& url) override
        {
            // Intercept juce://log/{level}/{msg} URLs for console forwarding
            if (url.startsWith("juce://log/"))
            {
                auto rest = url.substring(11);
                auto slash = rest.indexOf("/");
                auto level = juce::URL::removeEscapeChars(slash > 0 ? rest.substring(0, slash) : rest);
                auto msg = juce::URL::removeEscapeChars(slash > 0 ? rest.substring(slash + 1) : "");
                DBG("[WEB][" + level + "] " + msg);
                return false;
            }
            
            // Intercept juce://param/{id}/{value} URLs from web UI
            if (url.startsWith("juce://param/"))
            {
                auto paramPart = url.substring(13);
                auto slashPos = paramPart.indexOf("/");
                if (slashPos > 0)
                {
                    auto paramId = juce::URL::removeEscapeChars(paramPart.substring(0, slashPos));
                    auto value = paramPart.substring(slashPos + 1).getFloatValue();
                    
                    if (paramId == "__ready__")
                        editor.webReady = true;
                    else
                        editor.handleParamFromWeb(paramId, value);
                }
                return false;  // Don't actually navigate
            }
            return true;
        }
        
    private:
        AmpexAudioProcessorEditor& editor;
    };
    
    WebUI webView{*this};
    bool webReady = false;
    
    void loadWebUI();
    void handleParamFromWeb(const juce::String& paramId, float value);
    void sendParamToWeb(const juce::String& paramId, float value);
    
    // Track last values to avoid redundant updates (keyed by real APVTS IDs)
    std::map<juce::String, float> lastSent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AmpexAudioProcessorEditor)
};
