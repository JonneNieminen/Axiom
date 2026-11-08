// axiom-render: offline check of the Axiom signal path.
// Usage: axiom-render [output-directory]
#include "AxiomDsp.h"
#include "Presets.h"
#include "PitchDetector.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstdio>

namespace
{
constexpr double pi = 3.14159265358979323846;

// Karplus-Strong pluck, a reasonable stand-in for a DI guitar signal.
void pluck (std::vector<float>& out, double fs, double startSec, double freq, double seconds, float level, juce::Random& rng)
{
    const int period = (int) std::round (fs / freq);
    std::vector<float> line ((size_t) period);
    for (auto& v : line) v = rng.nextFloat() * 2.0f - 1.0f;
    float prev = 0.0f;
    const int start = (int) (startSec * fs), total = (int) (seconds * fs);
    for (int i = 0; i < total && start + i < (int) out.size(); ++i)
    {
        const size_t idx = (size_t) (i % period);
        const float cur = line[idx];
        const float next = 0.4985f * (cur + prev);
        prev = cur;
        line[idx] = next;
        out[(size_t) (start + i)] += cur * level;
    }
}

std::vector<float> makeGuitar (double fs)
{
    std::vector<float> sig ((size_t) (9.0 * fs), 0.0f);
    juce::Random rng (1234);
    // E5 power chord strum, then single notes
    const double e2 = 82.41, b2 = 123.47, e3 = 164.81;
    pluck (sig, fs, 0.10, e2, 2.0, 1.0f, rng);
    pluck (sig, fs, 0.13, b2, 2.0, 0.9f, rng);
    pluck (sig, fs, 0.16, e3, 2.0, 0.8f, rng);
    pluck (sig, fs, 2.60, 220.0, 1.2, 1.0f, rng);
    pluck (sig, fs, 3.90, 329.63, 1.2, 1.0f, rng);
    pluck (sig, fs, 5.20, 493.88, 1.2, 1.0f, rng);
    pluck (sig, fs, 6.50, 659.26, 2.0, 1.0f, rng);
    float peak = 0.0f;
    for (auto v : sig) peak = std::max (peak, std::abs (v));
    for (auto& v : sig) v *= 0.3f / peak;   // typical DI level (about -10 dBFS)
    return sig;
}

double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-9)); }

struct Stats { double peak = 0, rms = 0, lrDiff = 0, dc = 0; bool bad = false; double bands[8] {}; };

Stats analyse (const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    Stats s;
    double sq = 0, diff = 0, sum = 0;
    const size_t from = (size_t) (0.1 * fs), n = l.size();
    for (size_t i = 0; i < n; ++i)
    {
        if (! std::isfinite (l[i]) || ! std::isfinite (r[i])) s.bad = true;
        s.peak = std::max ({ s.peak, (double) std::abs (l[i]), (double) std::abs (r[i]) });
        if (i >= from) { sq += (double) l[i] * l[i]; diff += std::abs (l[i] - r[i]); sum += l[i]; }
    }
    const double count = (double) (n - from);
    s.rms = std::sqrt (sq / count);
    s.lrDiff = diff / count;
    s.dc = sum / count;

    // Averaged spectrum in octave-ish bands
    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2);
    double power[size / 2] {};
    int frames = 0;
    for (size_t pos = from; pos + size <= n; pos += size / 2)
    {
        std::fill (data.begin(), data.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            data[(size_t) i] = l[pos + (size_t) i] * (0.5f - 0.5f * std::cos (2.0f * (float) pi * (float) i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (data.data());
        for (int b = 0; b < size / 2; ++b) power[b] += (double) data[(size_t) b] * data[(size_t) b];
        ++frames;
    }
    const double edges[9] { 20, 100, 250, 500, 1000, 2000, 4000, 8000, 20000 };
    double total = 1e-30;
    for (int b = 1; b < size / 2; ++b) total += power[b];
    for (int band = 0; band < 8; ++band)
    {
        double acc = 0;
        for (int b = 1; b < size / 2; ++b)
        {
            const double f = b * fs / size;
            if (f >= edges[band] && f < edges[band + 1]) acc += power[b];
        }
        s.bands[band] = 10.0 * std::log10 (std::max (acc / total, 1e-12));
    }
    (void) frames;
    return s;
}

/** Fraction of output energy (below 20 kHz) that is not at a harmonic of the input tone. */
double aliasRatio (axiom::AxiomParams p, double fs, double f0)
{
    axiom::AxiomDsp dsp;
    dsp.prepare (fs, 512);
    const int n = (int) (fs * 2.0);
    std::vector<float> in ((size_t) n), outL ((size_t) n), outR ((size_t) n);
    for (int i = 0; i < n; ++i) in[(size_t) i] = 0.3f * (float) std::sin (2.0 * pi * f0 * i / fs);
    p.room = 0; p.chorus = 0; p.delayMix = 0;
    for (int i = 0; i < n; i += 512)
        dsp.process (in.data() + i, nullptr, outL.data() + i, outR.data() + i, std::min (512, n - i), p);

    constexpr int order = 15, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    const int start = n - size;
    for (int i = 0; i < size; ++i)
        data[(size_t) i] = outL[(size_t) (start + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) pi * (float) i / (size - 1)));
    fft.performFrequencyOnlyForwardTransform (data.data());
    double total = 1e-30, harmonic = 0;
    for (int b = 1; b < size / 2; ++b)
    {
        const double f = b * fs / size;
        if (f > 20000.0) break;
        const double pw = (double) data[(size_t) b] * data[(size_t) b];
        total += pw;
        const double k = std::round (f / f0);
        if (k >= 1 && std::abs (f - k * f0) < 6.0 * fs / size) harmonic += pw;
    }
    return 10.0 * std::log10 (std::max ((total - harmonic) / total, 1e-12));
}
/** Total harmonic distortion (percent) of a sine at the given input amplitude. */
double thdPercent (axiom::AxiomParams p, double fs, double f0, float amplitude)
{
    axiom::AxiomDsp dsp;
    dsp.prepare (fs, 512);
    const int n = (int) (fs * 1.5);
    std::vector<float> in ((size_t) n), outL ((size_t) n), outR ((size_t) n);
    for (int i = 0; i < n; ++i) in[(size_t) i] = amplitude * (float) std::sin (2.0 * pi * f0 * i / fs);
    p.room = 0; p.chorus = 0; p.delayMix = 0; p.cabinet = false;
    for (int i = 0; i < n; i += 512)
        dsp.process (in.data() + i, nullptr, outL.data() + i, outR.data() + i, std::min (512, n - i), p);
    constexpr int order = 15, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (int i = 0; i < size; ++i)
        data[(size_t) i] = outL[(size_t) (n - size + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) pi * (float) i / (size - 1)));
    fft.performFrequencyOnlyForwardTransform (data.data());
    const auto binPower = [&] (double f)
    {
        const int centre = (int) std::round (f * size / fs);
        double acc = 0;
        for (int b = centre - 3; b <= centre + 3; ++b)
            if (b > 0 && b < size / 2) acc += (double) data[(size_t) b] * data[(size_t) b];
        return acc;
    };
    const double fundamental = binPower (f0);
    double harmonics = 0;
    for (int k = 2; k <= 12 && k * f0 < fs * 0.45; ++k) harmonics += binPower (k * f0);
    return 100.0 * std::sqrt (harmonics / std::max (fundamental, 1e-30));
}
} // namespace

int main (int argc, char** argv)
{
    const juce::File outDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File();
    if (outDir != juce::File()) outDir.createDirectory();

    const double fs = 48000.0;
    const auto guitar = makeGuitar (fs);
    const std::vector<float> silent (guitar.size(), 0.0f);
    int failures = 0;

    std::printf ("%-30s %7s %7s  bands (dB rel. total: <100 250 500 1k 2k 4k 8k >8k)  L/R\n", "preset", "peak", "rms");
    for (const auto& preset : axiom::factoryPresets)
    {
        axiom::AxiomDsp dsp;
        dsp.prepare (fs, 512);
        std::vector<float> l (guitar.size()), r (guitar.size());
        // Single-input interface: guitar on input 1, nothing on input 2.
        for (size_t i = 0; i < guitar.size(); i += 512)
        {
            const int n = (int) std::min<size_t> (512, guitar.size() - i);
            dsp.process (guitar.data() + i, silent.data() + i, l.data() + i, r.data() + i, n, preset.params);
        }
        const auto s = analyse (l, r, fs);
        std::printf ("%-30s %6.1f  %6.1f ", preset.name, toDb (s.peak), toDb (s.rms));
        for (double b : s.bands) std::printf ("%5.0f", b);
        std::printf ("  %.4f%s\n", s.lrDiff, s.bad ? "  NaN!" : "");
        if (s.bad || s.peak > 1.0 || std::abs (s.dc) > 0.01) ++failures;

        if (outDir != juce::File())
        {
            juce::WavAudioFormat wav;
            auto file = outDir.getChildFile (juce::String (preset.name).replaceCharacters (" ()", "___") + ".wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (fs)
                                                                                       .withNumChannels (2)
                                                                                       .withBitsPerSample (24));
            if (writer != nullptr)
            {
                const float* chans[2] { l.data(), r.data() };
                writer->writeFromFloatArrays (chans, 2, (int) l.size());
            }
        }
    }

    std::printf ("\nInharmonic (aliasing) energy, 3.1 kHz tone, 48 kHz, dB rel. total\n");
    const char* names[] { "Clean", "Classic", "Modern" };
    for (int v = 0; v < 3; ++v)
    {
        axiom::AxiomParams p; p.voice = v; p.gain = 9.0f; p.boost = 0.0f;
        std::printf ("  %-8s gain 9: %.1f dB\n", names[v], aliasRatio (p, fs, 3100.0));
    }

    std::printf ("\nDynamics: THD %% of a 330 Hz sine (no cabinet) as the guitar volume comes up\n");
    std::printf ("  %-22s input: 0.02    0.05    0.1     0.2     0.4\n", "");
    for (const auto& preset : axiom::factoryPresets)
    {
        std::printf ("  %-26s", preset.name);
        for (float a : { 0.02f, 0.05f, 0.1f, 0.2f, 0.4f })
            std::printf ("%7.1f ", thdPercent (preset.params, fs, 330.0, a));
        std::printf ("\n");
    }

    // Noise gate: a short note, then interface hiss peaking at -72 dBFS. With the gate on, the
    // hiss must be gone once the note has died; the note itself must come through untouched.
    {
        std::vector<float> sig ((size_t) (4.0 * fs), 0.0f);
        juce::Random rng (99);
        pluck (sig, fs, 0.05, 110.0, 0.6, 0.3f, rng);
        const float hiss = (float) std::pow (10.0, -72.0 / 20.0);
        for (auto& v : sig) v += hiss * (rng.nextFloat() * 2.0f - 1.0f);
        const auto run = [&] (float gateDb, double fromSec, double toSec)
        {
            auto p = axiom::factoryPresets[1].params;  // high gain makes hiss loud
            p.gateDb = gateDb; p.room = 0; p.delayMix = 0;
            axiom::AxiomDsp dsp;
            dsp.prepare (fs, 512);
            std::vector<float> l (sig.size()), r (sig.size());
            for (size_t i = 0; i < sig.size(); i += 512)
                dsp.process (sig.data() + i, nullptr, l.data() + i, r.data() + i, (int) std::min<size_t> (512, sig.size() - i), p);
            double sq = 0;
            const auto a = (size_t) (fromSec * fs), b = (size_t) (toSec * fs);
            for (size_t i = a; i < b; ++i) sq += (double) l[i] * l[i];
            return toDb (std::sqrt (sq / (double) (b - a)));
        };
        const double noteOff = run (-80.0f, 0.1, 0.5), noteOn = run (-62.0f, 0.1, 0.5);
        const double hissOff = run (-80.0f, 3.0, 4.0), hissOn = run (-62.0f, 3.0, 4.0);
        const bool ok = hissOn < -80.0 && hissOn < hissOff - 30.0 && std::abs (noteOn - noteOff) < 1.0;
        std::printf ("\nNoise gate (George Lynch, -62 dB): note %.1f -> %.1f dB, hiss %.1f -> %.1f dB%s\n",
                     noteOff, noteOn, hissOff, hissOn, ok ? "" : "  FAIL");
        if (! ok) ++failures;
    }

    // Tuner: Karplus-Strong plucks have a fundamental of fs / (period + 0.5).
    {
        double worst = 0;
        for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (double f : { 73.42, 82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 659.26, 1046.5 })
            {
                std::vector<float> sig ((size_t) rate, 0.0f);
                juce::Random rng (7);
                pluck (sig, rate, 0.0, f, 1.0, 0.3f, rng);
                const double expected = rate / ((double) std::round (rate / f) + 0.5);
                const auto reading = axiom::detectPitch (sig.data() + (size_t) (0.2 * rate), axiom::pitchWindowFor (rate), rate);
                const double cents = reading.hz > 0 ? 1200.0 * std::log2 (reading.hz / expected) : 1.0e9;
                worst = std::max (worst, std::abs (cents));
                if (std::abs (cents) > 3.0)
                {
                    std::printf ("FAIL tuner %.2f Hz @ %.0f Hz: read %.2f Hz\n", expected, rate, reading.hz);
                    ++failures;
                }
            }
        std::printf ("Tuner: worst error %.2f cents over 9 notes at 44.1, 48, 96 and 192 kHz\n", worst);
    }

    // Sample-rate safety: every preset at common rates must stay finite and bounded.
    for (double rate : { 44100.0, 88200.0, 96000.0, 192000.0 })
        for (const auto& preset : axiom::factoryPresets)
        {
            axiom::AxiomDsp dsp;
            dsp.prepare (rate, 256);
            const auto g = makeGuitar (rate);
            std::vector<float> l (g.size()), r (g.size());
            for (size_t i = 0; i < g.size(); i += 256)
                dsp.process (g.data() + i, nullptr, l.data() + i, r.data() + i, (int) std::min<size_t> (256, g.size() - i), preset.params);
            const auto s = analyse (l, r, rate);
            if (s.bad || s.peak > 1.0) { std::printf ("FAIL %s @ %.0f Hz\n", preset.name, rate); ++failures; }
        }
    std::printf ("\n%s\n", failures == 0 ? "All checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
