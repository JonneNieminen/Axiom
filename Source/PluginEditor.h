#pragma once
#include <JuceHeader.h>
#include <optional>
#include "PluginProcessor.h"
#include "ToneMatchAnalyzer.h"

/** Flat, warm look: arc knobs, hairline-bordered controls, one accent. Colours come from the
    current palette (DesignTokens.h); call refreshColours() after switching theme. */
class AxiomLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AxiomLookAndFeel();
    void refreshColours();
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
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font getLabelFont (juce::Label& label) override { return label.getFont(); }
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

/** A row of mutually exclusive options (the amp voice, the theme). Optional per-option colour dots. */
class AxiomSegmented final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    AxiomSegmented (juce::StringArray options, std::vector<int> dotColourIndex = {});
    void setSelected (int index);
    int getSelected() const { return selected; }
    /** Fired when the user picks an option. */
    std::function<void (int)> onChange;
    /** Supplies the dot colour for option i (used when dot indices were given). */
    std::function<juce::Colour (int)> dotColour;
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hovered = -1; repaint(); }
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    int indexAt (juce::Point<int>) const;
    juce::StringArray names;
    std::vector<int> dots;
    int selected = 0, hovered = -1;
};

/** Sidebar tuner. Click to tune: the output mutes and the display shows the note and how
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

/** A small dialog drawn inside the editor (plugin hosts handle separate windows badly):
    a title, a message, an optional name field and Confirm / Cancel. */
class AxiomPromptOverlay final : public juce::Component
{
public:
    AxiomPromptOverlay();
    void show (const juce::String& title, const juce::String& message, const juce::String& confirmLabel,
               bool withField, const juce::String& fieldText, std::function<void (const juce::String&)> onConfirm);
    void dismiss();
    void applyTheme();
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    juce::Rectangle<int> card() const;
    void confirm();
    juce::Label title, message;
    juce::TextEditor field;
    juce::TextButton confirmButton, cancelButton { "Cancel" };
    bool hasField = false;
    std::function<void (const juce::String&)> callback;
};

class AxiomAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit AxiomAudioProcessorEditor (AxiomAudioProcessor&);
    ~AxiomAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int defaultWidth = 1120, defaultHeight = 760;

private:
    struct Layout
    {
        juce::Rectangle<float> sidebar, ampCard, inputCard, effectsCard, matchCard, meters;
        float contentX = 0, contentW = 0;
    };
    Layout computeLayout() const;

    void timerCallback() override;
    void configureSlider (juce::Slider&, juce::Label&, const juce::String&, const juce::String& tooltip, bool bipolar = false);
    void attach (juce::Slider&, const juce::String& parameterId, std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>&);
    void configureSectionLabel (juce::Label&, const juce::String&);
    void applyTheme();
    void drawLogo (juce::Graphics&, juce::Rectangle<float>) const;
    void drawWordmark (juce::Graphics&, juce::Rectangle<float>) const;
    void drawCard (juce::Graphics&, juce::Rectangle<float>, const juce::String& title, const juce::String& subtitle) const;
    void drawChips (juce::Graphics&, juce::Rectangle<float>) const;
    void drawLevelMeters (juce::Graphics&, juce::Rectangle<float>);
    void rebuildPresetMenu();
    void syncPresetSelection();
    void presetMenuChanged();
    void promptSavePreset();
    void promptDeletePreset();
    juce::String currentToneName() const;
    void chooseReferenceFile();
    void beginReferenceAnalysis (const juce::File&);
    void finishReferenceAnalysis (const AxiomToneSuggestion&, const juce::String& error);
    void applyToneSuggestion();

    AxiomAudioProcessor& audioProcessor;
    AxiomLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 650 };
    AxiomTunerDisplay tuner;
    AxiomPromptOverlay prompt;
    AxiomSegmented voiceSelector { { "Clean", "Classic", "Modern" }, { 0, 1, 2 } };
    AxiomSegmented themeSelector { { "Dark", "Light" } };
    juce::Array<juce::File> userPresets;
    static constexpr int userPresetBaseId = 1000, savePresetId = 900, deletePresetId = 901, revealPresetsId = 902;
    juce::Label presetLabel, voiceLabel, sourceLabel, tunerLabel, footnote;
    juce::Label cabinetCaption, referenceStatus;
    juce::Slider gain, bass, middle, treble, presence, master, input, gate, room, boost, chorus, delayMix, delayTime;
    juce::Label gainLabel, bassLabel, middleLabel, trebleLabel, presenceLabel, masterLabel, inputLabel, gateLabel, roomKnobLabel, boostLabel, chorusLabel, delayMixLabel, delayTimeLabel;
    juce::ComboBox preset, inputSource;
    juce::ToggleButton cabinet;
    juce::TextButton chooseReferenceButton { "Choose audio" }, applyToneButton { "Apply tone" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::ThreadPool analysisPool { 1 };
    std::optional<AxiomToneSuggestion> toneSuggestion;
    int shownVoice = -1;
    juce::String shownHeader;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> gainAttachment, bassAttachment, middleAttachment, trebleAttachment;
    std::unique_ptr<SliderAttachment> presenceAttachment, masterAttachment, inputAttachment, roomAttachment;
    std::unique_ptr<SliderAttachment> boostAttachment, chorusAttachment, delayMixAttachment, delayTimeAttachment, gateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttachment;
    std::unique_ptr<juce::ParameterAttachment> voiceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabinetAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessorEditor)
};
