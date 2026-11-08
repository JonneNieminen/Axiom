#pragma once
#include <JuceHeader.h>
#include <optional>
#include "PluginProcessor.h"
#include "ToneMatchAnalyzer.h"

class AxiomLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AxiomLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
};

/** Header tuner. Click to tune: the output mutes and the display shows the note and how
    many cents it is off. Pitch detection runs here on the message thread, never in the audio callback. */
class AxiomTunerDisplay final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    explicit AxiomTunerDisplay (AxiomAudioProcessor&);
    /** Called from the editor's timer. */
    void update();
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    bool keyPressed (const juce::KeyPress&) override;
    void toggle();

private:
    AxiomAudioProcessor& processor;
    std::vector<float> scratch;
    double smoothedHz = 0.0;
    int framesWithoutPitch = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomTunerDisplay)
};

class AxiomAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit AxiomAudioProcessorEditor (AxiomAudioProcessor&);
    ~AxiomAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void configureSlider (juce::Slider&, juce::Label&, const juce::String&, const juce::String& tooltip, bool bipolar = false);
    void attach (juce::Slider&, const juce::String& parameterId, std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>&);
    void configureSectionLabel (juce::Label&, const juce::String&);
    void drawBrandMark (juce::Graphics&, juce::Rectangle<float>);
    void drawOutputMeter (juce::Graphics&, juce::Rectangle<float>);
    juce::Rectangle<float> meterArea() const { return { (float) getWidth() - 240.0f, 38.0f, 188.0f, 48.0f }; }
    void chooseReferenceFile();
    void beginReferenceAnalysis (const juce::File&);
    void finishReferenceAnalysis (const AxiomToneSuggestion&, const juce::String& error);
    void applyToneSuggestion();

    AxiomAudioProcessor& audioProcessor;
    AxiomLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 650 };
    AxiomTunerDisplay tuner;
    juce::Label wordmark, descriptor, ampHeading, ampSubheading, voiceLabel;
    juce::Label presetLabel, sourceLabel;
    juce::Label cabinetCaption, toneMatchHeading, toneMatchDescription, referenceStatus;
    juce::Slider gain, bass, middle, treble, presence, master, input, gate, room, boost, chorus, delayMix, delayTime;
    juce::Label gainLabel, bassLabel, middleLabel, trebleLabel, presenceLabel, masterLabel, inputLabel, gateLabel, roomKnobLabel, boostLabel, chorusLabel, delayMixLabel, delayTimeLabel;
    juce::ComboBox voice, preset, inputSource;
    juce::ToggleButton cabinet;
    juce::TextButton chooseReferenceButton { "Choose audio" }, applyToneButton { "Apply tone" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::ThreadPool analysisPool { 1 };
    std::optional<AxiomToneSuggestion> toneSuggestion;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> gainAttachment, bassAttachment, middleAttachment, trebleAttachment;
    std::unique_ptr<SliderAttachment> presenceAttachment, masterAttachment, inputAttachment, roomAttachment;
    std::unique_ptr<SliderAttachment> boostAttachment, chorusAttachment, delayMixAttachment, delayTimeAttachment, gateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> voiceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessorEditor)
};
