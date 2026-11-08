#include "PluginEditor.h"
#include "DesignTokens.h"
#include "PitchDetector.h"

namespace
{
using axiom::theme::current;
const juce::String dot = juce::String::fromUTF8 (" \xc2\xb7 ");
const juce::Colour brandEmber { 0xfff08a3c }, brandCream { 0xfff2ece3 };

juce::Font fontOf (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

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

// ─── Look and feel ──────────────────────────────────────────────────────────

AxiomLookAndFeel::AxiomLookAndFeel() { refreshColours(); }

void AxiomLookAndFeel::refreshColours()
{
    const auto& t = current();
    setColour (juce::Slider::textBoxTextColourId, t.muted);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, t.accentSoft);
    setColour (juce::ComboBox::textColourId, t.text);
    setColour (juce::ComboBox::backgroundColourId, t.elevated);
    setColour (juce::ComboBox::outlineColourId, t.border);
    setColour (juce::ComboBox::arrowColourId, t.muted);
    setColour (juce::PopupMenu::backgroundColourId, t.surface);
    setColour (juce::PopupMenu::textColourId, t.text);
    setColour (juce::PopupMenu::headerTextColourId, t.faint);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, t.accentSoft);
    setColour (juce::PopupMenu::highlightedTextColourId, t.accent);
    setColour (juce::TextEditor::backgroundColourId, t.elevated);
    setColour (juce::TextEditor::textColourId, t.text);
    setColour (juce::TextEditor::outlineColourId, t.border);
    setColour (juce::TextEditor::focusedOutlineColourId, t.accent);
    setColour (juce::TextEditor::highlightColourId, t.accentSoft);
    setColour (juce::TextEditor::highlightedTextColourId, t.text);
    setColour (juce::CaretComponent::caretColourId, t.accent);
    setColour (juce::TooltipWindow::backgroundColourId, t.tooltipBackground);
    setColour (juce::TooltipWindow::textColourId, t.tooltipText);
    setColour (juce::TooltipWindow::outlineColourId, juce::Colours::transparentBlack);
}

void AxiomLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                         float pos, float angleStart, float angleEnd, juce::Slider& slider)
{
    const auto& t = current();
    const float diameter = juce::jmin ((float) w, (float) h) - 8.0f;
    const float radius = diameter * 0.5f;
    const float cx = (float) x + (float) w * 0.5f;
    const float cy = (float) y + (float) h * 0.5f;
    const float angle = angleStart + pos * (angleEnd - angleStart);
    const float arcRadius = radius - 3.0f;
    const float thickness = juce::jlimit (3.5f, 6.0f, radius * 0.11f);
    const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f, angleStart, angleEnd, true);
    g.setColour (t.pressed);
    g.strokePath (track, stroke);

    // Value arc: from the start, or from 0 dB for cut/boost controls.
    float from = angleStart;
    if ((bool) slider.getProperties().getWithDefault ("bipolar", false))
        from = angleStart + (float) slider.valueToProportionOfLength (0.0) * (angleEnd - angleStart);
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (value, stroke);
    }

    // Flat cap with a pointer: no gradients, no glow.
    const float body = arcRadius - thickness - 4.0f;
    g.setColour (t.elevated);
    g.fillEllipse (cx - body, cy - body, body * 2.0f, body * 2.0f);
    g.setColour (t.borderStrong);
    g.drawEllipse (cx - body, cy - body, body * 2.0f, body * 2.0f, 1.0f);

    const auto dir = juce::Point<float> (std::cos (angle - juce::MathConstants<float>::halfPi), std::sin (angle - juce::MathConstants<float>::halfPi));
    g.setColour (t.text);
    g.drawLine (juce::Line<float> (cx + dir.x * body * 0.38f, cy + dir.y * body * 0.38f, cx + dir.x * body * 0.86f, cy + dir.y * body * 0.86f),
                juce::jlimit (2.0f, 3.2f, body * 0.07f));
    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (t.accent.withAlpha (0.55f));
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.2f);
    }
}

void AxiomLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down,
                                     int, int, int, int, juce::ComboBox& box)
{
    const auto& t = current();
    const auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
    g.setColour (down ? t.pressed : t.elevated);
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (box.hasKeyboardFocus (true) ? t.accent : t.border);
    g.drawRoundedRectangle (r, 9.0f, 1.0f);
    juce::Path arrow;
    const float cx = (float) w - 18.0f, cy = (float) h * 0.5f;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (t.muted);
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void AxiomLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    const auto& t = current();
    const auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f);
    g.fillAll (t.background);
    g.setColour (t.surface);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (t.borderStrong);
    g.drawRoundedRectangle (r, 8.0f, 1.0f);
}

void AxiomLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // Drawn as a switch: the label on the left, a pill on the right.
    const auto& t = current();
    const auto b = button.getLocalBounds().toFloat();
    const bool on = button.getToggleState();
    const auto pill = juce::Rectangle<float> (b.getRight() - 38.0f, b.getCentreY() - 11.0f, 38.0f, 22.0f);
    g.setColour (on ? (highlighted ? t.accentHover : t.accent) : (highlighted ? t.borderStrong : t.pressed));
    g.fillRoundedRectangle (pill, 11.0f);
    g.setColour (juce::Colours::white.withAlpha (on ? 1.0f : 0.9f));
    g.fillEllipse (pill.getX() + (on ? 18.0f : 2.0f), pill.getY() + 2.0f, 18.0f, 18.0f);
    if (button.hasKeyboardFocus (true))
    {
        g.setColour (t.accent.withAlpha (0.6f));
        g.drawRoundedRectangle (pill.expanded (2.0f), 13.0f, 1.2f);
    }
    g.setColour (t.text);
    g.setFont (fontOf (13.0f));
    g.drawText (button.getButtonText(), b.withTrimmedRight (48.0f).toNearestInt(), juce::Justification::centredLeft);
}

void AxiomLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                              const juce::Colour&, bool highlighted, bool down)
{
    const auto& t = current();
    const bool primary = (bool) button.getProperties().getWithDefault ("primary", false);
    const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = primary ? t.accent : t.elevated;
    if (! button.isEnabled())
        fill = t.pressed.withAlpha (0.5f);
    else if (down)
        fill = primary ? t.accentHover.darker (0.1f) : t.pressed;
    else if (highlighted)
        fill = primary ? t.accentHover : t.pressed;
    g.setColour (fill);
    g.fillRoundedRectangle (r, 9.0f);
    if (! primary || ! button.isEnabled())
    {
        g.setColour (highlighted && button.isEnabled() ? t.borderStrong : t.border);
        g.drawRoundedRectangle (r, 9.0f, 1.0f);
    }
}

void AxiomLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const auto& t = current();
    const bool primary = (bool) button.getProperties().getWithDefault ("primary", false);
    g.setColour (! button.isEnabled() ? t.faint : (primary ? t.accentText : t.text));
    g.setFont (fontOf (13.0f, true));
    g.drawText (button.getButtonText(), button.getLocalBounds().reduced (8, 0), juce::Justification::centred);
}

void AxiomLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    const auto& t = current();
    g.setColour ((over || dragging ? t.muted : t.faint).withAlpha (0.8f));
    for (int i = 1; i <= 3; ++i)
    {
        const float o = (float) i * 4.0f;
        g.drawLine ((float) w - o, (float) h - 2.0f, (float) w - 2.0f, (float) h - o, 1.2f);
    }
}

// ─── Segmented control ──────────────────────────────────────────────────────

AxiomSegmented::AxiomSegmented (juce::StringArray options, std::vector<int> dotColourIndex)
    : names (std::move (options)), dots (std::move (dotColourIndex))
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    dotColour = [] (int i) { return current().voice (i); };
}

void AxiomSegmented::setSelected (int index)
{
    index = juce::jlimit (0, names.size() - 1, index);
    if (index != selected)
    {
        selected = index;
        repaint();
    }
}

int AxiomSegmented::indexAt (juce::Point<int> p) const
{
    if (names.isEmpty() || getWidth() <= 0)
        return -1;
    return juce::jlimit (0, names.size() - 1, p.x * names.size() / getWidth());
}

void AxiomSegmented::mouseMove (const juce::MouseEvent& e)
{
    const int i = indexAt (e.getPosition());
    if (i != hovered)
    {
        hovered = i;
        repaint();
    }
}

void AxiomSegmented::mouseUp (const juce::MouseEvent& e)
{
    if (! getLocalBounds().contains (e.getPosition()))
        return;
    const int i = indexAt (e.getPosition());
    if (i >= 0 && i != selected)
    {
        selected = i;
        repaint();
        if (onChange)
            onChange (i);
    }
}

bool AxiomSegmented::keyPressed (const juce::KeyPress& key)
{
    const int step = key == juce::KeyPress::leftKey ? -1 : (key == juce::KeyPress::rightKey ? 1 : 0);
    if (step == 0)
        return false;
    const int next = juce::jlimit (0, names.size() - 1, selected + step);
    if (next != selected)
    {
        selected = next;
        repaint();
        if (onChange)
            onChange (next);
    }
    return true;
}

void AxiomSegmented::paint (juce::Graphics& g)
{
    const auto& t = current();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (t.dark ? t.background : t.pressed);
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (hasKeyboardFocus (true) ? t.accent : t.border);
    g.drawRoundedRectangle (r, 9.0f, 1.0f);

    const float segment = r.getWidth() / (float) juce::jmax (1, names.size());
    for (int i = 0; i < names.size(); ++i)
    {
        const auto cell = juce::Rectangle<float> (r.getX() + segment * (float) i, r.getY(), segment, r.getHeight()).reduced (3.0f);
        const bool on = i == selected;
        if (on)
        {
            g.setColour (t.dark ? t.elevated : t.surface);
            g.fillRoundedRectangle (cell, 7.0f);
            g.setColour (t.borderStrong);
            g.drawRoundedRectangle (cell, 7.0f, 1.0f);
        }
        else if (i == hovered)
        {
            g.setColour (t.text.withAlpha (0.05f));
            g.fillRoundedRectangle (cell, 7.0f);
        }
        auto text = cell;
        if (! dots.empty() && (size_t) i < dots.size())
        {
            g.setColour (dotColour (dots[(size_t) i]).withAlpha (on ? 1.0f : 0.55f));
            g.fillEllipse (cell.getX() + 7.0f, cell.getCentreY() - 3.0f, 6.0f, 6.0f);
            text = text.withTrimmedLeft (12.0f);
        }
        g.setColour (on ? t.text : t.muted);
        g.setFont (fontOf (12.0f, on));
        g.drawText (names[i], text.toNearestInt(), juce::Justification::centred);
    }
}

// ─── Tuner ──────────────────────────────────────────────────────────────────

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
    const auto& t = current();
    const bool active = processor.tunerActive.load();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (t.surface);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (active ? t.accent : (isMouseOver() || hasKeyboardFocus (false) ? t.borderStrong : t.border));
    g.drawRoundedRectangle (r, 10.0f, 1.0f);

    g.setColour (active ? t.accent : t.muted);
    g.setFont (fontOf (10.0f, true));
    g.drawText (active ? "TUNING" + dot + "OUTPUT MUTED" : juce::String ("TUNER"),
                (int) r.getX() + 12, (int) r.getY() + 8, (int) r.getWidth() - 24, 14, juce::Justification::centredLeft);

    const auto body = juce::Rectangle<float> (r.getX() + 12.0f, r.getY() + 26.0f, r.getWidth() - 24.0f, 22.0f);
    if (! active)
    {
        g.setColour (t.muted);
        g.setFont (fontOf (12.0f));
        g.drawText ("Click to tune", body.toNearestInt(), juce::Justification::centredLeft);
        return;
    }
    if (smoothedHz <= 0.0)
    {
        g.setColour (t.muted);
        g.setFont (fontOf (12.0f));
        g.drawText ("Play one string", body.toNearestInt(), juce::Justification::centredLeft);
        return;
    }

    const auto note = axiom::noteFor (smoothedHz);
    const bool inTune = std::abs (note.cents) < 3.0;
    const auto good = inTune ? t.success : t.text;
    g.setColour (good);
    g.setFont (fontOf (19.0f, true));
    g.drawText (juce::String (axiom::noteName (note.midi)) + juce::String (note.midi / 12 - 1),
                body.withWidth (44.0f).translated (0.0f, -1.0f).toNearestInt(), juce::Justification::centredLeft);

    // Cents scale: centre tick, a dot for the offset and the number on the right.
    const auto scale = juce::Rectangle<float> (body.getX() + 50.0f, body.getCentreY(), body.getWidth() - 50.0f - 40.0f, 0.0f);
    g.setColour (t.pressed);
    g.drawLine (scale.getX(), scale.getY(), scale.getRight(), scale.getY(), 3.0f);
    g.setColour (t.muted);
    g.drawLine (scale.getCentreX(), scale.getY() - 6.0f, scale.getCentreX(), scale.getY() + 6.0f, 1.5f);
    const float px = scale.getCentreX() + (float) juce::jlimit (-50.0, 50.0, note.cents) / 50.0f * scale.getWidth() * 0.5f;
    g.setColour (good);
    g.fillEllipse (px - 5.0f, scale.getY() - 5.0f, 10.0f, 10.0f);
    g.setColour (t.muted);
    g.setFont (fontOf (11.0f));
    const int cents = juce::roundToInt (note.cents);
    g.drawText ((cents > 0 ? "+" : "") + juce::String (cents) + juce::String::fromUTF8 (" \xc2\xa2"),
                juce::Rectangle<float> (body.getRight() - 38.0f, body.getY(), 38.0f, body.getHeight()).toNearestInt(),
                juce::Justification::centredRight);
}

// ─── Editor ─────────────────────────────────────────────────────────────────

AxiomAudioProcessorEditor::AxiomAudioProcessorEditor (AxiomAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), tuner (p)
{
    axiom::theme::use (p.darkTheme.load());
    lookAndFeel.refreshColours();
    setLookAndFeel (&lookAndFeel);
    setSize (defaultWidth, defaultHeight);
    setResizable (true, true);
    setResizeLimits (1040, 720, 1440, 1000);

    configureSlider (gain, gainLabel, "GAIN", "How hard the preamp is driven, from clean to saturated.");
    configureSlider (bass, bassLabel, "BASS", "Low end. Cut it for tighter high-gain rhythm.", true);
    configureSlider (middle, middleLabel, "MIDDLE", "Body and punch. Boost it to cut through a band.", true);
    configureSlider (treble, trebleLabel, "TREBLE", "Edge and pick attack.", true);
    configureSlider (presence, presenceLabel, "PRESENCE", "Upper harmonics after the power amp: air and bite.", true);
    configureSlider (master, masterLabel, "MASTER", "Output level after the amp and cabinet.");
    configureSlider (input, inputLabel, "INPUT TRIM", "Level hitting the preamp. Raise it if your interface is quiet.", true);
    configureSlider (gate, gateLabel, "GATE", "Noise gate. Silences hum and hiss between notes. Lower it if note tails get cut; all the way down is off.");
    configureSlider (boost, boostLabel, "BOOST", "A mid-focused overdrive in front of the amp, for leads.");
    configureSlider (chorus, chorusLabel, "CHORUS", "Stereo chorus after the cabinet.");
    configureSlider (delayMix, delayMixLabel, "DELAY", "Level of the darkening stereo delay.");
    configureSlider (delayTime, delayTimeLabel, "DELAY TIME", "Delay time, 40 ms to 1 s.");
    configureSlider (room, roomKnobLabel, "ROOM", "A small stereo room reverb.");

    addAndMakeVisible (tuner);
    addChildComponent (prompt);

    configureSectionLabel (presetLabel, "TONE");
    addAndMakeVisible (presetLabel);
    preset.setComponentID ("preset");
    preset.setTextWhenNothingSelected ("Custom");
    preset.setJustificationType (juce::Justification::centredLeft);
    preset.setTooltip ("Load a starting tone, or save your own. A preset sets the voice, gain, EQ and effects; every control stays adjustable.");
    rebuildPresetMenu();
    preset.onChange = [this] { presetMenuChanged(); };
    addAndMakeVisible (preset);

    configureSectionLabel (voiceLabel, "AMP VOICE");
    addAndMakeVisible (voiceLabel);
    voiceSelector.setTooltip ("Clean: glassy, lots of headroom. Classic: plexi-style crunch. Modern: tight, hot-rodded high gain.");
    voiceSelector.setTitle ("Amp voice");
    addAndMakeVisible (voiceSelector);

    configureSectionLabel (sourceLabel, "INPUT CHANNEL");
    addAndMakeVisible (sourceLabel);
    inputSource.addItemList ({ "Left", "Right", "Left + Right" }, 1);
    inputSource.setJustificationType (juce::Justification::centredLeft);
    inputSource.setTooltip ("Which input channel carries your guitar. The amp is mono and plays through both ears.");
    addAndMakeVisible (inputSource);

    configureSectionLabel (tunerLabel, "TUNE");
    addAndMakeVisible (tunerLabel);

    cabinet.setButtonText ("Cabinet");
    cabinet.setClickingTogglesState (true);
    cabinet.setTooltip ("The built-in 2x12 speaker response. Turn it off to use your own cabinet impulse response after Axiom.");
    addAndMakeVisible (cabinet);
    cabinetCaption.setText (juce::String::fromUTF8 ("2\xc3\x97" "12 closed-back, studio mic"), juce::dontSendNotification);
    cabinetCaption.setFont (fontOf (11.0f));
    cabinetCaption.setJustificationType (juce::Justification::centredLeft);
    cabinetCaption.setBorderSize ({ 0, 0, 0, 0 });
    addAndMakeVisible (cabinetCaption);

    configureSectionLabel (engineLabel, "AMP ENGINE");
    addAndMakeVisible (engineLabel);
    engineSelector.setTooltip ("Standard: Axiom's original tube-style stages, light on CPU. Physical: the 12AX7 stages, power supply "
                               "and push-pull power amp solved as circuits, sample by sample. Physical uses several times more CPU.");
    engineSelector.setTitle ("Amp engine");
    addAndMakeVisible (engineSelector);
    configureSectionLabel (mathLabel, "TUBE MATH");
    addAndMakeVisible (mathLabel);
    mathSelector.setTooltip ("Physical engine only. Fast: table-based exp/log, at most 4 solver iterations per sample. "
                             "Exact: the system math library and up to 6 iterations, for more CPU. They differ far below audibility.");
    mathSelector.setTitle ("Tube math");
    addAndMakeVisible (mathSelector);

    themeSelector.setTooltip ("Dark or light interface. Saved with your session.");
    themeSelector.setTitle ("Theme");
    themeSelector.setSelected (p.darkTheme.load() ? 0 : 1);
    themeSelector.onChange = [this] (int index)
    {
        audioProcessor.darkTheme.store (index == 0);
        applyTheme();
    };
    addAndMakeVisible (themeSelector);
    footnote.setText ("Everything runs locally on this computer. Your audio never leaves it.", juce::dontSendNotification);
    footnote.setFont (fontOf (11.0f));
    footnote.setJustificationType (juce::Justification::topLeft);
    footnote.setBorderSize ({ 0, 0, 0, 0 });
    footnote.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (footnote);

    referenceStatus.setText ("No reference selected\nWAV, AIFF, FLAC, OGG; MP3, M4A, AAC on macOS.", juce::dontSendNotification);
    referenceStatus.setFont (fontOf (12.0f));
    referenceStatus.setJustificationType (juce::Justification::topLeft);
    referenceStatus.setBorderSize ({ 0, 0, 0, 0 });
    addAndMakeVisible (referenceStatus);

    chooseReferenceButton.setComponentID ("choose-reference");
    chooseReferenceButton.setTooltip ("Choose a short audio file to analyze as a guitar tone reference.");
    chooseReferenceButton.onClick = [this] { chooseReferenceFile(); };
    addAndMakeVisible (chooseReferenceButton);

    applyToneButton.setComponentID ("apply-tone");
    applyToneButton.getProperties().set ("primary", true);
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
    attach (boost, "boost", boostAttachment);
    attach (chorus, "chorus", chorusAttachment);
    attach (delayMix, "delayMix", delayMixAttachment);
    attach (delayTime, "delayTime", delayTimeAttachment);
    cabinetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, "cabinet", cabinet);
    sourceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "inputSource", inputSource);

    if (auto* voiceParameter = state.getParameter ("voice"))
    {
        voiceAttachment = std::make_unique<juce::ParameterAttachment> (*voiceParameter, [this] (float v)
        {
            voiceSelector.setSelected (juce::roundToInt (v));
        });
        voiceSelector.onChange = [this] (int index) { voiceAttachment->setValueAsCompleteGesture ((float) index); };
        voiceAttachment->sendInitialUpdate();
    }

    bindSegmented (engineSelector, "engine", engineAttachment);
    bindSegmented (mathSelector, "tubeMath", mathAttachment);
    refreshEngineControls();

    applyTheme();
    startTimerHz (30);
}

void AxiomAudioProcessorEditor::bindSegmented (AxiomSegmented& selector, const char* parameterId,
                                               std::unique_ptr<juce::ParameterAttachment>& attachment)
{
    if (auto* parameter = audioProcessor.parameters.getParameter (parameterId))
    {
        attachment = std::make_unique<juce::ParameterAttachment> (*parameter, [this, &selector] (float v)
        {
            selector.setSelected (juce::roundToInt (v));
            refreshEngineControls();
        });
        selector.onChange = [&attachment] (int index) { attachment->setValueAsCompleteGesture ((float) index); };
        attachment->sendInitialUpdate();
    }
}

/** Tube math only matters to the Physical engine, so it is dimmed (but still settable) otherwise. */
void AxiomAudioProcessorEditor::refreshEngineControls()
{
    const bool physical = audioProcessor.parameters.getRawParameterValue ("engine")->load() >= 0.5f;
    mathSelector.setAlpha (physical ? 1.0f : 0.45f);
    mathLabel.setAlpha (physical ? 1.0f : 0.45f);
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
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 88, 22);
    slider.setScrollWheelEnabled (false);
    addAndMakeVisible (slider);

    label.setText (text, juce::dontSendNotification);
    label.setFont (fontOf (10.5f, true));
    label.setJustificationType (juce::Justification::centred);
    label.setTooltip (slider.getTooltip());
    addAndMakeVisible (label);
}

void AxiomAudioProcessorEditor::configureSectionLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (fontOf (10.5f, true));
    label.setJustificationType (juce::Justification::centredLeft);
    label.setBorderSize ({ 0, 0, 0, 0 });
}

void AxiomAudioProcessorEditor::applyTheme()
{
    const auto& t = current();
    axiom::theme::use (audioProcessor.darkTheme.load());
    lookAndFeel.refreshColours();
    for (auto* label : { &presetLabel, &voiceLabel, &sourceLabel, &tunerLabel, &engineLabel, &mathLabel })
        label->setColour (juce::Label::textColourId, t.faint);
    for (auto* label : { &cabinetCaption, &footnote, &referenceStatus })
        label->setColour (juce::Label::textColourId, t.muted);
    for (auto* label : { &gainLabel, &bassLabel, &middleLabel, &trebleLabel, &presenceLabel, &masterLabel, &inputLabel, &gateLabel,
                         &roomKnobLabel, &boostLabel, &chorusLabel, &delayMixLabel, &delayTimeLabel })
        label->setColour (juce::Label::textColourId, t.muted);
    for (auto* slider : { &gain, &bass, &middle, &treble, &presence, &master, &input, &gate, &room, &boost, &chorus, &delayMix, &delayTime })
    {
        slider->setColour (juce::Slider::rotarySliderFillColourId, t.accent);
        slider->setColour (juce::Slider::textBoxTextColourId, t.muted);
    }
    shownVoice = juce::roundToInt (audioProcessor.parameters.getRawParameterValue ("voice")->load());
    gain.setColour (juce::Slider::rotarySliderFillColourId, current().voice (shownVoice));
    prompt.applyTheme();
    sendLookAndFeelChange();
    repaint();
}

// ─── Painting ───────────────────────────────────────────────────────────────

AxiomAudioProcessorEditor::Layout AxiomAudioProcessorEditor::computeLayout() const
{
    constexpr float sidebarW = 248.0f, pad = 28.0f, gap = 16.0f, headerH = 104.0f, matchH = 112.0f, rowH = 204.0f;
    Layout l;
    const float w = (float) getWidth(), h = (float) getHeight();
    l.sidebar = { 0.0f, 0.0f, sidebarW, h };
    l.contentX = sidebarW + pad;
    l.contentW = w - l.contentX - pad;
    l.meters = { w - pad - 236.0f, 26.0f, 236.0f, 58.0f };
    const float ampH = h - headerH - pad - matchH - rowH - 2.0f * gap;
    l.ampCard = { l.contentX, headerH, l.contentW, ampH };
    const float rowY = headerH + ampH + gap;
    const float inputW = (l.contentW - gap) * 3.0f / 7.0f;
    l.inputCard = { l.contentX, rowY, inputW, rowH };
    l.effectsCard = { l.contentX + inputW + gap, rowY, l.contentW - inputW - gap, rowH };
    l.matchCard = { l.contentX, rowY + rowH + gap, l.contentW, matchH };
    return l;
}

juce::String AxiomAudioProcessorEditor::currentToneName() const
{
    const auto user = audioProcessor.getUserPresetName();
    if (user.isNotEmpty())
        return user;
    return axiom::factoryPresets[(size_t) juce::jlimit (0, AxiomAudioProcessor::numPresets - 1, audioProcessor.getCurrentProgram())].name;
}

void AxiomAudioProcessorEditor::drawLogo (juce::Graphics& g, juce::Rectangle<float> r) const
{
    const float s = r.getWidth() / 64.0f;
    const auto place = juce::AffineTransform::scale (s).translated (r.getX(), r.getY());
    g.setColour (current().tile);
    g.fillRoundedRectangle (r, 15.0f * s);
    g.setColour (current().borderStrong);
    g.drawRoundedRectangle (r.reduced (0.5f), 15.0f * s, 1.0f);
    auto legs = axiom::theme::logoLegs();
    legs.applyTransform (place);
    g.setColour (brandCream);
    g.strokePath (legs, juce::PathStrokeType (4.5f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    auto wave = axiom::theme::logoWave();
    wave.applyTransform (place);
    g.setColour (brandEmber);
    g.strokePath (wave, juce::PathStrokeType (3.2f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void AxiomAudioProcessorEditor::drawWordmark (juce::Graphics& g, juce::Rectangle<float> r) const
{
    // Lowercase, with a dotless i so the dot can be drawn in the accent colour.
    const auto font = fontOf (27.0f, true);
    const auto text = juce::String::fromUTF8 ("ax\xc4\xb1om");
    g.setColour (current().text);
    g.setFont (font);
    g.drawText (text, r.toNearestInt(), juce::Justification::centredLeft);
    const float before = juce::GlyphArrangement::getStringWidth (font, "ax");
    const float iWidth = juce::GlyphArrangement::getStringWidth (font, juce::String::fromUTF8 ("\xc4\xb1"));
    g.setColour (brandEmber);
    g.fillEllipse (r.getX() + before + iWidth * 0.5f - 2.9f, r.getCentreY() - 12.4f, 5.8f, 5.8f);
}

void AxiomAudioProcessorEditor::drawCard (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, const juce::String& subtitle) const
{
    const auto& t = current();
    g.setColour (t.surface);
    g.fillRoundedRectangle (r, 14.0f);
    g.setColour (t.border);
    g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.0f);
    g.setColour (t.text);
    g.setFont (fontOf (15.0f, true));
    g.drawText (title, juce::Rectangle<float> (r.getX() + 20.0f, r.getY() + 14.0f, 200.0f, 20.0f).toNearestInt(), juce::Justification::centredLeft);
    g.setColour (t.muted);
    g.setFont (fontOf (11.5f));
    g.drawText (subtitle, juce::Rectangle<float> (r.getX() + 20.0f, r.getY() + 34.0f, r.getWidth() - 40.0f, 16.0f).toNearestInt(), juce::Justification::centredLeft);
}

void AxiomAudioProcessorEditor::drawChips (juce::Graphics& g, juce::Rectangle<float> area) const
{
    const auto& t = current();
    const int voice = juce::roundToInt (audioProcessor.parameters.getRawParameterValue ("voice")->load());
    const float gateDb = audioProcessor.parameters.getRawParameterValue ("gate")->load();
    const bool cab = audioProcessor.parameters.getRawParameterValue ("cabinet")->load() >= 0.5f;
    const juce::String voices[] { "Clean", "Classic", "Modern" };

    float x = area.getX();
    const auto chip = [&] (const juce::String& text, juce::Colour dotColour, bool accentChip = false)
    {
        const auto font = fontOf (11.5f, true);
        const float tw = juce::GlyphArrangement::getStringWidth (font, text);
        const float w = tw + 20.0f + (dotColour.isTransparent() ? 0.0f : 13.0f);
        const auto box = juce::Rectangle<float> (x, area.getY(), w, 24.0f);
        g.setColour (accentChip ? t.accentSoft : t.elevated);
        g.fillRoundedRectangle (box, 7.0f);
        float tx = box.getX() + 10.0f;
        if (! dotColour.isTransparent())
        {
            g.setColour (dotColour);
            g.fillEllipse (tx, box.getCentreY() - 3.5f, 7.0f, 7.0f);
            tx += 13.0f;
        }
        g.setColour (accentChip ? t.accent : t.muted);
        g.setFont (font);
        g.drawText (text, juce::Rectangle<float> (tx, box.getY(), tw + 2.0f, box.getHeight()).toNearestInt(), juce::Justification::centredLeft);
        x += w + 8.0f;
    };
    chip (voices[juce::jlimit (0, 2, voice)], t.voice (voice));
    chip (gateDb > axiom::gateOffDb ? "Gate " + juce::String (juce::roundToInt (gateDb)) + " dB" : juce::String ("Gate off"), {});
    chip (cab ? "Cabinet on" : "Cabinet off", {});
    if (audioProcessor.parameters.getRawParameterValue ("engine")->load() >= 0.5f)
        chip (juce::String::fromUTF8 (audioProcessor.parameters.getRawParameterValue ("tubeMath")->load() >= 0.5f ? "Physical \xc2\xb7 Exact" : "Physical \xc2\xb7 Fast"), {});
    if (audioProcessor.presetModified.load())
        chip ("Edited", {}, true);
}

void AxiomAudioProcessorEditor::drawLevelMeters (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto& t = current();
    g.setColour (t.surface);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (t.border);
    g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);

    // IN: what hits the preamp (aim for the shaded zone with Input trim). OUT: after everything.
    const auto row = [&] (const char* name, float peak, float y, bool showSweetSpot)
    {
        const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
        const float level = juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
        g.setColour (t.faint);
        g.setFont (fontOf (10.0f, true));
        g.drawText (name, (int) r.getX() + 14, (int) y - 7, 30, 14, juce::Justification::centredLeft);
        const auto bar = juce::Rectangle<float> (r.getX() + 44.0f, y - 3.0f, r.getWidth() - 44.0f - 52.0f, 6.0f);
        g.setColour (t.pressed);
        g.fillRoundedRectangle (bar, 3.0f);
        if (showSweetSpot)
        {
            // -18 to -6 dBFS: a healthy guitar level into the amp.
            const float a = (48.0f - 18.0f) / 48.0f, b = (48.0f - 6.0f) / 48.0f;
            g.setColour (t.accentSoft);
            g.fillRect (bar.getX() + bar.getWidth() * a, bar.getY(), bar.getWidth() * (b - a), bar.getHeight());
        }
        g.setColour (db > -1.0f ? t.danger : t.accent);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (0.0f, bar.getWidth() * level)), 3.0f);
        g.setColour (t.muted);
        g.setFont (fontOf (10.0f));
        g.drawText (juce::String (db <= -59.5f ? -60.0f : db, 0) + " dB",
                    (int) r.getRight() - 48, (int) y - 7, 38, 14, juce::Justification::centredRight);
    };
    row ("IN", audioProcessor.inputPeak.load (std::memory_order_relaxed), r.getY() + 20.0f, true);
    row ("OUT", audioProcessor.outputPeak.load (std::memory_order_relaxed), r.getY() + 39.0f, false);
}

void AxiomAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto& t = current();
    const auto l = computeLayout();
    g.fillAll (t.background);

    g.setColour (t.sidebar);
    g.fillRect (l.sidebar);
    g.setColour (t.border);
    g.drawLine (l.sidebar.getRight() - 0.5f, 0.0f, l.sidebar.getRight() - 0.5f, (float) getHeight(), 1.0f);
    drawLogo (g, { 24.0f, 24.0f, 38.0f, 38.0f });
    drawWordmark (g, { 74.0f, 22.0f, 150.0f, 42.0f });

    // Header: the tone's name, then what it is made of.
    g.setColour (t.text);
    g.setFont (fontOf (25.0f, true));
    g.drawText (currentToneName(), juce::Rectangle<float> (l.contentX, 22.0f, l.meters.getX() - l.contentX - 16.0f, 34.0f).toNearestInt(),
                juce::Justification::centredLeft);
    drawChips (g, { l.contentX, 62.0f, l.meters.getX() - l.contentX - 16.0f, 24.0f });
    drawLevelMeters (g, l.meters);

    drawCard (g, l.ampCard, "Amp", "Preamp, tone stack and power amp");
    drawCard (g, l.inputCard, "Input", "Level, noise gate and boost");
    drawCard (g, l.effectsCard, "Effects", "Chorus, delay and room");
    drawCard (g, l.matchCard, "Tone match", "Suggest a voice and EQ from a reference clip");
}

void AxiomAudioProcessorEditor::resized()
{
    const auto l = computeLayout();

    const int sx = 24, sw = (int) l.sidebar.getWidth() - 48;
    presetLabel.setBounds (sx, 92, sw, 14);
    preset.setBounds (sx, 110, sw, 36);
    voiceLabel.setBounds (sx, 162, sw, 14);
    voiceSelector.setBounds (sx, 180, sw, 36);
    sourceLabel.setBounds (sx, 232, sw, 14);
    inputSource.setBounds (sx, 250, sw, 36);
    tunerLabel.setBounds (sx, 302, sw, 14);
    tuner.setBounds (sx, 320, sw, 56);
    cabinet.setBounds (sx, 398, sw, 28);
    cabinetCaption.setBounds (sx, 426, sw, 16);
    engineLabel.setBounds (sx, 462, sw, 14);
    engineSelector.setBounds (sx, 480, sw, 36);
    mathLabel.setBounds (sx, 532, sw, 14);
    mathSelector.setBounds (sx, 550, sw, 36);
    themeSelector.setBounds (sx, getHeight() - 96, sw, 36);
    footnote.setBounds (sx, getHeight() - 48, sw, 32);

    const auto place = [] (juce::Rectangle<float> card, float top, std::initializer_list<std::pair<juce::Slider*, juce::Label*>> knobs)
    {
        const float inner = card.getWidth() - 24.0f;
        const float column = inner / (float) knobs.size();
        int i = 0;
        for (auto& k : knobs)
        {
            const int x = (int) (card.getX() + 12.0f + column * (float) i++);
            k.second->setBounds (x, (int) (card.getY() + top), (int) column, 14);
            const float avail = card.getHeight() - top - 16.0f - 10.0f;
            const float wanted = juce::jmin (avail, column * 0.92f + 26.0f);
            k.first->setBounds (x, (int) (card.getY() + top + 16.0f + (avail - wanted) * 0.5f), (int) column, (int) wanted);
        }
    };
    place (l.ampCard, 62.0f, { { &gain, &gainLabel }, { &bass, &bassLabel }, { &middle, &middleLabel },
                               { &treble, &trebleLabel }, { &presence, &presenceLabel }, { &master, &masterLabel } });
    place (l.inputCard, 62.0f, { { &input, &inputLabel }, { &gate, &gateLabel }, { &boost, &boostLabel } });
    place (l.effectsCard, 62.0f, { { &chorus, &chorusLabel }, { &delayMix, &delayMixLabel }, { &delayTime, &delayTimeLabel }, { &room, &roomKnobLabel } });

    const auto& m = l.matchCard;
    referenceStatus.setBounds ((int) m.getX() + 20, (int) m.getY() + 62, (int) m.getWidth() - 20 - 316, 44);
    chooseReferenceButton.setBounds ((int) m.getRight() - 296, (int) m.getY() + 36, 140, 40);
    applyToneButton.setBounds ((int) m.getRight() - 148, (int) m.getY() + 36, 128, 40);
    prompt.setBounds (getLocalBounds());
}

// ─── Presets ────────────────────────────────────────────────────────────────

void AxiomAudioProcessorEditor::rebuildPresetMenu()
{
    userPresets = audioProcessor.getUserPresetFiles();
    preset.clear (juce::dontSendNotification);
    preset.addSectionHeading ("Factory tones");
    for (int i = 0; i < AxiomAudioProcessor::numPresets; ++i)
        preset.addItem (axiom::factoryPresets[(size_t) i].name, i + 1);
    if (! userPresets.isEmpty())
    {
        preset.addSeparator();
        preset.addSectionHeading ("Your tones");
        for (int i = 0; i < userPresets.size(); ++i)
            preset.addItem (userPresets[i].getFileNameWithoutExtension(), userPresetBaseId + i);
    }
    preset.addSeparator();
    preset.addItem (juce::String::fromUTF8 ("Save current tone\xe2\x80\xa6"), savePresetId);
    const auto current = audioProcessor.getUserPresetName();
    if (current.isNotEmpty() && ! audioProcessor.presetModified.load())
        preset.addItem (juce::String::fromUTF8 ("Delete \xe2\x80\x9c") + current + juce::String::fromUTF8 ("\xe2\x80\x9d\xe2\x80\xa6"), deletePresetId);
    preset.addItem ("Show presets folder", revealPresetsId);
    syncPresetSelection();
}

void AxiomAudioProcessorEditor::syncPresetSelection()
{
    int selected = 0;
    if (! audioProcessor.presetModified.load())
    {
        const auto user = audioProcessor.getUserPresetName();
        if (user.isEmpty())
            selected = audioProcessor.getCurrentProgram() + 1;
        else
            for (int i = 0; i < userPresets.size(); ++i)
                if (userPresets[i].getFileNameWithoutExtension() == user)
                    selected = userPresetBaseId + i;
    }
    if (preset.getSelectedId() != selected)
        preset.setSelectedId (selected, juce::dontSendNotification);
    // The "Delete" entry only exists while an unmodified user tone is loaded.
    const bool wantsDelete = audioProcessor.getUserPresetName().isNotEmpty() && ! audioProcessor.presetModified.load();
    if (wantsDelete != (preset.indexOfItemId (deletePresetId) >= 0) && ! preset.isPopupActive())
        rebuildPresetMenu();
}

void AxiomAudioProcessorEditor::presetMenuChanged()
{
    const int id = preset.getSelectedId();
    if (id >= 1 && id <= AxiomAudioProcessor::numPresets)
    {
        audioProcessor.setCurrentProgram (id - 1);
    }
    else if (id >= userPresetBaseId && id - userPresetBaseId < userPresets.size())
    {
        const auto result = audioProcessor.loadUserPreset (userPresets[id - userPresetBaseId]);
        if (result.failed())
            prompt.show ("Couldn't load tone", result.getErrorMessage(), "OK", false, {}, {});
    }
    else if (id == savePresetId)
    {
        promptSavePreset();
    }
    else if (id == deletePresetId)
    {
        promptDeletePreset();
    }
    else if (id == revealPresetsId)
    {
        const auto folder = AxiomAudioProcessor::userPresetFolder();
        folder.createDirectory();
        folder.revealToUser();
    }
    rebuildPresetMenu();
}

void AxiomAudioProcessorEditor::promptSavePreset()
{
    auto name = audioProcessor.getUserPresetName();
    if (name.isEmpty())
        name = audioProcessor.presetModified.load() ? juce::String ("My tone")
                                                    : juce::String (axiom::factoryPresets[(size_t) audioProcessor.getCurrentProgram()].name) + " (mine)";
    auto safe = juce::Component::SafePointer<AxiomAudioProcessorEditor> (this);
    prompt.show ("Save tone", "Your tones appear under Tone in every session. Using an existing name replaces it.",
                 "Save", true, name, [safe] (const juce::String& entered)
    {
        if (safe == nullptr)
            return;
        const auto result = safe->audioProcessor.saveUserPreset (entered);
        if (result.failed())
            safe->prompt.show ("Couldn't save tone", result.getErrorMessage(), "OK", false, {}, {});
        safe->rebuildPresetMenu();
    });
}

void AxiomAudioProcessorEditor::promptDeletePreset()
{
    const auto name = audioProcessor.getUserPresetName();
    const auto file = AxiomAudioProcessor::userPresetFolder().getChildFile (name + AxiomAudioProcessor::userPresetExtension);
    auto safe = juce::Component::SafePointer<AxiomAudioProcessorEditor> (this);
    prompt.show ("Delete tone", juce::String::fromUTF8 ("\xe2\x80\x9c") + name + juce::String::fromUTF8 ("\xe2\x80\x9d will move to the Trash. The sound you hear stays as it is."),
                 "Delete", false, {}, [safe, file] (const juce::String&)
    {
        if (safe == nullptr)
            return;
        if (file.existsAsFile() && ! file.moveToTrash())
            file.deleteFile();
        safe->audioProcessor.presetModified.store (true);
        safe->rebuildPresetMenu();
    });
}

// ─── Prompt ─────────────────────────────────────────────────────────────────

AxiomPromptOverlay::AxiomPromptOverlay()
{
    setWantsKeyboardFocus (true);
    title.setFont (fontOf (16.0f, true));
    message.setFont (fontOf (12.5f));
    message.setJustificationType (juce::Justification::topLeft);
    message.setMinimumHorizontalScale (1.0f);
    field.setFont (fontOf (14.0f));
    field.setIndents (10, 8);
    field.onReturnKey = [this] { confirm(); };
    field.onEscapeKey = [this] { dismiss(); };
    confirmButton.getProperties().set ("primary", true);
    confirmButton.onClick = [this] { confirm(); };
    cancelButton.onClick = [this] { dismiss(); };
    for (auto* c : std::initializer_list<juce::Component*> { &title, &message, &field, &confirmButton, &cancelButton })
        addAndMakeVisible (c);
    applyTheme();
}

void AxiomPromptOverlay::applyTheme()
{
    const auto& t = current();
    title.setColour (juce::Label::textColourId, t.text);
    message.setColour (juce::Label::textColourId, t.muted);
    repaint();
}

void AxiomPromptOverlay::show (const juce::String& heading, const juce::String& text, const juce::String& confirmLabel,
                               bool withField, const juce::String& fieldText, std::function<void (const juce::String&)> onConfirm)
{
    title.setText (heading, juce::dontSendNotification);
    message.setText (text, juce::dontSendNotification);
    confirmButton.setButtonText (confirmLabel);
    hasField = withField;
    field.setVisible (withField);
    field.setText (fieldText, false);
    callback = std::move (onConfirm);
    cancelButton.setVisible (callback != nullptr);
    setVisible (true);
    toFront (true);
    resized();
    if (withField)
    {
        field.grabKeyboardFocus();
        field.selectAll();
    }
    else
    {
        grabKeyboardFocus();
    }
}

void AxiomPromptOverlay::dismiss()
{
    callback = nullptr;
    setVisible (false);
}

void AxiomPromptOverlay::confirm()
{
    auto cb = std::move (callback);
    const auto text = field.getText();
    setVisible (false);
    if (cb != nullptr)
        cb (text);
}

bool AxiomPromptOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        dismiss();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        confirm();
        return true;
    }
    return true;  // modal: keys never reach the controls underneath
}

juce::Rectangle<int> AxiomPromptOverlay::card() const
{
    return getLocalBounds().withSizeKeepingCentre (420, hasField ? 196 : 152);
}

void AxiomPromptOverlay::paint (juce::Graphics& g)
{
    const auto& t = current();
    g.fillAll (juce::Colours::black.withAlpha (t.dark ? 0.55f : 0.28f));
    const auto r = card().toFloat();
    g.setColour (juce::Colour (0x40000000));
    g.fillRoundedRectangle (r.translated (0.0f, 6.0f).expanded (2.0f), 18.0f);
    g.setColour (t.surface);
    g.fillRoundedRectangle (r, 14.0f);
    g.setColour (t.borderStrong);
    g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.0f);
}

void AxiomPromptOverlay::resized()
{
    auto r = card().reduced (24, 20);
    title.setBounds (r.removeFromTop (22));
    r.removeFromTop (6);
    message.setBounds (r.removeFromTop (36));
    if (hasField)
    {
        r.removeFromTop (8);
        field.setBounds (r.removeFromTop (36));
    }
    auto buttons = r.removeFromBottom (36);
    confirmButton.setBounds (buttons.removeFromRight (110));
    buttons.removeFromRight (10);
    cancelButton.setBounds (buttons.removeFromRight (100));
}

// ─── Tone match ─────────────────────────────────────────────────────────────

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
    referenceStatus.setColour (juce::Label::textColourId, current().muted);
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
        referenceStatus.setColour (juce::Label::textColourId, current().warning);
        referenceStatus.setText (error, juce::dontSendNotification);
        return;
    }

    toneSuggestion = suggestion;
    applyToneButton.setEnabled (true);
    referenceStatus.setColour (juce::Label::textColourId, current().muted);
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
    syncPresetSelection();
    tuner.update();

    // The gain arc takes the amp voice's colour; the header repaints when what it shows changes.
    const int voice = juce::roundToInt (audioProcessor.parameters.getRawParameterValue ("voice")->load());
    if (voice != shownVoice)
    {
        shownVoice = voice;
        gain.setColour (juce::Slider::rotarySliderFillColourId, current().voice (voice));
        gain.repaint();
    }
    const auto header = currentToneName() + juce::String (voice) + juce::String (audioProcessor.presetModified.load() ? 1 : 0)
                        + juce::String (audioProcessor.parameters.getRawParameterValue ("gate")->load(), 0)
                        + juce::String (audioProcessor.parameters.getRawParameterValue ("cabinet")->load(), 0)
                        + juce::String (audioProcessor.parameters.getRawParameterValue ("engine")->load(), 0)
                        + juce::String (audioProcessor.parameters.getRawParameterValue ("tubeMath")->load(), 0);
    const auto l = computeLayout();
    if (header != shownHeader)
    {
        shownHeader = header;
        repaint (juce::Rectangle<float> (l.contentX, 0.0f, l.meters.getX() - l.contentX, 100.0f).toNearestInt());
    }
    repaint (l.meters.expanded (2.0f).toNearestInt());
}
