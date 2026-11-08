#pragma once
#include <JuceHeader.h>

/**
    Design tokens: the only place colours are defined. Axiom's palette is "Ember": warm
    graphite surfaces with hairline borders and one copper-orange accent, in dark and light.
    Each amp voice has its own colour (teal, amber, coral) that tints the gain arc and chips.
    design/theme/tokens.css holds the same values for the README art; keep both in step.
*/
namespace axiom::theme
{
struct Palette
{
    juce::Colour background, sidebar, surface, elevated, pressed;
    juce::Colour border, borderStrong;
    juce::Colour text, muted, faint;
    juce::Colour accent, accentHover, accentText, accentSoft;
    juce::Colour voiceClean, voiceClassic, voiceModern;
    juce::Colour danger, success, warning;
    juce::Colour tooltipBackground, tooltipText;
    juce::Colour tile; // the logo tile (the same in both themes)
    bool dark = true;

    juce::Colour voice (int index) const
    {
        return index <= 0 ? voiceClean : (index == 1 ? voiceClassic : voiceModern);
    }
};

inline Palette darkPalette()
{
    Palette p;
    p.background = juce::Colour (0xff171513);
    p.sidebar = juce::Colour (0xff1e1b18);
    p.surface = juce::Colour (0xff26221e);
    p.elevated = juce::Colour (0xff302b26);
    p.pressed = juce::Colour (0xff3a342e);
    p.border = juce::Colour (0x1affffff);
    p.borderStrong = juce::Colour (0x2effffff);
    p.text = juce::Colour (0xfff2ece3);
    p.muted = juce::Colour (0xffa0978b);
    p.faint = juce::Colour (0xff6f675d);
    p.accent = juce::Colour (0xfff08a3c);
    p.accentHover = juce::Colour (0xfff59a55);
    p.accentText = juce::Colour (0xff1a1008);
    p.accentSoft = juce::Colour (0x2ef08a3c);
    p.voiceClean = juce::Colour (0xff5bb5a6);
    p.voiceClassic = juce::Colour (0xfff2b84b);
    p.voiceModern = juce::Colour (0xffee6a4d);
    p.danger = juce::Colour (0xffe5786a);
    p.success = juce::Colour (0xff7bc48f);
    p.warning = juce::Colour (0xfff2b84b);
    p.tooltipBackground = juce::Colour (0xffefe9df);
    p.tooltipText = juce::Colour (0xff1e1b18);
    p.tile = juce::Colour (0xff1f1a16);
    p.dark = true;
    return p;
}

inline Palette lightPalette()
{
    Palette p;
    p.background = juce::Colour (0xfff7f3ec);
    p.sidebar = juce::Colour (0xffeee8de);
    p.surface = juce::Colour (0xffffffff);
    p.elevated = juce::Colour (0xfff2ece2);
    p.pressed = juce::Colour (0xffe9e1d5);
    p.border = juce::Colour (0x17000000);
    p.borderStrong = juce::Colour (0x29000000);
    p.text = juce::Colour (0xff231f1b);
    p.muted = juce::Colour (0xff6f675d);
    p.faint = juce::Colour (0xffa59c90);
    p.accent = juce::Colour (0xffd4611a);
    p.accentHover = juce::Colour (0xffc45612);
    p.accentText = juce::Colour (0xffffffff);
    p.accentSoft = juce::Colour (0x1ed4611a);
    p.voiceClean = juce::Colour (0xff2f8f80);
    p.voiceClassic = juce::Colour (0xffb9801c);
    p.voiceModern = juce::Colour (0xffc8472e);
    p.danger = juce::Colour (0xffc4473a);
    p.success = juce::Colour (0xff2f9663);
    p.warning = juce::Colour (0xffb07a1f);
    p.tooltipBackground = juce::Colour (0xff2c2825);
    p.tooltipText = juce::Colour (0xfff7f3ec);
    p.tile = juce::Colour (0xff1f1a16);
    p.dark = false;
    return p;
}

/** The palette the editor draws with. Message thread only. */
inline Palette& current()
{
    static Palette palette = darkPalette();
    return palette;
}

inline void use (bool dark)
{
    current() = dark ? darkPalette() : lightPalette();
}

/** The logo geometry (64 x 64 units), shared by the plugin and design/logo/make.py. */
inline juce::Path logoLegs()
{
    juce::Path p;
    p.startNewSubPath (17.0f, 48.0f);
    p.lineTo (32.0f, 15.5f);
    p.lineTo (47.0f, 48.0f);
    return p;
}

/** One period of a soft-clipped sine: the crossbar of the A. */
inline juce::Path logoWave()
{
    juce::Path p;
    constexpr int steps = 48;
    for (int i = 0; i <= steps; ++i)
    {
        const float t = (float) i / (float) steps;
        const float x = 22.9f + (41.1f - 22.9f) * t;
        const float y = 38.0f - 5.4f * std::tanh (2.8f * std::sin (juce::MathConstants<float>::twoPi * t)) / std::tanh (2.8f);
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    return p;
}
}
