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
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate = sampleRate;
    const juce::dsp::ProcessSpec spec { sampleRate, 1, (juce::uint32) getTotalNumOutputChannels() };

    for (auto& strip : strips)
    {
        strip.inputHighPass.prepare (spec);
        strip.bass.prepare (spec);
        strip.mid.prepare (spec);
        strip.treble.prepare (spec);
        strip.presence.prepare (spec);
        strip.cabHighPass.prepare (spec);
        strip.cabResonance.prepare (spec);
        strip.cabLowPass.prepare (spec);
        const auto makeIdentity = [] { return new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f); };
        strip.inputHighPass.coefficients = makeIdentity();
        strip.bass.coefficients = makeIdentity();
        strip.mid.coefficients = makeIdentity();
        strip.treble.coefficients = makeIdentity();
        strip.presence.coefficients = makeIdentity();
        strip.cabHighPass.coefficients = makeIdentity();
        strip.cabResonance.coefficients = makeIdentity();
        strip.cabLowPass.coefficients = makeIdentity();
        strip.inputHighPass.reset();
        strip.bass.reset();
        strip.mid.reset();
        strip.treble.reset();
        strip.presence.reset();
        strip.cabHighPass.reset();
        strip.cabResonance.reset();
        strip.cabLowPass.reset();
    }

    reverb.reset();
    reverb.setSampleRate (sampleRate);
    inputGain.reset (sampleRate, 0.025);
    ampGain.reset (sampleRate, 0.04);
    masterGain.reset (sampleRate, 0.025);
    roomMix.reset (sampleRate, 0.08);
    lastBass = read (parameters, bassId);
    lastMiddle = read (parameters, midId);
    lastTreble = read (parameters, trebleId);
    lastPresence = read (parameters, presenceId);
    updateToneFilters (lastBass, lastMiddle, lastTreble, lastPresence);
}

void AxiomAudioProcessor::updateToneFilters (float bassDb, float midDb, float trebleDb, float presenceDb)
{
    const float nyquistSafe = (float) (currentSampleRate * 0.44);
    using Coefficients = juce::dsp::IIR::ArrayCoefficients<float>;
    const auto inputHighPass = Coefficients::makeHighPass (currentSampleRate, 72.0f, 0.707f);
    const auto bassCoefficients = Coefficients::makeLowShelf (currentSampleRate, 115.0f, 0.707f, juce::Decibels::decibelsToGain (bassDb));
    const auto midCoefficients = Coefficients::makePeakFilter (currentSampleRate, 720.0f, 0.85f, juce::Decibels::decibelsToGain (midDb));
    const auto trebleCoefficients = Coefficients::makeHighShelf (currentSampleRate, 3400.0f, 0.707f, juce::Decibels::decibelsToGain (trebleDb));
    const auto presenceCoefficients = Coefficients::makeHighShelf (currentSampleRate, 4600.0f, 0.707f, juce::Decibels::decibelsToGain (presenceDb));
    const auto cabHighPass = Coefficients::makeHighPass (currentSampleRate, 78.0f, 0.707f);
    const auto cabResonance = Coefficients::makePeakFilter (currentSampleRate, juce::jmin (2800.0f, nyquistSafe), 1.1f, juce::Decibels::decibelsToGain (2.0f));
    const auto cabLowPass = Coefficients::makeLowPass (currentSampleRate, juce::jmin (6800.0f, nyquistSafe), 0.707f);
    const auto set = [] (auto& filter, const auto& values) { *filter.coefficients = values; };
    for (auto& strip : strips)
    {
        set (strip.inputHighPass, inputHighPass);
        set (strip.bass, bassCoefficients);
        set (strip.mid, midCoefficients);
        set (strip.treble, trebleCoefficients);
        set (strip.presence, presenceCoefficients);
        set (strip.cabHighPass, cabHighPass);
        set (strip.cabResonance, cabResonance);
        set (strip.cabLowPass, cabLowPass);
    }
}

void AxiomAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin (buffer.getNumChannels(), 2);
    const int samples = buffer.getNumSamples();

    const float inputDb = read (parameters, inputId);
    const float gain = read (parameters, gainId);
    const int voice = juce::roundToInt (read (parameters, voiceId));
    const float bassDb = read (parameters, bassId);
    const float midDb = read (parameters, midId);
    const float trebleDb = read (parameters, trebleId);
    const float presenceDb = read (parameters, presenceId);
    const float masterDb = read (parameters, masterId);
    const float room = read (parameters, roomId) * 0.01f;
    const bool cabinetEnabled = read (parameters, cabinetId) >= 0.5f;

    const float voiceDrive = voice == 0 ? 0.28f : (voice == 1 ? 0.56f : 0.88f);
    const float drive = 1.0f + gain * voiceDrive;
    const float stageOutput = voice == 0 ? 1.15f : (voice == 1 ? 0.88f : 0.72f);
    inputGain.setTargetValue (juce::Decibels::decibelsToGain (inputDb));
    ampGain.setTargetValue (drive);
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (masterDb) * stageOutput);
    roomMix.setTargetValue (room);

    const auto differs = [] (float a, float b) { return ! juce::exactlyEqual (a, b); };
    if (differs (bassDb, lastBass) || differs (midDb, lastMiddle) || differs (trebleDb, lastTreble) || differs (presenceDb, lastPresence))
    {
        updateToneFilters (bassDb, midDb, trebleDb, presenceDb);
        lastBass = bassDb;
        lastMiddle = midDb;
        lastTreble = trebleDb;
        lastPresence = presenceDb;
    }

    // Single-input audio interfaces leave the right input silent in the standalone app;
    // feed the guitar to both sides so the amp is centred instead of left-only.
    if (wrapperType == wrapperType_Standalone && channels == 2
        && buffer.getMagnitude (1, 0, samples) == 0.0f && buffer.getMagnitude (0, 0, samples) > 0.0f)
        buffer.copyFrom (1, 0, buffer, 0, 0, samples);

    for (int i = 0; i < samples; ++i)
    {
        const float in = inputGain.getNextValue();
        const float driveNow = ampGain.getNextValue();
        const float out = masterGain.getNextValue();
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& s = strips[(size_t) ch];
            float x = buffer.getSample (ch, i) * in;
            x = s.inputHighPass.processSample (x);
            x *= driveNow;

            if (voice == 0)
            {
                x = std::tanh (x * 0.92f);
            }
            else if (voice == 1)
            {
                x = std::tanh (x * 1.15f);
                x = std::tanh (x * 1.18f);
            }
            else
            {
                x = std::tanh (x * 1.35f);
                x = std::tanh (x * 1.30f);
                x = std::tanh (x * 1.18f);
                x = 0.92f * x + 0.08f * std::tanh (x * 2.4f);
            }

            x = s.bass.processSample (x);
            x = s.mid.processSample (x);
            x = s.treble.processSample (x);
            x = s.presence.processSample (x);
            if (cabinetEnabled)
            {
                x = s.cabHighPass.processSample (x);
                x = s.cabResonance.processSample (x);
                x = s.cabLowPass.processSample (x);
            }
            buffer.setSample (ch, i, x * out);
        }
    }

    const float smoothedRoom = roomMix.skip (samples);
    if (channels > 0 && smoothedRoom > 0.0001f)
    {
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.25f + smoothedRoom * 0.65f;
        rp.damping = 0.52f;
        rp.wetLevel = smoothedRoom * 0.28f;
        rp.dryLevel = 1.0f;
        rp.width = 1.0f;
        reverb.setParameters (rp);
        if (channels == 2)
            reverb.processStereo (buffer.getWritePointer (0), buffer.getWritePointer (1), samples);
        else
            reverb.processMono (buffer.getWritePointer (0), samples);
    }

    float peak = 0.0f;
    for (int ch = 0; ch < channels; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, samples));
    outputPeak.store (juce::jmax (peak, outputPeak.load (std::memory_order_relaxed) * 0.88f), std::memory_order_relaxed);
}

void AxiomAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    std::unique_ptr<juce::XmlElement> xml (parameters.copyState().createXml());
    copyXmlToBinary (*xml, dest);
}

void AxiomAudioProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml != nullptr && xml->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* AxiomAudioProcessor::createEditor() { return new AxiomAudioProcessorEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AxiomAudioProcessor(); }
