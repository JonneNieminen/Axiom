// axiom-snapshot: renders the plugin editor to a PNG without a host or a window, for the
// README screenshot and as a smoke test that the editor builds its UI without crashing.
// Usage: axiom-snapshot <output.png> [preset-index] [tuner] [save] [light]
//        axiom-snapshot --check   (user presets and session state)
// With "tuner" the tuner is switched on and fed a slightly sharp A string; with "save" the
// Save tone prompt is open.
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include <cstdlib>

namespace
{
/** Processor checks that need the plugin's own code: user presets and session state. */
int runChecks()
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* what)
    {
        std::printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (! ok) ++failures;
    };
    const auto value = [] (AxiomAudioProcessor& p, const char* id)
    {
        auto* param = p.parameters.getParameter (id);
        return param->convertFrom0to1 (param->getValue());
    };
    const auto set = [] (AxiomAudioProcessor& p, const char* id, float v)
    {
        auto* param = p.parameters.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (v));
    };

    juce::TemporaryFile folder;
    folder.getFile().createDirectory();
    setenv ("AXIOM_PRESET_DIR", folder.getFile().getFullPathName().toRawUTF8(), 1);

    AxiomAudioProcessor processor;
    processor.setCurrentProgram (1);
    expect (! processor.presetModified.load(), "loading a factory tone is not a modification");
    set (processor, "gain", 4.2f);
    expect (processor.presetModified.load(), "moving a control marks the tone modified");
    set (processor, "inputSource", 1.0f);
    expect (processor.saveUserPreset ("Lead: Big/Fat").wasOk(), "save a user tone");
    const auto files = processor.getUserPresetFiles();
    expect (files.size() == 1, "the saved tone is listed");
    expect (processor.getUserPresetName().isNotEmpty() && ! processor.presetModified.load(), "the saved tone is now the current one");

    set (processor, "gain", 9.0f);
    set (processor, "inputSource", 2.0f);
    expect (files.size() == 1 && processor.loadUserPreset (files[0]).wasOk(), "load it back");
    expect (std::abs (value (processor, "gain") - 4.2f) < 0.01f, "gain restored");
    expect (std::abs (value (processor, "inputSource") - 2.0f) < 0.01f, "input channel is left alone");

    juce::MemoryBlock state;
    processor.getStateInformation (state);
    AxiomAudioProcessor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());
    expect (restored.getUserPresetName() == processor.getUserPresetName(), "a session remembers the user tone");
    expect (std::abs (value (restored, "gain") - 4.2f) < 0.01f && std::abs (value (restored, "gate") - value (processor, "gate")) < 0.01f,
            "a session restores the controls");

    // A session saved before the gate existed must come back with the gate off.
    auto old = processor.parameters.copyState();
    for (auto* id : { "gate", "engine", "tubeMath" })
        old.removeChild (old.getChildWithProperty ("id", id), nullptr);
    juce::MemoryBlock oldState;
    AxiomAudioProcessor::copyXmlToBinary (*old.createXml(), oldState);
    AxiomAudioProcessor fromOld;
    set (fromOld, "gate", -50.0f);
    set (fromOld, "engine", 1.0f);
    set (fromOld, "tubeMath", 1.0f);
    fromOld.setStateInformation (oldState.getData(), (int) oldState.getSize());
    expect (value (fromOld, "gate") <= axiom::gateOffDb, "old sessions load with the gate off");

    // The theme travels with the session; sessions from before it existed open dark.
    processor.darkTheme.store (false);
    juce::MemoryBlock themed;
    processor.getStateInformation (themed);
    AxiomAudioProcessor lightRestored;
    lightRestored.setStateInformation (themed.getData(), (int) themed.getSize());
    expect (! lightRestored.darkTheme.load(), "a session remembers the light theme");
    AxiomAudioProcessor darkDefault;
    darkDefault.setStateInformation (oldState.getData(), (int) oldState.getSize());
    expect (darkDefault.darkTheme.load(), "old sessions open dark");

    // The amp engine is part of a tone; tube math is a per-computer CPU choice and is not.
    AxiomAudioProcessor engines;
    set (engines, "engine", 1.0f);
    set (engines, "tubeMath", 1.0f);
    expect (engines.saveUserPreset ("Physical lead").wasOk(), "save a Physical-engine tone");
    set (engines, "engine", 0.0f);
    set (engines, "tubeMath", 0.0f);
    expect (engines.loadUserPreset (folder.getFile().getChildFile ("Physical lead.axiompreset")).wasOk(), "load the Physical-engine tone");
    expect (value (engines, "engine") >= 0.5f, "a user tone restores the amp engine");
    expect (value (engines, "tubeMath") < 0.5f, "a user tone leaves tube math alone");
    expect (files.size() == 1 && engines.loadUserPreset (files[0]).wasOk() && value (engines, "engine") < 0.5f,
            "a tone saved before the engine choice existed loads on the Standard engine");
    set (engines, "engine", 1.0f);
    set (engines, "tubeMath", 1.0f);
    engines.setCurrentProgram (2);
    expect (value (engines, "engine") >= 0.5f && value (engines, "tubeMath") >= 0.5f, "factory tones keep the chosen engine and math");
    expect (value (fromOld, "engine") < 0.5f && value (fromOld, "tubeMath") < 0.5f, "old sessions load on the Standard engine");
    juce::MemoryBlock engineState;
    engines.getStateInformation (engineState);
    AxiomAudioProcessor engineRestored;
    engineRestored.setStateInformation (engineState.getData(), (int) engineState.getSize());
    expect (value (engineRestored, "engine") >= 0.5f && value (engineRestored, "tubeMath") >= 0.5f, "a session restores engine and tube math");

    folder.getFile().deleteRecursively();
    std::printf ("\n%s\n", failures == 0 ? "All checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
}

int main (int argc, char** argv)
{
    if (argc > 1 && juce::String (argv[1]) == "--check")
    {
        juce::ScopedJuceInitialiser_GUI gui;
        return runChecks();
    }

    if (argc < 2)
    {
        std::printf ("Usage: axiom-snapshot <output.png> [preset-index]\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    AxiomAudioProcessor processor;
    const auto flag = [&] (const char* name)
    {
        for (int i = 3; i < argc; ++i)
            if (juce::String (argv[i]) == name)
                return true;
        return false;
    };
    if (argc > 2)
        processor.setCurrentProgram (juce::String (argv[2]).getIntValue());
    if (flag ("light"))
        processor.darkTheme.store (false);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (AxiomAudioProcessorEditor::defaultWidth, AxiomAudioProcessorEditor::defaultHeight);

    if (flag ("tuner"))
    {
        processor.tunerActive.store (true);
        processor.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;
        for (int start = 0; start < 8192; start += 512)
        {
            for (int i = 0; i < 512; ++i)
            {
                const auto v = 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.6 * (start + i) / 48000.0);
                block.setSample (0, i, v);
                block.setSample (1, i, 0.0f);
            }
            processor.processBlock (block, midi);
        }
        for (auto* child : editor->getChildren())
            if (auto* tuner = dynamic_cast<AxiomTunerDisplay*> (child))
                tuner->update();
    }
    if (flag ("save"))
        if (auto* menu = dynamic_cast<juce::ComboBox*> (editor->findChildWithID ("preset")))
            menu->setSelectedId (900, juce::sendNotificationSync);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);

    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    if (! out.openedOk() || ! png.writeImageToStream (image, out))
    {
        std::printf ("Could not write %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }
    std::printf ("Wrote %s (%d x %d)\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    editor.reset();
    return 0;
}
