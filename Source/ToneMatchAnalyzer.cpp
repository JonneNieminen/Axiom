#include "ToneMatchAnalyzer.h"
#include <array>
#include <cmath>
#include <vector>

AxiomToneAnalysisResult AxiomToneMatchAnalyzer::analyzeFile (const juce::File& file,
                                                              const std::function<bool()>& shouldCancel)
{
    AxiomToneAnalysisResult result;
    result.suggestion.sourceName = file.getFileName();
    const auto cancelled = [&] { return shouldCancel && shouldCancel(); };

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        result.error = "Axiom couldn't read that file.\nTry WAV, AIFF, FLAC, or OGG audio.";
        return result;
    }

    constexpr int fftOrder = 12;
    constexpr int fftSize = 1 << fftOrder;
    constexpr int hopSize = fftSize / 2;
    constexpr double maxAnalysisSeconds = 15.0;
    const int channelCount = juce::jlimit (1, 2, (int) reader->numChannels);
    const auto boundedLength = juce::jmin (reader->lengthInSamples,
                                           (juce::int64) std::ceil (reader->sampleRate * maxAnalysisSeconds));
    const int sampleCount = (int) boundedLength;
    if (sampleCount < fftSize || reader->sampleRate <= 0.0)
    {
        result.error = "That clip is too short to analyze.\nChoose at least 0.1 seconds of audio.";
        return result;
    }
    if (cancelled())
        return result;

    juce::AudioBuffer<float> clip (channelCount, sampleCount);
    if (! reader->read (&clip, 0, sampleCount, 0, true, channelCount > 1))
    {
        result.error = "Axiom couldn't decode that clip.\nTry WAV, AIFF, FLAC, or OGG audio.";
        return result;
    }

    double squareSum = 0.0;
    float peak = 0.0f;
    const auto totalSamples = (size_t) sampleCount * (size_t) channelCount;
    for (int i = 0; i < sampleCount; ++i)
    {
        if ((i & 4095) == 0 && cancelled())
            return result;
        for (int channel = 0; channel < channelCount; ++channel)
        {
            const float sample = clip.getSample (channel, i);
            peak = juce::jmax (peak, std::abs (sample));
            squareSum += (double) sample * (double) sample;
        }
    }
    const double rms = std::sqrt (squareSum / (double) totalSamples);
    if (rms < 1.0e-5 || peak < 1.0e-5f)
    {
        result.error = "That clip is nearly silent.\nChoose a reference with audible guitar.";
        return result;
    }

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> fftData ((size_t) fftSize * 2, 0.0f);
    std::array<double, 5> bandPower {};
    std::array<size_t, 5> bandBinCount {};
    size_t analyzedFrames = 0;
    const double rate = reader->sampleRate;

    for (int frameStart = 0; frameStart + fftSize <= sampleCount; frameStart += hopSize)
    {
        if (cancelled())
            return result;

        std::fill (fftData.begin(), fftData.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
        {
            float mono = 0.0f;
            for (int channel = 0; channel < channelCount; ++channel)
                mono += clip.getSample (channel, frameStart + i);
            mono /= (float) channelCount;
            const float window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (fftSize - 1));
            fftData[(size_t) i] = mono * window;
        }
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        for (int bin = 1; bin < fftSize / 2; ++bin)
        {
            const double frequency = (double) bin * rate / (double) fftSize;
            int band = -1;
            if (frequency >= 80.0 && frequency < 180.0) band = 0;
            else if (frequency >= 180.0 && frequency < 600.0) band = 1;
            else if (frequency >= 600.0 && frequency < 1800.0) band = 2;
            else if (frequency >= 1800.0 && frequency < 4500.0) band = 3;
            else if (frequency >= 4500.0 && frequency <= juce::jmin (10000.0, rate * 0.48)) band = 4;

            if (band >= 0)
            {
                const double magnitude = fftData[(size_t) bin];
                bandPower[(size_t) band] += magnitude * magnitude;
                ++bandBinCount[(size_t) band];
            }
        }
        ++analyzedFrames;
    }

    if (analyzedFrames == 0)
    {
        result.error = "Axiom couldn't find enough audio to analyze.";
        return result;
    }

    std::array<float, 5> bandDb {};
    float meanBandDb = 0.0f;
    for (size_t i = 0; i < bandDb.size(); ++i)
    {
        const double meanPower = bandBinCount[i] > 0 ? bandPower[i] / (double) bandBinCount[i] : 1.0e-12;
        bandDb[i] = (float) (10.0 * std::log10 (juce::jmax (meanPower, 1.0e-12)));
        meanBandDb += bandDb[i] / (float) bandDb.size();
    }

    const float lowTilt = (bandDb[0] + bandDb[1]) * 0.5f;
    const float highTilt = (bandDb[3] + bandDb[4]) * 0.5f;
    const float tilt = highTilt - lowTilt;
    result.suggestion.profileName = tilt < -10.0f ? "Warm and rounded"
                                  : tilt > -2.0f ? "Bright and present"
                                                 : "Balanced and full";

    const float crestDb = juce::Decibels::gainToDecibels (peak / (float) rms, 0.0f);
    result.suggestion.voice = crestDb >= 13.0f ? 0 : (crestDb >= 9.0f ? 1 : 2);
    result.suggestion.gain = juce::jlimit (0.5f, 9.5f, 9.5f - (crestDb - 6.0f) * 0.58f);
    result.suggestion.bass = juce::jlimit (-8.0f, 8.0f, (bandDb[0] - meanBandDb) * 0.62f);
    result.suggestion.middle = juce::jlimit (-8.0f, 8.0f, ((bandDb[1] + bandDb[2]) * 0.5f - meanBandDb) * 0.70f);
    result.suggestion.treble = juce::jlimit (-8.0f, 8.0f, (bandDb[3] - meanBandDb) * 0.68f);
    result.suggestion.presence = juce::jlimit (-6.0f, 6.0f, (bandDb[4] - meanBandDb) * 0.48f);
    return result;
}
