#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr auto inputId = "input";
constexpr auto gainId = "gain";
constexpr auto bassId = "bass";
constexpr auto midId = "mid";
constexpr auto trebleId = "treble";
constexpr auto presenceId = "presence";
constexpr auto masterId = "master";
constexpr auto roomId = "room";
constexpr auto voiceId = "voice";
constexpr auto cabinetId = "cabinet";
constexpr auto sourceId = "inputSource";
constexpr auto boostId = "boost";
constexpr auto chorusId = "chorus";
constexpr auto delayMixId = "delayMix";
constexpr auto delayTimeId = "delayTime";
constexpr auto gateId = "gate";

juce::String formatValue (float value, const char* unit, int decimals)
{
    const auto text = juce::String (value, decimals);
    const auto clean = text.containsOnly ("-0.") ? text.trimCharactersAtStart ("-") : text;
    return clean + unit;
}

juce::AudioParameterFloatAttributes decibels()
{
    return juce::AudioParameterFloatAttributes()
        .withLabel (" dB")
        .withStringFromValueFunction ([] (float v, int) { return formatValue (v, " dB", 1); });
}

float read (juce::AudioProcessorValueTreeState& state, const char* id)
{
    return state.getRawParameterValue (id)->load (std::memory_order_relaxed);
}
}

AxiomAudioProcessor::AxiomAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "AXIOM_STATE", createParameterLayout())
{
    for (auto* parameter : getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            parameters.addParameterListener (withId->paramID, this);
}

AxiomAudioProcessor::~AxiomAudioProcessor()
{
    for (auto* parameter : getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            parameters.removeParameterListener (withId->paramID, this);
}

void AxiomAudioProcessor::parameterChanged (const juce::String&, float)
{
    if (! applyingPreset)
        presetModified.store (true);
}

juce::AudioProcessorValueTreeState::ParameterLayout AxiomAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout p;

    p.add (std::make_unique<Float> (juce::ParameterID { inputId, 1 }, "Input", juce::NormalisableRange<float> (-18.0f, 12.0f, 0.01f), 0.0f, decibels()));
    p.add (std::make_unique<Float> (juce::ParameterID { gainId, 1 }, "Gain", juce::NormalisableRange<float> (0.0f, 10.0f, 0.01f), 3.5f, juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatValue (v, "", 1); })));
    p.add (std::make_unique<Float> (juce::ParameterID { bassId, 1 }, "Bass", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, decibels()));
    p.add (std::make_unique<Float> (juce::ParameterID { midId, 1 }, "Middle", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, decibels()));
    p.add (std::make_unique<Float> (juce::ParameterID { trebleId, 1 }, "Treble", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, decibels()));
    p.add (std::make_unique<Float> (juce::ParameterID { presenceId, 1 }, "Presence", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, decibels()));
    p.add (std::make_unique<Float> (juce::ParameterID { masterId, 1 }, "Master", juce::NormalisableRange<float> (-24.0f, 6.0f, 0.01f), -3.0f, decibels()));
    p.add (std::make_unique<Float> (juce::ParameterID { roomId, 1 }, "Room", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 16.0f, juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction ([] (float v, int) { return formatValue (v, "%", 0); })));
    p.add (std::make_unique<Choice> (juce::ParameterID { voiceId, 1 }, "Voice",
                                     juce::StringArray { "Clean", "Classic", "Modern" }, 1));
    p.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { cabinetId, 1 }, "Cabinet", true));
    p.add (std::make_unique<Choice> (juce::ParameterID { sourceId, 1 }, "Input Source",
                                     juce::StringArray { "Left", "Right", "Left + Right" }, 0));
    const auto percent = juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction ([] (float v, int) { return formatValue (v, "%", 0); });
    p.add (std::make_unique<Float> (juce::ParameterID { boostId, 1 }, "Boost", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    p.add (std::make_unique<Float> (juce::ParameterID { chorusId, 1 }, "Chorus", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    p.add (std::make_unique<Float> (juce::ParameterID { delayMixId, 1 }, "Delay", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    p.add (std::make_unique<Float> (juce::ParameterID { delayTimeId, 1 }, "Delay Time", juce::NormalisableRange<float> (40.0f, 1000.0f, 1.0f, 0.5f), 380.0f,
                                    juce::AudioParameterFloatAttributes().withLabel (" ms").withStringFromValueFunction ([] (float v, int) { return formatValue (v, " ms", 0); })));
    p.add (std::make_unique<Float> (juce::ParameterID { gateId, 1 }, "Noise Gate", juce::NormalisableRange<float> (-80.0f, -20.0f, 0.1f), -80.0f,
                                    juce::AudioParameterFloatAttributes().withLabel (" dB").withStringFromValueFunction ([] (float v, int)
                                    {
                                        return v <= axiom::gateOffDb ? juce::String ("Off") : formatValue (v, " dB", 0);
                                    })));
    return p;
}

bool AxiomAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return input == output && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

void AxiomAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    dsp.prepare (sampleRate, samplesPerBlock);
    currentSampleRate.store (sampleRate);
    outputGain = tunerActive.load() ? 0.0f : 1.0f;
    setLatencySamples (dsp.getLatencySamples());
}

axiom::AxiomParams AxiomAudioProcessor::readParameters()
{
    axiom::AxiomParams p;
    p.inputDb = read (parameters, inputId);
    p.gain = read (parameters, gainId);
    p.voice = juce::roundToInt (read (parameters, voiceId));
    p.source = juce::roundToInt (read (parameters, sourceId));
    p.bassDb = read (parameters, bassId);
    p.midDb = read (parameters, midId);
    p.trebleDb = read (parameters, trebleId);
    p.presenceDb = read (parameters, presenceId);
    p.masterDb = read (parameters, masterId);
    p.room = read (parameters, roomId);
    p.boost = read (parameters, boostId);
    p.chorus = read (parameters, chorusId);
    p.delayMix = read (parameters, delayMixId);
    p.delayMs = read (parameters, delayTimeId);
    p.cabinet = read (parameters, cabinetId) >= 0.5f;
    p.gateDb = read (parameters, gateId);
    return p;
}

void AxiomAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int channels = juce::jmin (buffer.getNumChannels(), 2);
    const int samples = buffer.getNumSamples();
    if (channels == 0 || samples == 0)
        return;

    const auto params = readParameters();
    const float* inL = buffer.getReadPointer (0);
    const float* inR = channels > 1 ? buffer.getReadPointer (1) : nullptr;

    {
        const float trim = juce::Decibels::decibelsToGain (params.inputDb);
        float in = 0.0f;
        for (int i = 0; i < samples; ++i)
        {
            const float r = inR != nullptr ? inR[i] : inL[i];
            const float src = params.source == 1 ? r : (params.source == 2 ? 0.5f * (inL[i] + r) : inL[i]);
            in = juce::jmax (in, std::abs (src));
        }
        inputPeak.store (juce::jmax (in * trim, inputPeak.load (std::memory_order_relaxed) * 0.88f), std::memory_order_relaxed);
    }

    const bool tuning = tunerActive.load (std::memory_order_relaxed);
    if (tuning)
    {
        int w = tunerWrite.load (std::memory_order_relaxed);
        for (int i = 0; i < samples; ++i)
        {
            const float r = inR != nullptr ? inR[i] : inL[i];
            const float src = params.source == 1 ? r : (params.source == 2 ? 0.5f * (inL[i] + r) : inL[i]);
            tunerBuffer[(size_t) w].store (src, std::memory_order_relaxed);
            w = (w + 1) & (tunerBufferSize - 1);
        }
        tunerWrite.store (w, std::memory_order_release);
    }

    dsp.process (inL, inR, buffer.getWritePointer (0), channels > 1 ? buffer.getWritePointer (1) : nullptr, samples, params);

    const float targetGain = tuning ? 0.0f : 1.0f;
    if (! juce::exactlyEqual (outputGain, targetGain) || targetGain < 1.0f)
    {
        buffer.applyGainRamp (0, samples, outputGain, targetGain);
        outputGain = targetGain;
    }

    float peak = 0.0f;
    for (int ch = 0; ch < channels; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, samples));
    outputPeak.store (juce::jmax (peak, outputPeak.load (std::memory_order_relaxed) * 0.88f), std::memory_order_relaxed);
}

void AxiomAudioProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, numPresets - 1, index);
    applyingPreset = true;
    const auto& p = axiom::factoryPresets[(size_t) index].params;
    const auto set = [this] (const char* id, float value)
    {
        if (auto* parameter = parameters.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    set (inputId, p.inputDb);
    set (gainId, p.gain);
    set (bassId, p.bassDb);
    set (midId, p.midDb);
    set (trebleId, p.trebleDb);
    set (presenceId, p.presenceDb);
    set (masterId, p.masterDb);
    set (roomId, p.room);
    set (voiceId, (float) p.voice);
    set (cabinetId, p.cabinet ? 1.0f : 0.0f);
    set (boostId, p.boost);
    set (chorusId, p.chorus);
    set (delayMixId, p.delayMix);
    set (delayTimeId, p.delayMs);
    set (gateId, p.gateDb);
    applyingPreset = false;
    currentPreset.store (index);
    presetModified.store (false);
    setUserPresetName ({});
}

juce::File AxiomAudioProcessor::userPresetFolder()
{
    // Tests point this somewhere temporary so they never touch your real presets.
    const auto overridden = juce::SystemStats::getEnvironmentVariable ("AXIOM_PRESET_DIR", {});
    if (overridden.isNotEmpty())
        return juce::File (overridden);
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Presets/Axiom");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Axiom/Presets");
   #endif
}

juce::Array<juce::File> AxiomAudioProcessor::getUserPresetFiles() const
{
    auto files = userPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + userPresetExtension);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0;
    });
    return files;
}

juce::Result AxiomAudioProcessor::saveUserPreset (const juce::String& rawName)
{
    const auto name = juce::File::createLegalFileName (rawName.trim()).trim();
    if (name.isEmpty())
        return juce::Result::fail ("Give the tone a name.");

    juce::XmlElement xml ("AxiomPreset");
    xml.setAttribute ("name", name);
    xml.setAttribute ("version", 1);
    for (auto* parameter : getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            if (withId->paramID != sourceId)  // the input channel is hardware, not tone
            {
                auto* e = xml.createNewChildElement ("param");
                e->setAttribute ("id", withId->paramID);
                e->setAttribute ("value", (double) withId->convertFrom0to1 (withId->getValue()));
            }

    const auto folder = userPresetFolder();
    if (! folder.createDirectory())
        return juce::Result::fail ("Axiom couldn't create " + folder.getFullPathName());
    const auto file = folder.getChildFile (name + userPresetExtension);
    if (! xml.writeTo (file))
        return juce::Result::fail ("Axiom couldn't write " + file.getFullPathName());

    setUserPresetName (name);
    presetModified.store (false);
    return juce::Result::ok();
}

juce::Result AxiomAudioProcessor::loadUserPreset (const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("AxiomPreset"))
        return juce::Result::fail ("That preset file couldn't be read.");

    applyingPreset = true;
    for (auto* parameter : getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (parameter))
        {
            if (withId->paramID == sourceId)
                continue;
            // A control the preset doesn't know (saved by an older Axiom) goes to its default.
            float normalised = withId->getDefaultValue();
            for (auto* e : xml->getChildWithTagNameIterator ("param"))
                if (e->getStringAttribute ("id") == withId->paramID)
                    normalised = withId->convertTo0to1 ((float) e->getDoubleAttribute ("value"));
            withId->setValueNotifyingHost (normalised);
        }
    applyingPreset = false;
    setUserPresetName (file.getFileNameWithoutExtension());
    presetModified.store (false);
    return juce::Result::ok();
}

juce::String AxiomAudioProcessor::getUserPresetName() const
{
    const juce::ScopedLock lock (userPresetLock);
    return userPresetName;
}

void AxiomAudioProcessor::setUserPresetName (const juce::String& name)
{
    const juce::ScopedLock lock (userPresetLock);
    userPresetName = name;
}

double AxiomAudioProcessor::readTunerInput (float* dest, int count) const
{
    count = juce::jlimit (0, tunerBufferSize, count);
    const int end = tunerWrite.load (std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        dest[i] = tunerBuffer[(size_t) ((end - count + i) & (tunerBufferSize - 1))].load (std::memory_order_relaxed);
    return currentSampleRate.load();
}

const juce::String AxiomAudioProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, numPresets) ? juce::String (axiom::factoryPresets[(size_t) index].name) : juce::String();
}

void AxiomAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty ("preset", currentPreset.load(), nullptr);
    state.setProperty ("presetModified", presetModified.load(), nullptr);
    state.setProperty ("userPreset", getUserPresetName(), nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, dest);
}

void AxiomAudioProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml != nullptr && xml->hasTagName (parameters.state.getType()))
    {
        const auto state = juce::ValueTree::fromXml (*xml);
        currentPreset.store (juce::jlimit (0, numPresets - 1, (int) state.getProperty ("preset", 0)));
        applyingPreset = true;
        parameters.replaceState (state);
        applyingPreset = false;
        presetModified.store ((bool) state.getProperty ("presetModified", ! state.hasProperty ("preset")));
        setUserPresetName (state.getProperty ("userPreset", juce::String()).toString());
    }
}

juce::AudioProcessorEditor* AxiomAudioProcessor::createEditor() { return new AxiomAudioProcessorEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AxiomAudioProcessor(); }
