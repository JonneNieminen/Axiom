#pragma once
#include <JuceHeader.h>
#include "AxiomDsp.h"
#include "Presets.h"

class AxiomAudioProcessor final : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener
{
public:
    AxiomAudioProcessor();
    ~AxiomAudioProcessor() override;
    static constexpr int numPresets = (int) axiom::factoryPresets.size();

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
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return numPresets; }
    int getCurrentProgram() override { return currentPreset.load(); }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float> outputPeak { 0.0f };
    /** True once any control differs from the loaded preset. */
    std::atomic<bool> presetModified { false };
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    axiom::AxiomDsp dsp;
    std::atomic<int> currentPreset { 0 };
    axiom::AxiomParams readParameters();
    void parameterChanged (const juce::String&, float) override;
    bool applyingPreset = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessor)
};
