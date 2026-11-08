#include "PluginEditor.h"
#include "DesignTokens.h"
#include "PitchDetector.h"

namespace
{
using namespace axiom::theme;
const juce::String dot = juce::String::fromUTF8 (" \xc2\xb7 ");
const auto& bg = background;
const auto& raised = surface;
const auto& elevated = surfaceHi;
const auto& strong = borderHi;

class ToneAnalysisJob final : public juce::ThreadPoolJob
{
public:
    using Callback = std::function<void (AxiomToneAnalysisResult)>;

    ToneAnalysisJob (juce::File audioFile, Callback completion)
        : juce::ThreadPoolJob ("Axiom reference tone analysis"), file (std::move (audioFile)), callback (std::move (completion))
    {
    }

    JobStatus runJob() override
    {
        auto result = AxiomToneMatchAnalyzer::analyzeFile (file, [this] { return shouldExit(); });
        if (! shouldExit())
            callback (std::move (result));
        return jobHasFinished;
    }

private:
    juce::File file;
    Callback callback;
};
}

AxiomLookAndFeel::AxiomLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, cream);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::textColourId, cream);
    setColour (juce::ComboBox::backgroundColourId, raised);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::arrowColourId, amber);
    setColour (juce::PopupMenu::backgroundColourId, raised);
    setColour (juce::PopupMenu::textColourId, cream);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, amberSoft);
    setColour (juce::PopupMenu::highlightedTextColourId, amber);
    setColour (juce::TooltipWindow::backgroundColourId, surfaceHi);
    setColour (juce::TooltipWindow::textColourId, cream);
    setColour (juce::TooltipWindow::outlineColourId, borderHi);
}

void AxiomLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                         float pos, float angleStart, float angleEnd, juce::Slider& slider)
{
    const float diameter = juce::jmin ((float) w, (float) h) - 10.0f;
    const float radius = diameter * 0.5f;
    const float cx = (float) x + (float) w * 0.5f;
    const float cy = (float) y + (float) h * 0.5f;
    const float angle = angleStart + pos * (angleEnd - angleStart);
    const auto knob = juce::Rectangle<float> (cx - radius * 0.77f, cy - radius * 0.77f,
                                               radius * 1.54f, radius * 1.54f);

    // Value arc: from the start, or from 0 dB for cut/boost controls.
    const float arcRadius = radius - 2.0f;
    const float thickness = juce::jlimit (2.5f, 4.0f, radius * 0.06f);
    const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path track;
    track.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f, angleStart, angleEnd, true);
    g.setColour (border);
    g.strokePath (track, stroke);
    float from = angleStart;
    if ((bool) slider.getProperties().getWithDefault ("bipolar", false))
        from = angleStart + (float) slider.valueToProportionOfLength (0.0) * (angleEnd - angleStart);
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (slider.isEnabled() ? amber : muted);
        g.strokePath (value, stroke);
    }

    g.setColour (juce::Colour (0x28000000));
    g.fillEllipse (knob.translated (0.0f, 2.0f));
    g.setColour (raised);
    g.fillEllipse (knob);
    g.setColour (border);
    g.drawEllipse (knob, 1.0f);

    const float pointerLength = radius * 0.55f;
    const juce::Point<float> pointerEnd (cx + std::cos (angle - juce::MathConstants<float>::halfPi) * pointerLength,
                                         cy + std::sin (angle - juce::MathConstants<float>::halfPi) * pointerLength);
    g.setColour (cream);
    g.drawLine (cx, cy, pointerEnd.x, pointerEnd.y, 2.0f);
    g.setColour (amber);
    g.fillEllipse (cx - 2.4f, cy - 2.4f, 4.8f, 4.8f);
}

void AxiomLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down,
                                     int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
    g.setColour (down ? elevated : raised);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (box.hasKeyboardFocus (true) ? amber : border);
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
    juce::Path arrow;
    const float cx = (float) w - 19.0f, cy = (float) h * 0.5f;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (amber);
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void AxiomLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                         bool highlighted, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();
    g.setColour (on ? amberSoft : raised);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (on || highlighted || down ? strong : border);
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
    g.setColour (on ? amber : muted);
    g.fillEllipse (r.getRight() - 32.0f, r.getCentreY() - 5.0f, 10.0f, 10.0f);
    if (on)
    {
        g.setColour (juce::Colour (0x40ffc857));
        g.fillEllipse (r.getRight() - 36.0f, r.getCentreY() - 9.0f, 18.0f, 18.0f);
        g.setColour (amber);
        g.fillEllipse (r.getRight() - 32.0f, r.getCentreY() - 5.0f, 10.0f, 10.0f);
    }
    g.setColour (cream);
    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.drawText (button.getButtonText(), r.withTrimmedLeft (16.0f).withTrimmedRight (43.0f).toNearestInt(), juce::Justification::centredLeft);
}

void AxiomLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                              const juce::Colour&, bool highlighted, bool down)
{
    const bool primary = button.getComponentID() == "apply-tone";
    auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = primary ? amber : raised;
    if (! button.isEnabled())
        fill = panel;
    else if (down)
        fill = primary ? juce::Colour (0xfff5b93a) : elevated;
    else if (highlighted)
        fill = primary ? juce::Colour (0xffffd27a) : elevated;
    g.setColour (fill);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (primary && button.isEnabled() ? amber : (highlighted ? strong : border));
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
}

void AxiomLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const bool primary = button.getComponentID() == "apply-tone";
    g.setColour (button.isEnabled() ? (primary ? navy : cream) : muted);
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText (button.getButtonText(), button.getLocalBounds().reduced (8, 0), juce::Justification::centred);
}

AxiomTunerDisplay::AxiomTunerDisplay (AxiomAudioProcessor& p) : processor (p)
{
    setTooltip ("Tuner. Click to tune: the output mutes while it is on. Click again to play.");
    setTitle ("Tuner");
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    scratch.resize ((size_t) AxiomAudioProcessor::tunerBufferSize);
}

void AxiomTunerDisplay::toggle()
{
    processor.tunerActive.store (! processor.tunerActive.load());
    smoothedHz = 0.0;
    framesWithoutPitch = 0;
    repaint();
}

void AxiomTunerDisplay::mouseUp (const juce::MouseEvent& e)
{
    if (getLocalBounds().contains (e.getPosition()))
        toggle();
}

bool AxiomTunerDisplay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey)
    {
        toggle();
        return true;
    }
    return false;
}

void AxiomTunerDisplay::update()
{
    if (! processor.tunerActive.load())
        return;

    double rate = processor.readTunerInput (scratch.data(), 0);
    const int count = juce::jmin (AxiomAudioProcessor::tunerBufferSize, axiom::pitchWindowFor (rate));
    rate = processor.readTunerInput (scratch.data(), count);
    const auto reading = axiom::detectPitch (scratch.data(), count, rate);
    if (reading.hz > 0.0 && reading.clarity > 0.85)
    {
        // Glide within a semitone so the needle settles; jump when a new string is played.
        if (smoothedHz > 0.0 && std::abs (12.0 * std::log2 (reading.hz / smoothedHz)) < 1.0)
            smoothedHz *= std::pow (reading.hz / smoothedHz, 0.4);
        else
            smoothedHz = reading.hz;
        framesWithoutPitch = 0;
    }
    else if (++framesWithoutPitch > 20)
    {
        smoothedHz = 0.0;
    }
    repaint();
}

void AxiomTunerDisplay::paint (juce::Graphics& g)
{
    const bool active = processor.tunerActive.load();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (active ? raised : panel);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (active ? amber : (isMouseOver() || hasKeyboardFocus (false) ? strong : border));
    g.drawRoundedRectangle (r, 12.0f, 1.0f);

    g.setColour (active ? amber : muted);
    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    g.drawText (active ? "TUNER  " + dot.trim() + "  OUTPUT MUTED" : juce::String ("TUNER"),
                (int) r.getX() + 12, (int) r.getY() + 7, (int) r.getWidth() - 24, 14, juce::Justification::centredLeft);

    const auto body = juce::Rectangle<float> (r.getX() + 12.0f, r.getY() + 22.0f, r.getWidth() - 24.0f, 18.0f);
    if (! active)
    {
        g.setColour (secondary);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("Click to tune", body.toNearestInt(), juce::Justification::centredLeft);
        return;
    }
    if (smoothedHz <= 0.0)
    {
        g.setColour (secondary);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("Play one string", body.toNearestInt(), juce::Justification::centredLeft);
        return;
    }

    const auto note = axiom::noteFor (smoothedHz);
    const bool inTune = std::abs (note.cents) < 3.0;
    g.setColour (inTune ? amber : cream);
    g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    g.drawText (juce::String (axiom::noteName (note.midi)) + juce::String (note.midi / 12 - 1),
                body.withWidth (40.0f).translated (0.0f, -1.0f).toNearestInt(), juce::Justification::centredLeft);

    // Cents scale: centre tick, a dot for the offset and the number on the right.
    const auto scale = juce::Rectangle<float> (body.getX() + 44.0f, body.getCentreY(), body.getWidth() - 44.0f - 38.0f, 0.0f);
    g.setColour (border);
    g.drawLine (scale.getX(), scale.getY(), scale.getRight(), scale.getY(), 2.0f);
    g.setColour (secondary);
    g.drawLine (scale.getCentreX(), scale.getY() - 5.0f, scale.getCentreX(), scale.getY() + 5.0f, 1.5f);
    const float px = scale.getCentreX() + (float) juce::jlimit (-50.0, 50.0, note.cents) / 50.0f * scale.getWidth() * 0.5f;
    g.setColour (inTune ? amber : cream);
    g.fillEllipse (px - 4.0f, scale.getY() - 4.0f, 8.0f, 8.0f);
    g.setColour (secondary);
    g.setFont (juce::Font (juce::FontOptions (10.0f)));
    const int cents = juce::roundToInt (note.cents);
    g.drawText ((cents > 0 ? "+" : "") + juce::String (cents) + juce::String::fromUTF8 (" \xc2\xa2"),
                juce::Rectangle<float> (body.getRight() - 36.0f, body.getY(), 36.0f, body.getHeight()).toNearestInt(),
                juce::Justification::centredRight);
}

AxiomAudioProcessorEditor::AxiomAudioProcessorEditor (AxiomAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), tuner (p)
{
    setLookAndFeel (&lookAndFeel);
    setSize (1080, 820);
    setResizable (true, true);
    setResizeLimits (1060, 820, 1440, 1000);

    wordmark.setText ("axiom", juce::dontSendNotification);
    wordmark.setFont (juce::Font (juce::FontOptions (29.0f, juce::Font::bold)));
    wordmark.setColour (juce::Label::textColourId, cream);
    wordmark.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (wordmark);

    descriptor.setText ("GUITAR AMPLIFIER", juce::dontSendNotification);
    descriptor.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    descriptor.setColour (juce::Label::textColourId, muted);
    descriptor.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (descriptor);

    ampHeading.setText ("Amplifier", juce::dontSendNotification);
    ampHeading.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    ampHeading.setColour (juce::Label::textColourId, cream);
    ampHeading.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (ampHeading);

    ampSubheading.setText ("A responsive amp voice for the way you play.", juce::dontSendNotification);
    ampSubheading.setFont (juce::Font (juce::FontOptions (12.0f)));
    ampSubheading.setColour (juce::Label::textColourId, secondary);
    ampSubheading.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (ampSubheading);

    configureSlider (gain, gainLabel, "GAIN", "How hard the preamp is driven, from clean to saturated.");
    configureSlider (bass, bassLabel, "BASS", "Low end. Cut it for tighter high-gain rhythm.", true);
    configureSlider (middle, middleLabel, "MIDDLE", "Body and punch. Boost it to cut through a band.", true);
    configureSlider (treble, trebleLabel, "TREBLE", "Edge and pick attack.", true);
    configureSlider (presence, presenceLabel, "PRESENCE", "Upper harmonics after the power amp: air and bite.", true);
    configureSlider (master, masterLabel, "MASTER", "Output level after the amp and cabinet.");
    configureSlider (input, inputLabel, "INPUT TRIM", "Level hitting the preamp. Raise it if your interface is quiet.", true);
    configureSlider (gate, gateLabel, "GATE", "Noise gate. Silences hum and hiss between notes. Lower it if note tails get cut; all the way down is off.");
    configureSlider (room, roomKnobLabel, "ROOM", "A small stereo room reverb.");

    configureSlider (boost, boostLabel, "BOOST", "A mid-focused overdrive in front of the amp, for leads.");
    configureSlider (chorus, chorusLabel, "CHORUS", "Stereo chorus after the cabinet.");
    configureSlider (delayMix, delayMixLabel, "DELAY", "Level of the darkening stereo delay.");
    configureSlider (delayTime, delayTimeLabel, "DELAY TIME", "Delay time, 40 ms to 1 s.");

    addAndMakeVisible (tuner);

    configureSectionLabel (presetLabel, "TONE PRESET");
    addAndMakeVisible (presetLabel);
    for (int i = 0; i < AxiomAudioProcessor::numPresets; ++i)
        preset.addItem (axiom::factoryPresets[(size_t) i].name, i + 1);
    preset.setTextWhenNothingSelected ("Custom");
    preset.setSelectedId (audioProcessor.presetModified.load() ? 0 : audioProcessor.getCurrentProgram() + 1, juce::dontSendNotification);
    preset.setJustificationType (juce::Justification::centredLeft);
    preset.setTooltip ("Load a starting tone. It sets the voice, gain, EQ and effects; every control stays adjustable.");
    preset.onChange = [this]
    {
        if (preset.getSelectedId() > 0)
            audioProcessor.setCurrentProgram (preset.getSelectedId() - 1);
    };
    addAndMakeVisible (preset);

    configureSectionLabel (sourceLabel, "INPUT");
    addAndMakeVisible (sourceLabel);
    inputSource.addItemList ({ "Left", "Right", "Left + Right" }, 1);
    inputSource.setJustificationType (juce::Justification::centredLeft);
    inputSource.setTooltip ("Which input channel carries your guitar. The amp is mono and plays through both ears.");
    addAndMakeVisible (inputSource);

    configureSectionLabel (voiceLabel, "AMP VOICE");
    addAndMakeVisible (voiceLabel);
    voice.addItemList ({ "Clean", "Classic", "Modern" }, 1);
    voice.setSelectedId (2, juce::dontSendNotification);
    voice.setJustificationType (juce::Justification::centredLeft);
    voice.setTextWhenNothingSelected ("Choose a voice");
    voice.setTooltip ("Clean: glassy, lots of headroom. Classic: plexi-style crunch. Modern: tight, hot-rodded high gain.");
    addAndMakeVisible (voice);

    cabinet.setButtonText ("CABINET SIM");
    cabinet.setClickingTogglesState (true);
    cabinet.setTooltip ("The built-in 2x12 speaker response. Turn it off to use your own cabinet impulse response after Axiom.");
    addAndMakeVisible (cabinet);
    cabinetCaption.setText (juce::String::fromUTF8 ("2\xc3\x97" "12 \xc2\xb7 studio cabinet"), juce::dontSendNotification);
    cabinetCaption.setFont (juce::Font (juce::FontOptions (10.0f)));
    cabinetCaption.setColour (juce::Label::textColourId, muted);
    cabinetCaption.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (cabinetCaption);

    configureSectionLabel (toneMatchHeading, "TONE MATCH");
    addAndMakeVisible (toneMatchHeading);
    toneMatchDescription.setText ("Analyze a guitar clip to suggest an amp voice and EQ.", juce::dontSendNotification);
    toneMatchDescription.setFont (juce::Font (juce::FontOptions (11.0f)));
    toneMatchDescription.setColour (juce::Label::textColourId, secondary);
    toneMatchDescription.setJustificationType (juce::Justification::centredLeft);
    toneMatchDescription.setMinimumHorizontalScale (0.9f);
    addAndMakeVisible (toneMatchDescription);

    referenceStatus.setText ("No reference selected\nWAV, AIFF, FLAC, OGG; MP3, M4A, AAC on macOS.", juce::dontSendNotification);
    referenceStatus.setFont (juce::Font (juce::FontOptions (12.0f)));
    referenceStatus.setColour (juce::Label::textColourId, secondary);
    referenceStatus.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (referenceStatus);

    chooseReferenceButton.setComponentID ("choose-reference");
    chooseReferenceButton.setTooltip ("Choose a short audio file to analyze as a guitar tone reference.");
    chooseReferenceButton.onClick = [this] { chooseReferenceFile(); };
    addAndMakeVisible (chooseReferenceButton);

    applyToneButton.setComponentID ("apply-tone");
    applyToneButton.setTooltip ("Apply the suggested amp voice, gain, and EQ. Other controls stay as they are.");
    applyToneButton.onClick = [this] { applyToneSuggestion(); };
    applyToneButton.setEnabled (false);
    addAndMakeVisible (applyToneButton);

    auto& state = p.parameters;
    attach (gain, "gain", gainAttachment);
    attach (bass, "bass", bassAttachment);
    attach (middle, "mid", middleAttachment);
    attach (treble, "treble", trebleAttachment);
    attach (presence, "presence", presenceAttachment);
    attach (master, "master", masterAttachment);
    attach (input, "input", inputAttachment);
    attach (gate, "gate", gateAttachment);
    attach (room, "room", roomAttachment);
    voiceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "voice", voice);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, "cabinet", cabinet);
    attach (boost, "boost", boostAttachment);
    attach (chorus, "chorus", chorusAttachment);
    attach (delayMix, "delayMix", delayMixAttachment);
    attach (delayTime, "delayTime", delayTimeAttachment);
    sourceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "inputSource", inputSource);
    startTimerHz (30);
}

AxiomAudioProcessorEditor::~AxiomAudioProcessorEditor()
{
    stopTimer();
    // Never leave the output muted once the window that shows the tuner is gone.
    audioProcessor.tunerActive.store (false);
    analysisPool.removeAllJobs (true, -1);
    setLookAndFeel (nullptr);
}

void AxiomAudioProcessorEditor::attach (juce::Slider& slider, const juce::String& parameterId,
                                        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
{
    attachment = std::make_unique<SliderAttachment> (audioProcessor.parameters, parameterId, slider);
    if (auto* parameter = audioProcessor.parameters.getParameter (parameterId))
        slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
}

void AxiomAudioProcessorEditor::configureSlider (juce::Slider& slider, juce::Label& label, const juce::String& text,
                                                 const juce::String& tooltip, bool bipolar)
{
    slider.getProperties().set ("bipolar", bipolar);
    slider.setTooltip (tooltip + " Double-click to reset.");
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 21);
    slider.setColour (juce::Slider::textBoxTextColourId, secondary);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::rotarySliderFillColourId, amber);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, raised);
    slider.setScrollWheelEnabled (false);
    addAndMakeVisible (slider);

    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    label.setColour (juce::Label::textColourId, secondary);
    label.setJustificationType (juce::Justification::centred);
    label.setTooltip (slider.getTooltip());
    addAndMakeVisible (label);
}

void AxiomAudioProcessorEditor::configureSectionLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    label.setColour (juce::Label::textColourId, muted);
    label.setJustificationType (juce::Justification::centredLeft);
}

void AxiomAudioProcessorEditor::drawBrandMark (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto centre = r.getCentre();
    g.setColour (raised);
    g.fillRoundedRectangle (r, 13.0f);
    g.setColour (border);
    g.drawRoundedRectangle (r, 13.0f, 1.0f);
    g.setColour (strong);
    g.drawEllipse (r.reduced (8.0f), 1.5f);
    juce::Path ampA;
    ampA.startNewSubPath (centre.x - 8.0f, centre.y + 8.0f);
    ampA.lineTo (centre.x, centre.y - 9.0f);
    ampA.lineTo (centre.x + 8.0f, centre.y + 8.0f);
    ampA.startNewSubPath (centre.x - 4.0f, centre.y + 1.0f);
    ampA.lineTo (centre.x + 4.0f, centre.y + 1.0f);
    g.setColour (cream);
    g.strokePath (ampA, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void AxiomAudioProcessorEditor::drawOutputMeter (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (panel);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (border);
    g.drawRoundedRectangle (r, 12.0f, 1.0f);
    g.setColour (muted);
    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    g.drawText ("OUTPUT", (int) r.getX() + 12, (int) r.getY() + 7, 60, 14, juce::Justification::centredLeft);

    const float peak = audioProcessor.outputPeak.load (std::memory_order_relaxed);
    const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
    const float level = juce::jlimit (0.0f, 1.0f, (db + 42.0f) / 42.0f);
    const auto bar = juce::Rectangle<float> (r.getX() + 12.0f, r.getY() + 27.0f, r.getWidth() - 48.0f, 8.0f);
    g.setColour (raised);
    g.fillRoundedRectangle (bar, 4.0f);
    auto activeBar = bar.withWidth (bar.getWidth() * level);
    g.setColour (amber);
    g.fillRoundedRectangle (activeBar, 4.0f);
    if (level > 0.02f)
    {
        g.setColour (juce::Colour (0x40ffc857));
        g.fillRoundedRectangle (activeBar.expanded (2.0f, 3.0f), 5.0f);
        g.setColour (amber);
        g.fillRoundedRectangle (activeBar, 4.0f);
    }
    g.setColour (secondary);
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    g.drawText (juce::String (db <= -59.5f ? -60.0f : db, 0) + " dB",
                (int) r.getRight() - 39, (int) r.getY() + 23, 33, 16, juce::Justification::centredRight);
}

void AxiomAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    auto outer = getLocalBounds().toFloat().reduced (20.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (outer, 20.0f);
    g.setColour (border);
    g.drawRoundedRectangle (outer, 20.0f, 1.0f);

    drawBrandMark (g, { 40.0f, 36.0f, 48.0f, 48.0f });

    auto ampCard = juce::Rectangle<float> (36.0f, 118.0f, (float) getWidth() - 72.0f, 328.0f);
    g.setColour (raised);
    g.fillRoundedRectangle (ampCard, 16.0f);
    g.setColour (border);
    g.drawRoundedRectangle (ampCard, 16.0f, 1.0f);

    g.setColour (amberSoft);
    g.fillRoundedRectangle (ampCard.getX() + 18.0f, ampCard.getY() + 18.0f, 3.0f, 35.0f, 1.5f);
    g.setColour (border);
    g.drawLine (ampCard.getX() + 24.0f, ampCard.getY() + 65.0f, ampCard.getRight() - 24.0f, ampCard.getY() + 65.0f, 1.0f);
    drawOutputMeter (g, meterArea());

    auto controlsCard = juce::Rectangle<float> (36.0f, 462.0f, (float) getWidth() - 72.0f, 150.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (controlsCard, 16.0f);
    g.setColour (border);
    g.drawRoundedRectangle (controlsCard, 16.0f, 1.0f);

    auto matchCard = juce::Rectangle<float> (36.0f, 626.0f, (float) getWidth() - 72.0f, (float) getHeight() - 686.0f);
    g.setColour (raised);
    g.fillRoundedRectangle (matchCard, 16.0f);
    g.setColour (border);
    g.drawRoundedRectangle (matchCard, 16.0f, 1.0f);
    g.setColour (amberSoft);
    g.fillRoundedRectangle (matchCard.getX() + 18.0f, matchCard.getY() + 17.0f, 3.0f, matchCard.getHeight() - 34.0f, 1.5f);

    g.setColour (border);
    g.drawLine (54.0f, (float) getHeight() - 47.0f, (float) getWidth() - 54.0f, (float) getHeight() - 47.0f, 1.0f);
    g.setColour (muted);
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    g.drawText (juce::String::fromUTF8 ("AXIOM  \xc2\xb7  ANALOG-INSPIRED SIGNAL PATH"), 52, getHeight() - 42, 300, 16, juce::Justification::centredLeft);
    g.drawText (juce::String::fromUTF8 ("STEREO  \xc2\xb7  REAL-TIME"), getWidth() - 210, getHeight() - 42, 158, 16, juce::Justification::centredRight);
}

void AxiomAudioProcessorEditor::resized()
{
    const int w = getWidth();
    wordmark.setBounds (100, 37, 170, 32);
    descriptor.setBounds (101, 66, 200, 16);
    ampHeading.setBounds (62, 133, 180, 24);
    ampSubheading.setBounds (62, 156, 390, 18);

    const int cardX = 56;
    const int cardWidth = w - 112;
    const int gap = juce::jmax (8, (cardWidth - 6 * 136) / 5);
    const int controlWidth = juce::jmin (136, (cardWidth - 5 * gap) / 6);
    juce::Slider* sliders[] = { &gain, &bass, &middle, &treble, &presence, &master };
    juce::Label* labels[] = { &gainLabel, &bassLabel, &middleLabel, &trebleLabel, &presenceLabel, &masterLabel };
    for (int i = 0; i < 6; ++i)
    {
        const int x = cardX + i * (controlWidth + gap);
        labels[i]->setBounds (x, 194, controlWidth, 18);
        sliders[i]->setBounds (x + 3, 215, controlWidth - 6, 202);
    }

    presetLabel.setBounds (w - 540, 33, 200, 14);
    preset.setBounds (w - 540, 49, 276, 36);
    tuner.setBounds (w - 744, 38, 188, 48);

    voiceLabel.setBounds (58, 474, 170, 14);
    voice.setBounds (58, 491, 190, 34);
    sourceLabel.setBounds (58, 538, 170, 14);
    inputSource.setBounds (58, 555, 190, 34);

    juce::Slider* extras[] = { &input, &gate, &boost, &chorus, &delayMix, &delayTime, &room };
    juce::Label* extraLabels[] = { &inputLabel, &gateLabel, &boostLabel, &chorusLabel, &delayMixLabel, &delayTimeLabel, &roomKnobLabel };
    const int extrasX = 264, cabinetX = w - 232;
    const int pitch = juce::jmin (92, (cabinetX - 16 - extrasX) / 7);
    for (int i = 0; i < 7; ++i)
    {
        const int x = extrasX + i * pitch;
        extraLabels[i]->setBounds (x, 474, pitch - 2, 14);
        extras[i]->setBounds (x, 491, pitch - 2, 117);
    }
    cabinet.setBounds (cabinetX, 496, 176, 44);
    cabinetCaption.setBounds (cabinetX + 11, 548, 165, 16);

    toneMatchHeading.setBounds (58, 644, 220, 18);
    toneMatchDescription.setBounds (58, 667, 270, 44);
    referenceStatus.setBounds (350, 645, w - 350 - 345, 76);
    chooseReferenceButton.setBounds (w - 326, 656, 142, 46);
    applyToneButton.setBounds (w - 173, 656, 118, 46);
}

void AxiomAudioProcessorEditor::chooseReferenceFile()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Choose a guitar tone reference", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.aac", true);
    auto safeEditor = juce::Component::SafePointer<AxiomAudioProcessorEditor> (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeEditor] (const juce::FileChooser& chooser)
    {
        if (safeEditor != nullptr)
        {
            const auto file = chooser.getResult();
            if (file.existsAsFile())
                safeEditor->beginReferenceAnalysis (file);
        }
    });
}

void AxiomAudioProcessorEditor::beginReferenceAnalysis (const juce::File& file)
{
    toneSuggestion.reset();
    applyToneButton.setEnabled (false);
    chooseReferenceButton.setEnabled (false);
    referenceStatus.setColour (juce::Label::textColourId, secondary);
    chooseReferenceButton.setButtonText (juce::String::fromUTF8 ("Analyzing\xe2\x80\xa6"));
    referenceStatus.setText ("Reading " + file.getFileName() + "\n" + juce::String::fromUTF8 ("Analyzing up to 15 seconds of audio\xe2\x80\xa6"), juce::dontSendNotification);

    auto safeEditor = juce::Component::SafePointer<AxiomAudioProcessorEditor> (this);
    analysisPool.addJob (new ToneAnalysisJob (file, [safeEditor] (AxiomToneAnalysisResult result)
    {
        juce::MessageManager::callAsync ([safeEditor, analysis = std::move (result)]() mutable
        {
            if (safeEditor != nullptr)
                safeEditor->finishReferenceAnalysis (analysis.suggestion, analysis.error);
        });
    }), true);
}

void AxiomAudioProcessorEditor::finishReferenceAnalysis (const AxiomToneSuggestion& suggestion, const juce::String& error)
{
    chooseReferenceButton.setEnabled (true);
    chooseReferenceButton.setButtonText ("Choose audio");
    if (error.isNotEmpty())
    {
        toneSuggestion.reset();
        applyToneButton.setEnabled (false);
        referenceStatus.setColour (juce::Label::textColourId, juce::Colour (0xffff9f43));
        referenceStatus.setText (error, juce::dontSendNotification);
        return;
    }

    toneSuggestion = suggestion;
    applyToneButton.setEnabled (true);
    referenceStatus.setColour (juce::Label::textColourId, secondary);
    const juce::String voices[] { "Clean", "Classic", "Modern" };
    referenceStatus.setText (suggestion.sourceName + "\n" + suggestion.profileName + dot + voices[juce::jlimit (0, 2, suggestion.voice)] + " voice suggested",
                             juce::dontSendNotification);
}

void AxiomAudioProcessorEditor::applyToneSuggestion()
{
    if (! toneSuggestion.has_value())
        return;

    const auto setFloat = [this] (const juce::String& id, float value)
    {
        if (auto* parameter = audioProcessor.parameters.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    setFloat ("gain", toneSuggestion->gain);
    setFloat ("bass", toneSuggestion->bass);
    setFloat ("mid", toneSuggestion->middle);
    setFloat ("treble", toneSuggestion->treble);
    setFloat ("presence", toneSuggestion->presence);
    if (auto* parameter = audioProcessor.parameters.getParameter ("voice"))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) toneSuggestion->voice));

    const juce::String voices[] { "Clean", "Classic", "Modern" };
    referenceStatus.setText (toneSuggestion->sourceName + "\nApplied" + dot + toneSuggestion->profileName + dot + voices[juce::jlimit (0, 2, toneSuggestion->voice)],
                             juce::dontSendNotification);
}

void AxiomAudioProcessorEditor::timerCallback()
{
    const int selected = audioProcessor.presetModified.load() ? 0 : audioProcessor.getCurrentProgram() + 1;
    if (preset.getSelectedId() != selected)
        preset.setSelectedId (selected, juce::dontSendNotification);
    tuner.update();
    repaint (meterArea().expanded (2.0f).toNearestInt());
}
