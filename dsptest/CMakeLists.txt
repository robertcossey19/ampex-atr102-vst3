# Measurement / calibration tools (not part of the plugin build).
#   cmake -S dsptest -B dsptest/build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C dsptest/build
#   dsptest/build/AmpexTest_artefacts/Release/AmpexTest [sampleRate]   -> response, THD, SOL, bias, hiss, W&F, CPU
#   dsptest/build/AmpexCalib_artefacts/Release/AmpexCalib > Source/TapeCalibration.inc   (re-run if TapeDSP.h tape constants change)
cmake_minimum_required(VERSION 3.22)
project(AmpexTest VERSION 2.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(../JUCE JUCE)
juce_add_console_app(AmpexTest PRODUCT_NAME "AmpexTest" NEEDS_WEB_BROWSER TRUE)
target_sources(AmpexTest PRIVATE main.cpp ../Source/PluginProcessor.cpp ../Source/PluginEditor.cpp)
target_include_directories(AmpexTest PRIVATE ../Source)
target_compile_definitions(AmpexTest PRIVATE JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JucePlugin_Name="Ampex" JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(AmpexTest PRIVATE juce::juce_audio_processors juce::juce_dsp juce::juce_gui_basics juce::juce_gui_extra juce::juce_recommended_config_flags)

juce_add_console_app(AmpexCalib PRODUCT_NAME "AmpexCalib")
target_sources(AmpexCalib PRIVATE calib.cpp)
target_include_directories(AmpexCalib PRIVATE ../Source)
target_compile_definitions(AmpexCalib PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(AmpexCalib PRIVATE juce::juce_dsp juce::juce_recommended_config_flags)

# Reference: the browser's Jiles-Atherton core driven by a decaying AC-bias field (why JA is not used for the tape)
juce_add_console_app(AmpexAnh PRODUCT_NAME "AmpexAnh")
target_sources(AmpexAnh PRIVATE anh.cpp)
target_include_directories(AmpexAnh PRIVATE ../Source)
target_compile_definitions(AmpexAnh PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(AmpexAnh PRIVATE juce::juce_dsp juce::juce_recommended_config_flags)
