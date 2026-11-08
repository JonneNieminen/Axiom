// axiom-match: compare Axiom with reference recordings.
//   axiom-match analyze <audio>                         spectrum / level report
//   axiom-match render <di.wav> <out.wav> <preset#> [key=value ...]   run a DI recording through an Axiom preset
//        keys: voice gain bass mid treble presence master boost chorus delay room cab gate in (input dB)
//   axiom-match au-list "<AU name>"                     list an Audio Unit's parameters
//   axiom-match au <di.wav> <out.wav> "<AU name>" [Param=0..1 ...]   render through an installed Audio Unit
//   axiom-match diff <a> <b>                            1/3-octave difference of two files, loudness matched
// Audio Units are used only as a listening/measuring yardstick on this machine.
#include "AxiomDsp.h"
#include "Presets.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include <cstdio>

namespace
{
constexpr double pi = 3.14159265358979323846;

struct Clip { juce::AudioBuffer<float> audio; double rate = 44100.0; };

bool load (const juce::File& file, Clip& out)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr) { std::printf ("cannot read %s\n", file.getFullPathName().toRawUTF8()); return false; }
    out.rate = reader->sampleRate;
    out.audio.setSize (2, (int) reader->lengthInSamples);
    reader->read (&out.audio, 0, (int) reader->lengthInSamples, 0, true, true);
    if (reader->numChannels < 2) out.audio.copyFrom (1, 0, out.audio, 0, 0, out.audio.getNumSamples());
    return true;
}

void save (const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    if (writer != nullptr) writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

// 1/3-octave band levels (dB) of the long-term spectrum, using mid (L+R)/2.
constexpr int bandCount = 24;
double bandCentre (int i) { return 50.0 * std::pow (2.0, i / 3.0); }

struct Report
{
    double peakDb = 0, rmsDb = 0, crestDb = 0, widthCorr = 0, centroid = 0;
    double band[bandCount] {};
    double loudDb = 0; // RMS over frames above -50 dB of the loudest
    double frameCrestDb = 0; // median peak/RMS of 20 ms frames (pick attack vs body)
};

Report analyse (const Clip& c)
{
    Report r;
    const int n = c.audio.getNumSamples();
    std::vector<float> mid ((size_t) n);
    double sq = 0, ll = 0, rr = 0, lr = 0;
    float peak = 0;
    for (int i = 0; i < n; ++i)
    {
        const float l = c.audio.getSample (0, i), rt = c.audio.getSample (1, i);
        mid[(size_t) i] = 0.5f * (l + rt);
        sq += (double) mid[(size_t) i] * mid[(size_t) i];
        ll += (double) l * l; rr += (double) rt * rt; lr += (double) l * rt;
        peak = std::max ({ peak, std::abs (l), std::abs (rt) });
    }
    r.peakDb = 20 * std::log10 (std::max (1e-9f, peak));
    r.rmsDb = 10 * std::log10 (std::max (1e-12, sq / n));
    r.crestDb = r.peakDb - r.rmsDb;
    r.widthCorr = lr / std::sqrt (std::max (1e-18, ll * rr));
    {
        const int frame = (int) (0.02 * c.rate);
        std::vector<double> crests; std::vector<std::pair<double, double>> fr;
        double loudest = 0;
        for (int pos = 0; pos + frame <= n; pos += frame)
        {
            double e = 0; float pk = 0;
            for (int i = 0; i < frame; ++i) { const float v = mid[(size_t) (pos + i)]; e += (double) v * v; pk = std::max (pk, std::abs (v)); }
            fr.push_back ({ e / frame, pk }); loudest = std::max (loudest, e / frame);
        }
        for (auto& f : fr) if (f.first > loudest * 1e-4) crests.push_back (20 * std::log10 (f.second / std::sqrt (f.first)));
        if (! crests.empty()) { std::nth_element (crests.begin(), crests.begin() + crests.size() / 2, crests.end()); r.frameCrestDb = crests[crests.size() / 2]; }
    }

    constexpr int order = 14, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2);
    std::vector<double> power (size / 2, 0.0);
    // loudest frame, to gate out silence
    std::vector<double> frameRms;
    for (int pos = 0; pos + size <= n; pos += size / 2)
    {
        double e = 0;
        for (int i = 0; i < size; ++i) e += (double) mid[(size_t) (pos + i)] * mid[(size_t) (pos + i)];
        frameRms.push_back (e / size);
    }
    double top = 0; for (double e : frameRms) top = std::max (top, e);
    int used = 0; double loud = 0; size_t fi = 0;
    for (int pos = 0; pos + size <= n; pos += size / 2, ++fi)
    {
        if (frameRms[fi] < top * 1e-5) continue;
        std::fill (data.begin(), data.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            data[(size_t) i] = mid[(size_t) (pos + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) pi * (float) i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (data.data());
        for (int b = 0; b < size / 2; ++b) power[(size_t) b] += (double) data[(size_t) b] * data[(size_t) b];
        loud += frameRms[fi]; ++used;
    }
    r.loudDb = 10 * std::log10 (std::max (1e-12, loud / std::max (1, used)));
    double totalP = 1e-30, cen = 0;
    for (int b = 1; b < size / 2; ++b) { const double f = b * c.rate / size; totalP += power[(size_t) b]; cen += f * power[(size_t) b]; }
    r.centroid = cen / totalP;
    for (int i = 0; i < bandCount; ++i)
    {
        const double lo = bandCentre (i) / std::pow (2.0, 1.0 / 6.0), hi = bandCentre (i) * std::pow (2.0, 1.0 / 6.0);
        double acc = 0;
        for (int b = 1; b < size / 2; ++b) { const double f = b * c.rate / size; if (f >= lo && f < hi) acc += power[(size_t) b]; }
        r.band[i] = 10 * std::log10 (std::max (acc / totalP, 1e-12));
    }
    return r;
}

void print (const char* name, const Report& r)
{
    std::printf ("%-34s peak %6.1f  rms %6.1f  loud %6.1f  crest %5.1f  frame-crest %4.1f  L/R %4.2f  centroid %5.0f Hz\n", name, r.peakDb, r.rmsDb, r.loudDb, r.crestDb, r.frameCrestDb, r.widthCorr, r.centroid);
}

void printBands (const Report& r)
{
    for (int i = 0; i < bandCount; ++i) std::printf ("%7.0f", bandCentre (i));
    std::printf ("\n");
    for (int i = 0; i < bandCount; ++i) std::printf ("%7.1f", r.band[i]);
    std::printf ("\n");
}

std::unique_ptr<juce::AudioPluginInstance> openAu (const juce::String& name, double rate, int block)
{
    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::AudioUnitPluginFormat());
    auto* format = fm.getFormat (0);
    const juce::StringArray ids = format->searchPathsForPlugins (juce::FileSearchPath(), true, false);
    for (auto& id : ids)
    {
        juce::OwnedArray<juce::PluginDescription> descs;
        format->findAllTypesForFile (descs, id);
        for (auto* d : descs)
        {
            if (! d->name.containsIgnoreCase (name) || d->isInstrument) continue;
            juce::String error;
            auto inst = fm.createPluginInstance (*d, rate, block, error);
            if (inst == nullptr) std::printf ("load failed: %s\n", error.toRawUTF8());
            return inst;
        }
    }
    std::printf ("no Audio Unit matching '%s'\n", name.toRawUTF8());
    return nullptr;
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String cmd = argc > 1 ? argv[1] : "";
    const auto file = [&] (int i) { return juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]); };

    if (cmd == "analyze" && argc > 2)
    {
        Clip c; if (! load (file (2), c)) return 1;
        const auto r = analyse (c);
        print (argv[2], r); printBands (r);
        return 0;
    }
    if (cmd == "render" && argc > 4)
    {
        Clip c; if (! load (file (2), c)) return 1;
        const int idx = juce::jlimit (0, (int) axiom::factoryPresets.size() - 1, std::atoi (argv[4]));
        axiom::AxiomDsp dsp; dsp.prepare (c.rate, 512);
        auto params = axiom::factoryPresets[(size_t) idx].params;
        for (int a = 5; a < argc; ++a)
        {
            const juce::String arg (argv[a]);
            const auto key = arg.upToFirstOccurrenceOf ("=", false, false).trim();
            const float v = arg.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
            if (key == "voice") params.voice = (int) v; else if (key == "gain") params.gain = v; else if (key == "bass") params.bassDb = v;
            else if (key == "mid") params.midDb = v; else if (key == "treble") params.trebleDb = v; else if (key == "presence") params.presenceDb = v;
            else if (key == "master") params.masterDb = v; else if (key == "boost") params.boost = v; else if (key == "chorus") params.chorus = v;
            else if (key == "delay") params.delayMix = v; else if (key == "room") params.room = v; else if (key == "cab") params.cabinet = v > 0.5f;
            else if (key == "gate") params.gateDb = v; else if (key == "in") params.inputDb = v;
            else std::printf ("unknown key '%s'\n", key.toRawUTF8());
        }
        juce::AudioBuffer<float> out (2, c.audio.getNumSamples());
        // Interfaces feed one input; use the left channel as the guitar, like the plugin default.
        for (int i = 0; i < c.audio.getNumSamples(); i += 512)
        {
            const int n = std::min (512, c.audio.getNumSamples() - i);
            dsp.process (c.audio.getReadPointer (0) + i, c.audio.getReadPointer (1) + i, out.getWritePointer (0) + i, out.getWritePointer (1) + i, n, params);
        }
        save (file (3), out, c.rate);
        return 0;
    }
    if (cmd == "au-list" && argc > 2)
    {
        auto inst = openAu (argv[2], 44100.0, 512); if (! inst) return 1;
        std::printf ("%s  (%d params)\n", inst->getName().toRawUTF8(), (int) inst->getParameters().size());
        for (auto* p : inst->getParameters())
            std::printf ("  [%2d] %-28s %.3f  (%s)\n", p->getParameterIndex(), p->getName (40).toRawUTF8(), p->getValue(), p->getText (p->getValue(), 20).toRawUTF8());
        return 0;
    }
    if (cmd == "au" && argc > 4)
    {
        Clip c; if (! load (file (2), c)) return 1;
        auto inst = openAu (argv[4], c.rate, 512); if (! inst) return 1;
        for (int a = 5; a < argc; ++a)
        {
            const juce::String arg (argv[a]);
            const auto key = arg.upToFirstOccurrenceOf ("=", false, false).trim();
            const float value = arg.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
            if (key == "in") { c.audio.applyGain (std::pow (10.0f, value / 20.0f)); continue; }
            bool found = false;
            for (auto* p : inst->getParameters())
                if (p->getName (40).equalsIgnoreCase (key)) { p->setValue (value); found = true; }
            if (! found) std::printf ("unknown parameter '%s'\n", key.toRawUTF8());
        }
        juce::AudioProcessor::BusesLayout layout;
        layout.getMainInputChannelSet() = juce::AudioChannelSet::stereo();
        layout.getMainOutputChannelSet() = juce::AudioChannelSet::stereo();
        inst->setBusesLayout (layout);
        inst->prepareToPlay (c.rate, 512);
        juce::AudioBuffer<float> out (2, c.audio.getNumSamples());
        juce::MidiBuffer midi;
        // Guitar on the left input, mirrored to both sides, as most mono-input amp sims expect.
        for (int i = 0; i < c.audio.getNumSamples(); i += 512)
        {
            const int n = std::min (512, c.audio.getNumSamples() - i);
            juce::AudioBuffer<float> block (2, n);
            block.copyFrom (0, 0, c.audio, 0, i, n);
            block.copyFrom (1, 0, c.audio, 0, i, n);
            inst->processBlock (block, midi);
            out.copyFrom (0, i, block, 0, 0, n);
            out.copyFrom (1, i, block, 1, 0, n);
        }
        save (file (3), out, c.rate);
        return 0;
    }
    if (cmd == "diff" && argc > 3)
    {
        Clip a, b; if (! load (file (2), a) || ! load (file (3), b)) return 1;
        const auto ra = analyse (a), rb = analyse (b);
        print (argv[2], ra); print (argv[3], rb);
        for (int i = 0; i < bandCount; ++i) std::printf ("%7.0f", bandCentre (i));
        std::printf ("\n");
        for (int i = 0; i < bandCount; ++i) std::printf ("%7.1f", rb.band[i] - ra.band[i]);
        std::printf ("   <- second minus first (dB, relative to each file's total)\n");
        return 0;
    }
    std::printf ("usage: see top of tools/match/Main.cpp\n");
    return 1;
}
