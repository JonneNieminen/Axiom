#pragma once
#include <JuceHeader.h>
#include <functional>

struct AxiomToneSuggestion
{
    float gain = 0.0f;
    float bass = 0.0f;
    float middle = 0.0f;
    float treble = 0.0f;
    float presence = 0.0f;
    int voice = 0;
    juce::String sourceName;
    juce::String profileName;
};

struct AxiomToneAnalysisResult
{
    AxiomToneSuggestion suggestion;
    juce::String error;
};

class AxiomToneMatchAnalyzer final
{
public:
    static AxiomToneAnalysisResult analyzeFile (const juce::File&, const std::function<bool()>& shouldCancel = {});
};
