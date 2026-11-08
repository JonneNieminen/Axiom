// axiom-render: offline check of the Axiom signal path.
// Usage: axiom-render [output-directory]
#include "AxiomDsp.h"
#include "Presets.h"
#include "PitchDetector.h"
#include "PhysicalTubeStage.h"
#include <chrono>
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
void render (axiom::AxiomDsp& dsp, const std::vector<float>& in, std::vector<float>& l, std::vector<float>& r, const axiom::AxiomParams& p, int block)
{
    l.assign (in.size(), 0.0f);
    r.assign (in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += (size_t) block)
        dsp.process (in.data() + i, nullptr, l.data() + i, r.data() + i, (int) std::min<size_t> ((size_t) block, in.size() - i), p);
}

double rmsDb (const std::vector<float>& x, size_t from, size_t to)
{
    double sq = 0;
    for (size_t i = from; i < to; ++i) sq += (double) x[i] * x[i];
    return toDb (std::sqrt (sq / (double) std::max<size_t> (1, to - from)));
}

/** Checks for the Physical engine (component-level tube model). Returns the failure count. */
int physicalChecks (const std::vector<float>& guitar, double fs, const juce::File& outDir)
{
    int failures = 0;
    const char* voiceNames[] { "Clean", "Classic", "Modern" };

    //-- Operating points: what a tech would measure on the bench at idle.
    std::printf ("\nPhysical engine: idle operating points (44.1 kHz x 8)\n");
    std::printf ("  %-8s %6s %6s %6s %6s %6s   %6s   plates (V)\n", "voice", "B+1", "B+2", "PI", "pre", "V1", "bias");
    for (int v = 0; v < 3; ++v)
    {
        axiom::physical::PhysicalTubeStage amp;
        amp.setVoice (v);
        amp.prepare (44100.0 * 8.0);
        std::printf ("  %-8s", voiceNames[v]);
        for (int n = 0; n < axiom::physical::PhysicalTubeStage::numSupplyNodes; ++n)
            std::printf (" %6.1f", amp.supplyVoltage (n));
        std::printf ("   %6.1f  ", -amp.biasVoltage());
        bool ok = std::isfinite (amp.biasVoltage()) && amp.biasVoltage() > 5.0 && amp.supplyVoltage (4) > 100.0;
        for (int st = 0; st < amp.numStages(); ++st)
        {
            const double vp = amp.idlePlateVoltage (st);
            std::printf (" %6.1f", vp);
            ok = ok && vp > 20.0 && vp < amp.supplyVoltage (4) + 1.0;
        }
        std::printf ("%s\n", ok ? "" : "  FAIL");
        if (! ok) ++failures;
    }

    //-- Sag and Newton-Raphson behaviour under a hard-played riff, directly on the tube model.
    std::printf ("\nPhysical engine: supply sag and Newton-Raphson (gain 8, guitar riff, Fast | Exact)\n");
    for (int v = 0; v < 3; ++v)
        for (auto mode : { axiom::physical::MathMode::fast, axiom::physical::MathMode::exact })
        {
            axiom::physical::PhysicalTubeStage amp;
            amp.setVoice (v);
            amp.prepare (fs * 8.0);
            amp.setGain (0.8);
            amp.setMathMode (mode);
            juce::dsp::Oversampling<float> os (1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
            os.initProcessing (512);
            std::vector<float> buf (512);
            double minPlate = 1e9, minPre = 1e9;
            const double idlePlate = amp.supplyVoltage (0), idlePre = amp.supplyVoltage (3);
            for (size_t i = 0; i < guitar.size(); i += 512)
            {
                const int n = (int) std::min<size_t> (512, guitar.size() - i);
                std::copy (guitar.begin() + (long) i, guitar.begin() + (long) i + n, buf.begin());
                float* ch[1] { buf.data() };
                juce::dsp::AudioBlock<float> block (ch, 1, (size_t) n);
                auto up = os.processSamplesUp (block);
                float* u = up.getChannelPointer (0);
                for (size_t k = 0; k < up.getNumSamples(); k += 32)
                {
                    amp.process (u + k, (int) std::min<size_t> (32, up.getNumSamples() - k), [] (double x) { return x; });
                    minPlate = std::min (minPlate, amp.supplyVoltage (0));
                    minPre = std::min (minPre, amp.supplyVoltage (3));
                }
                os.processSamplesDown (block);
            }
            const auto pre = amp.preampStats(), pow = amp.powerStats();
            const double avgPre = (double) pre.iterations / (double) std::max<std::uint64_t> (1, pre.solves);
            const double avgPow = (double) pow.iterations / (double) std::max<std::uint64_t> (1, pow.solves);
            const double capPct = 100.0 * (double) (pre.capHits + pow.capHits) / (double) std::max<std::uint64_t> (1, pre.solves + pow.solves);
            std::printf ("  %-8s %-5s sag: B+ %5.1f -> %5.1f V, preamp %5.1f -> %5.1f V   evals/solve: preamp %.2f, power %.2f   at cap: %.3f%%\n",
                         voiceNames[v], mode == axiom::physical::MathMode::fast ? "Fast" : "Exact",
                         idlePlate, minPlate, idlePre, minPre, avgPre, avgPow, capPct);
            if (! (minPlate < idlePlate - 1.0) || capPct > 1.0) { std::printf ("  FAIL\n"); ++failures; }
        }

    //-- The factory presets through the Physical engine.
    std::printf ("\nPhysical engine (Fast): factory presets          peak     rms  bands (dB rel. total: <100 250 500 1k 2k 4k 8k >8k)  vs Standard\n");
    std::vector<float> l, r, sl, sr;
    for (const auto& preset : axiom::factoryPresets)
    {
        auto p = preset.params;
        axiom::AxiomDsp ref;
        ref.prepare (fs, 512);
        render (ref, guitar, sl, sr, p, 512);
        p.engine = 1;
        axiom::AxiomDsp dsp;
        dsp.prepare (fs, 512);
        render (dsp, guitar, l, r, p, 512);
        const auto s = analyse (l, r, fs), sref = analyse (sl, sr, fs);
        std::printf ("  %-30s           %6.1f  %6.1f ", preset.name, toDb (s.peak), toDb (s.rms));
        for (double b : s.bands) std::printf ("%5.0f", b);
        std::printf ("  %+5.1f dB%s\n", toDb (s.rms) - toDb (sref.rms), s.bad ? "  NaN!" : "");
        if (s.bad || s.peak > 1.0 || std::abs (s.dc) > 0.01) ++failures;

        if (outDir != juce::File())
        {
            juce::WavAudioFormat wav;
            auto file = outDir.getChildFile (juce::String (preset.name).replaceCharacters (" ()", "___") + "_physical.wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (fs).withNumChannels (2).withBitsPerSample (24));
            if (writer != nullptr)
            {
                const float* chans[2] { l.data(), r.data() };
                writer->writeFromFloatArrays (chans, 2, (int) l.size());
            }
        }
    }

    std::printf ("\nPhysical engine: aliasing, 3.1 kHz tone, 48 kHz, gain 9\n");
    for (int v = 0; v < 3; ++v)
    {
        axiom::AxiomParams p; p.voice = v; p.gain = 9.0f; p.engine = 1;
        std::printf ("  %-8s %.1f dB\n", voiceNames[v], aliasRatio (p, fs, 3100.0));
    }

    std::printf ("\nPhysical engine: THD %% of a 330 Hz sine (no cabinet) as the guitar volume comes up\n");
    std::printf ("  %-22s input: 0.02    0.05    0.1     0.2     0.4\n", "");
    for (const auto& preset : axiom::factoryPresets)
    {
        auto p = preset.params;
        p.engine = 1;
        std::printf ("  %-26s", preset.name);
        for (float a : { 0.02f, 0.05f, 0.1f, 0.2f, 0.4f })
            std::printf ("%7.1f ", thdPercent (p, fs, 330.0, a));
        std::printf ("\n");
    }

    //-- Stability: no input, everything up, at several rates. Negative feedback around a
    //   reactive load must not oscillate, and the idle amp must be silent.
    {
        double worst = -400.0;
        for (double rate : { 44100.0, 96000.0, 192000.0 })
            for (int v = 0; v < 3; ++v)
            {
                axiom::AxiomParams p; p.voice = v; p.gain = 10.0f; p.boost = 100.0f; p.engine = 1; p.room = 0; p.masterDb = 6.0f;
                p.bassDb = p.trebleDb = p.presenceDb = 12.0f;
                axiom::AxiomDsp dsp;
                dsp.prepare (rate, 256);
                std::vector<float> zero ((size_t) (2.0 * rate), 0.0f);
                render (dsp, zero, l, r, p, 256);
                const double level = rmsDb (l, (size_t) (0.5 * rate), l.size());
                worst = std::max (worst, level);
                bool finite = true;
                for (auto x : l) finite = finite && std::isfinite (x);
                if (level > -100.0 || ! finite) { std::printf ("FAIL physical idle noise %s @ %.0f Hz: %.1f dBFS\n", voiceNames[v], rate, level); ++failures; }
            }
        std::printf ("\nPhysical engine: idle output, everything maxed, no input: worst %.1f dBFS\n", worst);
    }

    //-- Fast vs Exact math: the same circuit, two implementations of exp/log/atan (and a
    //   4- vs 6-iteration cap on the triode solve). Compared as the RMS of the difference
    //   relative to the signal: a heavily saturated amp is a sensitive system, so single
    //   samples can differ on the edge of a clipped transient, but the sound must not.
    {
        double worst = -400.0;
        for (const auto& preset : axiom::factoryPresets)
        {
            auto p = preset.params;
            p.engine = 1;
            axiom::AxiomDsp a, b;
            a.prepare (fs, 512);
            b.prepare (fs, 512);
            render (a, guitar, l, r, p, 512);
            p.tubeMath = 1;
            render (b, guitar, sl, sr, p, 512);
            double diff = 0, sig = 1e-30;
            for (size_t i = 0; i < l.size(); ++i)
            {
                diff += (double) (l[i] - sl[i]) * (l[i] - sl[i]);
                sig += (double) sl[i] * sl[i];
            }
            worst = std::max (worst, 10.0 * std::log10 (diff / sig + 1e-30));
        }
        const bool ok = worst < -60.0;
        std::printf ("Physical engine: Fast vs Exact math, difference relative to the signal (worst preset): %.1f dB%s\n", worst, ok ? "" : "  FAIL");
        if (! ok) ++failures;
    }

    //-- Switching engines mid-note crossfades instead of clicking: the steepest sample-to-sample
    //   step around the switch must be no steeper than the steepest either engine makes on its
    //   own during the same passage.
    {
        auto p = axiom::factoryPresets[1].params;
        p.room = 0; p.delayMix = 0;
        const size_t switchAt = (size_t) (0.5 * fs), window = (size_t) (0.05 * fs);
        const auto steepest = [&] (const std::vector<float>& x)
        {
            double m = 0;
            for (size_t i = switchAt - window; i < switchAt + window; ++i) m = std::max (m, (double) std::abs (x[i] - x[i - 1]));
            return m;
        };
        std::vector<float> steady[2];
        for (int engine = 0; engine < 2; ++engine)
        {
            p.engine = engine;
            axiom::AxiomDsp dsp;
            dsp.prepare (fs, 256);
            render (dsp, guitar, steady[engine], r, p, 256);
        }
        axiom::AxiomDsp dsp;
        dsp.prepare (fs, 256);
        l.assign (guitar.size(), 0.0f);
        r.assign (guitar.size(), 0.0f);
        for (size_t i = 0; i < guitar.size(); i += 256)
        {
            p.engine = i >= switchAt && i < (size_t) (1.5 * fs) ? 1 : 0;
            dsp.process (guitar.data() + i, nullptr, l.data() + i, r.data() + i, (int) std::min<size_t> (256, guitar.size() - i), p);
        }
        const double jump = steepest (l), reference = std::max (steepest (steady[0]), steepest (steady[1]));
        const auto st = analyse (l, r, fs);
        const bool ok = ! st.bad && st.peak <= 1.0 && jump <= reference * 1.05;
        std::printf ("Physical engine: switching engines mid-note, steepest step %.3f (either engine alone: %.3f)%s\n", jump, reference, ok ? "" : "  FAIL");
        if (! ok) ++failures;
    }

    //-- Sample-rate safety for the Physical engine.
    for (double rate : { 44100.0, 88200.0, 96000.0, 192000.0 })
        for (const auto& preset : axiom::factoryPresets)
        {
            auto p = preset.params;
            p.engine = 1;
            axiom::AxiomDsp dsp;
            dsp.prepare (rate, 256);
            const auto g = makeGuitar (rate);
            render (dsp, g, l, r, p, 256);
            const auto s = analyse (l, r, rate);
            if (s.bad || s.peak > 1.0) { std::printf ("FAIL physical %s @ %.0f Hz\n", preset.name, rate); ++failures; }
        }

    //-- CPU: single core, 44.1 kHz, 128-sample blocks, the whole plugin signal path.
    std::printf ("\nCPU, %% of one core in real time (44.1 kHz, 128-sample blocks, whole signal path)\n");
    std::printf ("  %-8s %9s %9s %9s\n", "voice", "Standard", "Phys Fast", "Phys Exact");
    {
        const double rate = 44100.0;
        const auto g = makeGuitar (rate);
        std::vector<float> in;
        for (int rep = 0; rep < 2; ++rep) in.insert (in.end(), g.begin(), g.end());
        for (int v = 0; v < 3; ++v)
        {
            std::printf ("  %-8s", voiceNames[v]);
            for (int mode = 0; mode < 3; ++mode)
            {
                axiom::AxiomParams p; p.voice = v; p.gain = 6.0f; p.room = 16.0f;
                p.engine = mode == 0 ? 0 : 1;
                p.tubeMath = mode == 2 ? 1 : 0;
                axiom::AxiomDsp dsp;
                dsp.prepare (rate, 128);
                render (dsp, g, l, r, p, 128);  // warm up caches and branch predictors
                double best = 1e9;
                for (int trial = 0; trial < 3; ++trial)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    render (dsp, in, l, r, p, 128);
                    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
                    best = std::min (best, secs);
                }
                std::printf (" %8.2f%%", 100.0 * best / ((double) in.size() / rate));
            }
            std::printf ("\n");
        }
    }
    return failures;
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
    failures += physicalChecks (guitar, fs, outDir);

    std::printf ("\n%s\n", failures == 0 ? "All checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
