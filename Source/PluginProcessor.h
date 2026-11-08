#pragma once
#include <JuceHeader.h>
#include "AxiomDsp.h"
#include "Presets.h"
#include <array>

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
    /** Peak of the guitar input after Input trim, i.e. what hits the preamp. */
    std::atomic<float> inputPeak { 0.0f };
    /** Interface theme, saved with the session (not a parameter, so it never touches automation). */
    std::atomic<bool> darkTheme { true };
    /** True once any control differs from the loaded preset. */
    std::atomic<bool> presetModified { false };
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Tuner: while active the input is captured for pitch detection and the output is muted. */
    std::atomic<bool> tunerActive { false };
    static constexpr int tunerBufferSize = 8192;
    /** Copies the newest `count` input samples (oldest first) into dest and returns the sample rate. */
    double readTunerInput (float* dest, int count) const;

    // User presets: one small XML file per tone in userPresetFolder(). Message thread only.
    static juce::File userPresetFolder();
    static constexpr const char* userPresetExtension = ".axiompreset";
    juce::Array<juce::File> getUserPresetFiles() const;
    /** Saves the current controls under `name` (replacing a preset of the same name). */
    juce::Result saveUserPreset (const juce::String& name);
    juce::Result loadUserPreset (const juce::File&);
    /** The loaded user preset's name, or empty when a factory preset (or nothing) is loaded. */
    juce::String getUserPresetName() const;

private:
    axiom::AxiomDsp dsp;
    std::atomic<int> currentPreset { 0 };
    axiom::AxiomParams readParameters();
    void parameterChanged (const juce::String&, float) override;
    std::atomic<bool> applyingPreset { false };
    juce::String userPresetName;
    mutable juce::CriticalSection userPresetLock;
    void setUserPresetName (const juce::String&);
    std::array<std::atomic<float>, tunerBufferSize> tunerBuffer {};
    std::atomic<int> tunerWrite { 0 };
    std::atomic<double> currentSampleRate { 44100.0 };
    float outputGain = 1.0f;  // ramps to 0 while the tuner mutes the output

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessor)
};
