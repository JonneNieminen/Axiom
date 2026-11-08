// axiom-snapshot: renders the plugin editor to a PNG without a host or a window, for the
// README screenshot and as a smoke test that the editor builds its UI without crashing.
// Usage: axiom-snapshot <output.png> [preset-index] [tuner]
// With "tuner" the tuner is switched on and fed a slightly sharp A string.
#include "PluginEditor.h"
#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("Usage: axiom-snapshot <output.png> [preset-index]\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    AxiomAudioProcessor processor;
    if (argc > 2)
        processor.setCurrentProgram (juce::String (argv[2]).getIntValue());

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1080, 820);

    if (argc > 3 && juce::String (argv[3]) == "tuner")
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
