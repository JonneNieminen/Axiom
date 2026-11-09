// axiom-render: offline check of the Axiom signal path.
// Usage: axiom-render [output-directory]
#include "AxiomDsp.h"
#include "Presets.h"
#include "PitchDetector.h"
#include "PhysicalTubeStage.h"
#include <chrono>
#include <tuple>
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
    dsp.setPhysicalNoise (false);   // measuring distortion products, not the first stage's hiss
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
    dsp.setPhysicalNoise (false);
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
/** Runs the Physical engine on its own through the same 8x oversampler the plugin uses.
    `perChunk` is called after every 32 oversampled samples (for telemetry). */
template <class Fn>
std::vector<float> runEngine (axiom::physical::PhysicalTubeStage& amp, const std::vector<float>& in, Fn&& perChunk)
{
    juce::dsp::Oversampling<float> os (1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    os.initProcessing (512);
    std::vector<float> out (in.size()), buf (512);
    for (size_t i = 0; i < in.size(); i += 512)
    {
        const int n = (int) std::min<size_t> (512, in.size() - i);
        std::copy (in.begin() + (long) i, in.begin() + (long) i + n, buf.begin());
        float* ch[1] { buf.data() };
        juce::dsp::AudioBlock<float> block (ch, 1, (size_t) n);
        auto up = os.processSamplesUp (block);
        float* u = up.getChannelPointer (0);
        for (size_t k = 0; k < up.getNumSamples(); k += 32)
        {
            amp.process (u + k, (int) std::min<size_t> (32, up.getNumSamples() - k));
            perChunk();
        }
        os.processSamplesDown (block);
        std::copy (buf.begin(), buf.begin() + n, out.begin() + (long) i);
    }
    return out;
}

/** Relative error (dB) of the analytic derivative against central differences, worst case. */
template <class F, class D>
double derivativeError (F&& f, D&& analytic, double x)
{
    const double h = 1.0e-5 * std::max (1.0, std::abs (x));
    const double numeric = (f (x + h) - f (x - h)) / (2.0 * h);
    const double a = analytic (x);
    const double scale = std::max ({ std::abs (numeric), std::abs (a), 1.0e-12 });
    return std::abs (a - numeric) / scale;
}

/** Checks for the Physical engine (circuit-level tube model). Returns the failure count. */
int physicalChecks (const std::vector<float>& guitar, double fs, const juce::File& outDir)
{
    namespace ph = axiom::physical;
    int failures = 0;
    const char* voiceNames[] { "Clean", "Classic", "Modern" };
    const auto fail = [&failures] (bool ok) { if (! ok) { ++failures; std::printf ("  FAIL\n"); } };

    //-- Operating points: what a tech would measure on the bench at idle.
    auto amp = std::make_unique<ph::PhysicalTubeStage>();
    amp->setNoise (false);
    amp->prepare (44100.0 * 8.0, 8);
    std::printf ("\nPhysical engine: idle operating points (44.1 kHz x 8; supply averaged over the mains cycle)\n");
    std::printf ("  %-8s %6s %6s %6s %6s %6s %6s   triodes (Vgk / Vpk)\n", "voice", "plates", "screen", "PI", "pre", "V1", "bias");
    for (int v = 0; v < 3; ++v)
    {
        const auto t = amp->idleTelemetry (v);
        std::printf ("  %-8s", voiceNames[v]);
        for (double x : t.supply) std::printf (" %6.1f", x);
        std::printf (" %6.1f  ", -t.bias);
        bool ok = t.bias > 10.0 && t.bias < 90.0 && t.supply[4] > 200.0;
        for (int k = 0; k < t.triodes; ++k)
        {
            const double vgk = t.preampPorts[(size_t) (2 * k)], vpk = t.preampPorts[(size_t) (2 * k + 1)];
            std::printf (" %5.2f/%-4.0f", vgk, vpk);
            ok = ok && vgk > -6.0 && vgk < 0.3 && vpk > 30.0 && vpk < 450.0;
        }
        std::printf ("\n");
        fail (ok);
    }

    //-- Analytic Jacobians against central differences: the device equations, then the whole
    //   coupled circuits (J = K dI/dv - I) at their operating points.
    {
        double worst = 0.0;
        const ph::TriodeModel tri {};
        for (double vgk = -8.0; vgk <= 2.0; vgk += 0.37)
            for (double vpk = 20.0; vpk <= 420.0; vpk += 37.0)
            {
                const auto r = tri.evaluate<ph::ExactMath> (vgk, vpk);
                worst = std::max (worst, derivativeError ([&] (double x) { return tri.evaluate<ph::ExactMath> (x, vpk).ig; }, [&] (double) { return r.dIg; }, vgk));
                worst = std::max (worst, derivativeError ([&] (double x) { return tri.evaluate<ph::ExactMath> (x, vpk).ip; }, [&] (double) { return r.dIpG; }, vgk));
                worst = std::max (worst, derivativeError ([&] (double x) { return tri.evaluate<ph::ExactMath> (vgk, x).ip; }, [&] (double) { return r.dIpP; }, vpk));
            }
        const ph::GridDiode diode { 2.0e-3, 0.2 };
        for (double v = -60.0; v <= 20.0; v += 0.71)
            worst = std::max (worst, derivativeError ([&] (double x) { return diode (x).i; }, [&] (double x) { return diode (x).di; }, v));

        const auto circuitError = [] (auto& solver, const ph::Netlist& net, auto u)
        {
            ph::Workspace w;
            solver.sys.build (net, 352800.0, w);
            solver.solveDc (net, u);
            const auto p = solver.sys.linearPart (solver.x, u);
            auto v = solver.v;
            for (auto& x : v) x += 0.37;   // off the operating point, so every term is exercised
            typename std::remove_reference_t<decltype (solver)>::Jac j {};
            std::array<double, std::tuple_size_v<decltype (v)>> i {};
            solver.template evaluate<ph::ExactMath> (v, i, j);
            std::array<double, std::tuple_size_v<decltype (v)> * std::tuple_size_v<decltype (v)>> J {};
            solver.buildJacobian (j, J);
            constexpr int n = (int) std::tuple_size_v<decltype (v)>;
            double err = 0.0;
            for (int c = 0; c < n; ++c)
            {
                const double h = 1.0e-5 * std::max (1.0, std::abs (v[(size_t) c]));
                auto vp = v, vm = v;
                vp[(size_t) c] += h; vm[(size_t) c] -= h;
                const auto fp = solver.residual (p, vp), fm = solver.residual (p, vm);
                for (int r = 0; r < n; ++r)
                {
                    const double numeric = (fp[(size_t) r] - fm[(size_t) r]) / (2.0 * h), a = J[(size_t) (r * n + c)];
                    err = std::max (err, std::abs (a - numeric) / std::max ({ std::abs (numeric), std::abs (a), 1.0 }));
                }
            }
            return err;
        };
        const ph::Pots pots {};
        auto modern = std::make_unique<ph::Preamp<2>>();
        auto inverter = std::make_unique<ph::PhaseInverter>();
        worst = std::max (worst, circuitError (*modern, ph::PhysicalTubeStage::preampNetlist (2, pots), std::array<double, 3> { 0.0, 340.0, 345.0 }));
        worst = std::max (worst, circuitError (*inverter, ph::PhysicalTubeStage::phaseInverterNetlist (1, pots), std::array<double, 6> { 0.0, 380.0, -40.0, 0.0, 0.0, 0.0 }));
        std::printf ("\nPhysical engine: analytic Jacobians vs central differences (devices, Modern preamp, phase inverter): worst %.1e relative\n", worst);
        fail (worst < 1.0e-5);
    }

    //-- ngspice cross-check: the same netlists, simulated independently (trapezoidal, step =
    //   the engine's sample period), against the engine's own coupled Newton solves.
    if (std::system ("command -v ngspice > /dev/null 2>&1") == 0)
    {
        std::printf ("\nPhysical engine vs ngspice (same circuit, 20 ms, RMS error relative to the signal)\n");
        const double rate = 352800.0, dur = 0.02;
        const int n = (int) (dur * rate);
        const auto dir = juce::File::createTempFile ("axiom-spice");
        dir.createDirectory();
        for (int which = 0; which < 4; ++which)
        {
            const bool isPi = which == 3;
            const int v = isPi ? 1 : which;
            const double amplitude = isPi ? 2.0 : 0.3;
            std::vector<double> x ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / rate;
                x[(size_t) i] = amplitude * (std::sin (2.0 * pi * 220.0 * t) + 0.4 * std::sin (2.0 * pi * 660.0 * t)) * std::min (1.0, t / 0.001);
            }
            std::string pwl = "PWL(";
            for (int i = 0; i < n; ++i)
                pwl += juce::String (i / rate, 12).toStdString() + " " + juce::String (x[(size_t) i], 12).toStdString() + (i % 6 == 5 ? "\n+ " : " ");
            pwl += ")";
            const ph::Pots pots {};
            const auto net = isPi ? ph::PhysicalTubeStage::phaseInverterNetlist (v, pots) : ph::PhysicalTubeStage::preampNetlist (v, pots);
            const std::vector<std::string> sources = isPi ? std::vector<std::string> { pwl, "DC 380", "DC -40", "DC 0", "DC 0", "DC 0" }
                                                        : std::vector<std::string> { pwl, "DC 340", "DC 345" };
            const auto data = dir.getChildFile ("out" + juce::String (which) + ".txt"), deck = dir.getChildFile ("deck" + juce::String (which) + ".cir");
            deck.replaceWithText (net.spice (sources, ph::TriodeModel {}, ph::GridDiode { 2.0e-3, 0.2 }, 1.0 / rate, dur, data.getFullPathName().toStdString()));
            const auto cmd = "ngspice -b \"" + deck.getFullPathName() + "\" > \"" + deck.getFullPathName() + ".log\" 2>&1";
            std::vector<double> ref;
            if (std::system (cmd.toRawUTF8()) == 0)
            {
                juce::StringArray lines;
                lines.addLines (data.loadFileAsString());
                for (int l = 1; l < lines.size(); ++l)
                {
                    juce::StringArray cols;
                    cols.addTokens (lines[l], " \t", "");
                    cols.removeEmptyStrings();
                    if (cols.size() >= 2) ref.push_back (cols[1].getDoubleValue());
                }
            }
            const auto ours = isPi ? ph::PhysicalTubeStage::runPhaseInverter (v, pots, x, rate, 380.0, 40.0, ph::MathMode::exact)
                                 : ph::PhysicalTubeStage::runPreamp (v, pots, x, rate, 340.0, 345.0, ph::MathMode::exact);
            const size_t m = std::min (ref.size(), ours.size()), a = (size_t) (0.004 * rate);
            double err = 0.0, sig = 1.0e-30, mean = 0.0;
            for (size_t i = a; i < m; ++i) mean += ref[i];
            mean /= (double) std::max<size_t> (1, m - a);
            for (size_t i = a; i < m; ++i)
            {
                err += (ours[i] - ref[i]) * (ours[i] - ref[i]);
                sig += (ref[i] - mean) * (ref[i] - mean);
            }
            const double db = 10.0 * std::log10 (err / sig + 1.0e-30);
            const bool ok = m > (size_t) (n * 0.9) && db < -60.0;
            std::printf ("  %-8s %-15s %6.1f dB%s\n", voiceNames[v], isPi ? "phase inverter" : "preamp", db, ok ? "" : "  FAIL");
            if (! ok) ++failures;
        }
        dir.deleteRecursively();
    }
    else
        std::printf ("\nPhysical engine vs ngspice: skipped (ngspice not installed)\n");

    //-- Supply sag, ripple and solver behaviour on a hard-played riff, directly on the engine.
    std::printf ("\nPhysical engine: supply and solver on a hard-played riff (gain 8; Fast | Exact)\n");
    for (int v = 0; v < 3; ++v)
        for (auto mode : { ph::MathMode::fast, ph::MathMode::exact })
        {
            amp->setVoice (v);
            amp->setMathMode (mode);
            amp->setPots ({ 0.8, 0.5, 0.5, 0.5, 0.5 });
            amp->reset();
            amp->clearStats();
            const double idle = amp->telemetry().supply[0];
            double lowest = 1.0e9;
            runEngine (*amp, guitar, [&] { lowest = std::min (lowest, amp->telemetry().supply[0]); });
            const auto t = amp->telemetry();
            const auto avg = [] (const ph::SolverStats& s) { return (double) s.iterations / (double) std::max<std::uint64_t> (1, s.solves); };
            const double capPct = 100.0 * (double) (t.preamp.capHits + t.phaseInverter.capHits + t.plates.capHits)
                                / (double) std::max<std::uint64_t> (1, t.preamp.solves + t.phaseInverter.solves + t.plates.solves);
            std::printf ("  %-8s %-5s B+ %5.1f -> %5.1f V   evals/solve: preamp %.2f, PI %.2f, plates %.2f   at cap: %.3f%%\n",
                         voiceNames[v], mode == ph::MathMode::fast ? "Fast" : "Exact", idle, lowest,
                         avg (t.preamp), avg (t.phaseInverter), avg (t.plates), capPct);
            if (! (lowest < idle - 1.0) || capPct > 1.0) { std::printf ("  FAIL\n"); ++failures; }
        }
    amp->setMathMode (ph::MathMode::fast);

    //-- Output transformer core: full drive at 30 Hz pushes the flux past saturation; 160 Hz
    //   does not. And tube heating under sustained full power.
    {
        std::printf ("\nPhysical engine: output transformer core and tube heating (Classic, gain 10)\n");
        double fluxAt[2] {};
        amp->setVoice (1);
        amp->setPots ({ 1.0, 0.5, 0.5, 0.5, 0.5 });
        for (double f : { 30.0, 160.0 })
        {
            amp->reset();
            std::vector<float> sine ((size_t) (fs * 1.0));
            for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.4f * (float) std::sin (2.0 * pi * f * (double) i / fs);
            // Steady state only (an abrupt full-power onset gives the flux a decaying DC offset,
            // the mechanism behind transformer inrush). Reported as the AC swing, which is what
            // saturates on low notes, and the mean, a DC magnetisation from the two tubes'
            // unequal average currents.
            double lo = 1.0e9, hi = -1.0e9, sum = 0.0;
            int chunk = 0, counted = 0;
            const int settle = (int) (0.5 * fs * 8.0 / 32.0);
            runEngine (*amp, sine, [&] { if (++chunk > settle) { const double fl = amp->telemetry().flux; lo = std::min (lo, fl); hi = std::max (hi, fl); sum += fl; ++counted; } });
            const auto t = amp->telemetry();
            const double peakFlux = 0.5 * (hi - lo);
            std::printf ("  %5.0f Hz: flux swing %.2f x saturation, DC offset %+.2f\n", f, peakFlux / t.fluxSaturation, sum / std::max (1, counted) / t.fluxSaturation);
            fluxAt[f < 100.0 ? 0 : 1] = peakFlux / t.fluxSaturation;
        }
        amp->reset();
        const double cold = amp->telemetry().temperatureA;
        std::vector<float> loud ((size_t) (fs * 8.0));
        for (size_t i = 0; i < loud.size(); ++i) loud[i] = 0.4f * (float) std::sin (2.0 * pi * 220.0 * (double) i / fs);
        runEngine (*amp, loud, [] {});
        const auto hot = amp->telemetry();
        std::printf ("  8 s at full power: plate dissipation %.1f -> %.1f W (4 s thermal), grid emission %.2f uA\n",
                     cold, hot.temperatureA, hot.emissionA * 1.0e6);
        const bool ok = hot.temperatureA > cold && std::isfinite (hot.emissionA) && fluxAt[0] > 0.6 && fluxAt[1] < 0.5 * fluxAt[0];
        fail (ok);
    }

    //-- Noise and hum at idle: the first stage's thermal/flicker noise and the supply ripple,
    //   with a typical setting and everything maxed. Nothing may oscillate.
    {
        std::printf ("\nPhysical engine: idle noise and hum (no input), dBFS RMS / 100 Hz component\n");
        for (int v = 0; v < 3; ++v)
            for (const bool maxed : { false, true })
            {
                axiom::AxiomParams p;
                p.voice = v; p.engine = 1; p.room = 0;
                if (maxed) { p.gain = 10.0f; p.bassDb = p.midDb = p.trebleDb = p.presenceDb = 12.0f; p.masterDb = 6.0f; }
                axiom::AxiomDsp dsp;
                dsp.prepare (fs, 512);
                std::vector<float> zero ((size_t) (2.0 * fs), 0.0f), l, r;
                render (dsp, zero, l, r, p, 512);
                const size_t from = (size_t) (0.5 * fs);
                const double level = rmsDb (l, from, l.size());
                double re = 0.0, im = 0.0;
                for (size_t i = from; i < l.size(); ++i)
                {
                    const double w = 2.0 * pi * 100.0 * (double) i / fs;
                    re += l[i] * std::cos (w);
                    im += l[i] * std::sin (w);
                }
                const double hum = toDb (2.0 * std::sqrt (re * re + im * im) / (double) (l.size() - from) / std::sqrt (2.0));
                double peak = 0.0;
                for (size_t i = from; i < l.size(); ++i) peak = std::max (peak, (double) std::abs (l[i]));
                const bool ok = std::isfinite (level) && level < (maxed ? -20.0 : -50.0) && toDb (peak) < (maxed ? -6.0 : -30.0);
                std::printf ("  %-8s %-10s %6.1f dBFS, hum %6.1f dBFS%s\n", voiceNames[v], maxed ? "maxed" : "gain 3.5", level, hum, ok ? "" : "  FAIL");
                if (! ok) ++failures;
            }
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

    //-- Fast vs Exact math: the same circuits, two implementations of exp/log/atan.
    //   Sample by sample on a smooth chord. Not on the plucks: a pluck starts with a full-scale
    //   noise burst, and a 100 dB-gain clipping amp turns any difference -- 1e-13 or a 1e-7 V
    //   change of input -- into moved zero crossings of clipped noise. There the spectra must
    //   agree instead.
    {
        std::vector<float> chord (guitar.size());
        for (size_t i = 0; i < chord.size(); ++i)
        {
            const double t = (double) i / fs;
            chord[i] = (float) (0.15 * (std::sin (2.0 * pi * 110.0 * t) + 0.7 * std::sin (2.0 * pi * 164.8 * t) + 0.5 * std::sin (2.0 * pi * 220.0 * t))
                                * std::min (1.0, t / 0.02) * std::exp (-0.4 * t));
        }
        double worst = -400.0, worstBand = 0.0, sensitivity = 0.0;
        auto nudged = guitar;
        nudged[(size_t) (0.05 * fs)] += 1.0e-7f;   // one sample, 1e-7 of full scale, before the first pluck
        for (const auto& preset : axiom::factoryPresets)
        {
            auto p = preset.params;
            p.engine = 1;
            for (int signal = 0; signal < 2; ++signal)
            {
                const auto& in = signal == 0 ? chord : guitar;
                auto q = p;
                axiom::AxiomDsp a, b;
                a.prepare (fs, 512); a.setPhysicalNoise (false);
                b.prepare (fs, 512); b.setPhysicalNoise (false);
                render (a, in, l, r, q, 512);
                q.tubeMath = 1;
                render (b, in, sl, sr, q, 512);
                if (signal == 0)
                {
                    double diff = 0, sig = 1e-30;
                    for (size_t i = 0; i < l.size(); ++i) { diff += (double) (l[i] - sl[i]) * (l[i] - sl[i]); sig += (double) sl[i] * sl[i]; }
                    worst = std::max (worst, 10.0 * std::log10 (diff / sig + 1e-30));
                }
                else
                {
                    const auto fa = analyse (l, r, fs), fb = analyse (sl, sr, fs);
                    for (int k = 0; k < 8; ++k) worstBand = std::max (worstBand, std::abs (fa.bands[k] - fb.bands[k]));
                    // Baseline: the same Fast render with the input nudged by 1e-7.
                    auto f = p;
                    axiom::AxiomDsp c;
                    c.prepare (fs, 512); c.setPhysicalNoise (false);
                    std::vector<float> nl, nr;
                    render (c, nudged, nl, nr, f, 512);
                    const auto fn = analyse (nl, nr, fs);
                    for (int k = 0; k < 8; ++k) sensitivity = std::max (sensitivity, std::abs (fa.bands[k] - fn.bands[k]));
                }
            }
        }
        const bool ok = worst < -80.0 && worstBand <= std::max (0.5, 1.5 * sensitivity);
        std::printf ("\nPhysical engine: Fast vs Exact math: chord %.1f dB (sample difference rel. signal), plucks %.2f dB largest band\n"
                     "  difference (a 1e-7 input nudge alone moves the bands %.2f dB: the attacks are clipped noise)%s\n",
                     worst, worstBand, sensitivity, ok ? "" : "  FAIL");
        if (! ok) ++failures;
    }

    //-- Knob moves rebuild the circuits on the fly; switching engines crossfades. Neither may
    //   click: the steepest sample-to-sample step must be no steeper than without them.
    {
        auto p = axiom::factoryPresets[1].params;
        p.room = 0; p.delayMix = 0; p.engine = 1;
        const auto steepest = [&] (const std::vector<float>& x, size_t from, size_t to)
        {
            double m = 0;
            for (size_t i = std::max<size_t> (1, from); i < to; ++i) m = std::max (m, (double) std::abs (x[i] - x[i - 1]));
            return m;
        };
        std::vector<float> steady;
        {
            axiom::AxiomDsp dsp;
            dsp.prepare (fs, 256);
            dsp.setPhysicalNoise (false);
            render (dsp, guitar, steady, r, p, 256);
        }
        axiom::AxiomDsp dsp;
        dsp.prepare (fs, 256);
        dsp.setPhysicalNoise (false);
        l.assign (guitar.size(), 0.0f);
        r.assign (guitar.size(), 0.0f);
        for (size_t i = 0; i < guitar.size(); i += 256)
        {
            const double t = (double) i / fs;
            auto q = p;
            if (t > 0.3 && t < 1.3)   // sweep every pot down and back up over a second
            {
                const float k = (float) std::abs (std::sin (pi * (t - 0.3)));
                q.gain = p.gain * (1.0f - 0.7f * k);
                q.bassDb = q.midDb = q.trebleDb = q.presenceDb = -12.0f * k;
            }
            dsp.process (guitar.data() + i, nullptr, l.data() + i, r.data() + i, (int) std::min<size_t> (256, guitar.size() - i), q);
        }
        const auto st = analyse (l, r, fs);
        const double sweep = steepest (l, (size_t) (0.3 * fs), (size_t) (1.3 * fs)), ref = steepest (steady, (size_t) (0.1 * fs), (size_t) (2.5 * fs));
        const bool ok = ! st.bad && st.peak <= 1.0 && sweep <= ref * 1.05;
        std::printf ("Physical engine: sweeping every pot while playing, steepest step %.3f (steady: %.3f)%s\n", sweep, ref, ok ? "" : "  FAIL");
        if (! ok) ++failures;
    }
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
        // References: the Standard engine playing throughout, and the Physical engine starting
        // fresh (from idle) at the switch, as it does when it is switched in.
        std::vector<float> steady[2];
        {
            p.engine = 0;
            axiom::AxiomDsp dsp;
            dsp.prepare (fs, 256);
            render (dsp, guitar, steady[0], r, p, 256);
            p.engine = 1;
            axiom::AxiomDsp fresh;
            fresh.prepare (fs, 256);
            fresh.setPhysicalNoise (false);
            auto late = guitar;
            std::fill (late.begin(), late.begin() + (long) switchAt, 0.0f);
            render (fresh, late, steady[1], r, p, 256);
        }
        axiom::AxiomDsp dsp;
        dsp.prepare (fs, 256);
        dsp.setPhysicalNoise (false);
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
                for (int trial = 0; trial < 2; ++trial)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    render (dsp, g, l, r, p, 128);
                    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
                    best = std::min (best, secs);
                }
                std::printf (" %8.2f%%", 100.0 * best / ((double) g.size() / rate));
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
