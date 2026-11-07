cmake_minimum_required(VERSION 3.22)
project(Axiom VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/JUCE" CACHE PATH "Path to a JUCE checkout (JUCE 9.0.5 or newer)")
if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE was not found at ${JUCE_DIR}. Clone JUCE 9.0.5 into third_party/JUCE or set -DJUCE_DIR=/path/to/JUCE")
endif()
add_subdirectory("${JUCE_DIR}" JUCE)

juce_add_plugin(Axiom
    COMPANY_NAME "Axiom Audio"
    BUNDLE_ID "audio.axiom.guitaramp"
    PLUGIN_MANUFACTURER_CODE AXIO
    PLUGIN_CODE AXM1
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Axiom"
    DESCRIPTION "A modern virtual guitar amplifier"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Axiom needs access to your audio interface input to process your guitar signal."
    ICON_BIG "${CMAKE_CURRENT_LIST_DIR}/assets/axiom-icon-1024.png"
    ICON_SMALL "${CMAKE_CURRENT_LIST_DIR}/assets/axiom-icon-512.png"
)

juce_generate_juce_header(Axiom)

target_sources(Axiom PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/ToneMatchAnalyzer.cpp
    Source/ToneMatchAnalyzer.h
    Source/DesignTokens.h
)

target_compile_definitions(Axiom PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=JucePlugin_Build_Standalone
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(Axiom PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
