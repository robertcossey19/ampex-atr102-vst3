# ============================================================
# Ampex (ANALOGEXACT) - CMakeLists.txt
# JUCE 8 plugin: VST3 + AU + Standalone, WebView UI
# ============================================================

cmake_minimum_required(VERSION 3.22)
project(Ampex VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------
# macOS: universal binary + signing
# Cubase 15 on Apple Silicon loads VST3 only (no VST2 in native mode).
# Deployment target 11.0: Cubase 15 itself needs macOS 14, and JUCE 8's
# WKWebView-based WebBrowserComponent is happiest on 11+.
# ------------------------------------------------------------
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build universal binary")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

    # "-" = ad-hoc signature. Fine for a plugin you build and run on your own Mac
    # (Apple Silicon refuses to load *unsigned* code, ad-hoc satisfies it).
    # For distribution set e.g. -DAMPEX_CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)"
    set(AMPEX_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity (\"-\" = ad-hoc)")
    set(AMPEX_DEVELOPMENT_TEAM ""  CACHE STRING "Apple Team ID (only for Developer ID builds)")
endif()

# ------------------------------------------------------------
# JUCE: use a local ./JUCE checkout if present, otherwise fetch 8.0.4
# ------------------------------------------------------------
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

# ------------------------------------------------------------
# Plugin target
# ------------------------------------------------------------
juce_add_plugin(${PROJECT_NAME}
    PLUGIN_MANUFACTURER_CODE MYPL
    PLUGIN_CODE Mypl

    PRODUCT_NAME "Ampex"
    COMPANY_NAME "Ampex"
    BUNDLE_ID "com.ampex.plugin"

    FORMATS AU VST3 Standalone

    # Linux only: link webkit2gtk/gtk for the WebView (no effect on macOS)
    NEEDS_WEB_BROWSER TRUE

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect

    # Needed for notarization; harmless for ad-hoc builds.
    HARDENED_RUNTIME_ENABLED TRUE

    # Drops the built .vst3 / .component straight into ~/Library/Audio/Plug-Ins/
    COPY_PLUGIN_AFTER_BUILD TRUE
)

# ------------------------------------------------------------
# Embedded web UI
# ------------------------------------------------------------
juce_add_binary_data(${PROJECT_NAME}Data
    HEADER_NAME "BinaryData.h"
    NAMESPACE BinaryData
    SOURCES
        web/index.html
)
# Binary data is linked into shared-library plugin bundles -> needs PIC on Linux
set_target_properties(${PROJECT_NAME}Data PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------
# Sources
# ------------------------------------------------------------
target_sources(${PROJECT_NAME}
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/TapeDSP.h
        Source/TapeCalibration.inc
        Source/PluginEditor.cpp
        Source/PluginEditor.h
)

target_include_directories(${PROJECT_NAME} PRIVATE Source)

target_compile_definitions(${PROJECT_NAME}
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${PROJECT_NAME}Data
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ------------------------------------------------------------
# macOS code signing (Xcode generator). build-mac.sh also re-signs
# explicitly after the build so Ninja/Makefile builds are covered too.
# ------------------------------------------------------------
if(APPLE)
    if(AMPEX_CODESIGN_IDENTITY STREQUAL "-")
        set(AMPEX_SIGN_FLAGS "--timestamp=none")      # ad-hoc: no Apple timestamp server
    else()
        set(AMPEX_SIGN_FLAGS "--timestamp")           # Developer ID: notarization needs a timestamp
    endif()
    foreach(tgt IN ITEMS ${PROJECT_NAME}_VST3 ${PROJECT_NAME}_AU ${PROJECT_NAME}_Standalone)
        if(TARGET ${tgt})
            set_target_properties(${tgt} PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${AMPEX_CODESIGN_IDENTITY}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${AMPEX_DEVELOPMENT_TEAM}"
                XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
                XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "${AMPEX_SIGN_FLAGS}"
            )
        endif()
    endforeach()
endif()
