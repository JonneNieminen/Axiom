#pragma once
#include <JuceHeader.h>
#include <limits>

class AxiomAudioProcessor final : public juce::AudioProcessor
{
public:
    AxiomAudioProcessor();
    ~AxiomAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float> outputPeak { 0.0f };
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    struct ChannelStrip
    {
        juce::dsp::IIR::Filter<float> inputHighPass;
        juce::dsp::IIR::Filter<float> bass;
        juce::dsp::IIR::Filter<float> mid;
        juce::dsp::IIR::Filter<float> treble;
        juce::dsp::IIR::Filter<float> presence;
        juce::dsp::IIR::Filter<float> cabHighPass;
        juce::dsp::IIR::Filter<float> cabResonance;
        juce::dsp::IIR::Filter<float> cabLowPass;
    };

    std::array<ChannelStrip, 2> strips;
    juce::Reverb reverb;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGain, ampGain, masterGain, roomMix;
    double currentSampleRate = 44100.0;
    float lastBass = std::numeric_limits<float>::quiet_NaN();
    float lastMiddle = std::numeric_limits<float>::quiet_NaN();
    float lastTreble = std::numeric_limits<float>::quiet_NaN();
    float lastPresence = std::numeric_limits<float>::quiet_NaN();
    void updateToneFilters (float bassDb, float midDb, float trebleDb, float presenceDb);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessor)
};
