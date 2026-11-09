#include "AxiomDsp.h"
#include "PhysicalTubeStage.h"
#include <array>
#include <cmath>

namespace axiom
{
namespace
{
constexpr double pi = 3.14159265358979323846;

double dbToLin (double db) { return std::pow (10.0, db / 20.0); }

struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

Coeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

double safeFreq (double fs, double f) { return std::min (f, fs * 0.45); }

Coeffs lowPass (double fs, double f, double q)
{
    const double w = 2.0 * pi * safeFreq (fs, f) / fs, c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
    return normalise ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Coeffs highPass (double fs, double f, double q)
{
    const double w = 2.0 * pi * safeFreq (fs, f) / fs, c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
    return normalise ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Coeffs peak (double fs, double f, double q, double db)
{
    const double a = std::pow (10.0, db / 40.0), w = 2.0 * pi * safeFreq (fs, f) / fs;
    const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
    return normalise (1 + alpha * a, -2 * c, 1 - alpha * a, 1 + alpha / a, -2 * c, 1 - alpha / a);
}

Coeffs lowShelf (double fs, double f, double q, double db)
{
    const double a = std::pow (10.0, db / 40.0), w = 2.0 * pi * safeFreq (fs, f) / fs;
    const double c = std::cos (w), beta = 2.0 * std::sqrt (a) * (std::sin (w) / (2.0 * q));
    return normalise (a * ((a + 1) - (a - 1) * c + beta), 2 * a * ((a - 1) - (a + 1) * c),
                      a * ((a + 1) - (a - 1) * c - beta), (a + 1) + (a - 1) * c + beta,
                      -2 * ((a - 1) + (a + 1) * c), (a + 1) + (a - 1) * c - beta);
}

Coeffs highShelf (double fs, double f, double q, double db)
{
    const double a = std::pow (10.0, db / 40.0), w = 2.0 * pi * safeFreq (fs, f) / fs;
    const double c = std::cos (w), beta = 2.0 * std::sqrt (a) * (std::sin (w) / (2.0 * q));
    return normalise (a * ((a + 1) + (a - 1) * c + beta), -2 * a * ((a - 1) + (a + 1) * c),
                      a * ((a + 1) + (a - 1) * c - beta), (a + 1) - (a - 1) * c + beta,
                      2 * ((a - 1) - (a + 1) * c), (a + 1) - (a - 1) * c - beta);
}

/** Double-precision transposed direct form II. Double matters for low corner frequencies at 4x rate. */
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (const Coeffs& c) { b0 = c.b0; b1 = c.b1; b2 = c.b2; a1 = c.a1; a2 = c.a2; }
    void reset() { z1 = z2 = 0; }
    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct OnePole
{
    double b0 = 1, b1 = 0, a1 = 0, z = 0;
    void setHighPass (double fs, double f)
    {
        const double k = std::tan (pi * safeFreq (fs, f) / fs), n = 1.0 / (1.0 + k);
        b0 = n; b1 = -n; a1 = (k - 1.0) * n;
    }
    void setLowPass (double fs, double f)
    {
        const double k = std::tan (pi * safeFreq (fs, f) / fs), n = 1.0 / (1.0 + k);
        b0 = k * n; b1 = k * n; a1 = (k - 1.0) * n;
    }
    void reset() { z = 0; }
    double process (double x)
    {
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }
};

/** One amp personality. Gain stages are cascaded asymmetric triode-style saturators with
    coupling high-passes and Miller-style low-passes between them. */
struct VoiceConfig
{
    double driveLoDb, driveHiDb;     // gain-knob range, applied before the first stage
    int stages;
    double preHpHz;                  // tightens the low end before clipping
    double emphHz, emphDb;           // pre-clip brightness
    double stageGainDb[3];
    double bias[3];                  // asymmetry per stage (even harmonics)
    double interHpHz[3], interLpHz[3];
    double bassHz, midHz, midQ, trebleHz, presenceHz;
    double powerDrive, sag, outDb;
};

const std::array<VoiceConfig, 3> voices {{
    // Clean: lots of headroom, glassy, gentle compression when pushed
    { 0.0, 20.0, 1, 55.0, 2000.0, 2.5, { 0.0, 0.0, 0.0 }, { 0.04, 0.0, 0.0 },
      { 40.0, 40.0, 40.0 }, { 12000.0, 12000.0, 12000.0 },
      110.0, 480.0, 0.7, 3200.0, 5200.0, 0.55, 0.25, -0.5 },
    // Classic: plexi-style crunch that cleans up with the guitar volume
    { 8.0, 42.0, 2, 65.0, 2400.0, 3.0, { 0.0, 6.0, 0.0 }, { 0.16, 0.10, 0.0 },
      { 50.0, 85.0, 85.0 }, { 8500.0, 6800.0, 6800.0 },
      100.0, 650.0, 0.8, 2600.0, 4300.0, 0.9, 0.32, -10.0 },
    // Modern: tight, hot-rodded high gain
    { 10.0, 46.0, 3, 75.0, 2800.0, 2.0, { 0.0, 6.0, 4.0 }, { 0.14, 0.10, 0.06 },
      { 45.0, 70.0, 100.0 }, { 7800.0, 6800.0, 6000.0 },
      115.0, 720.0, 1.0, 2800.0, 4000.0, 0.85, 0.12, -17.0 },
}};

double triode (double x, double bias)
{
    return std::tanh (x + bias) - std::tanh (bias);
}

float softLimit (float x)
{
    constexpr float knee = 0.85f;
    const float a = std::abs (x);
    if (a <= knee)
        return x;
    return std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), x);
}

/** Noise gate keyed on the guitar input, like a pedal in front of the amp. A peak detector
    with hysteresis and a hold time decides open or closed; the gain itself moves smoothly, so
    a pick attack opens it in about a millisecond and decaying notes fade instead of chopping. */
struct NoiseGate
{
    double env = 0.0, gain = 1.0, envRelease = 0.0, attack = 0.0, release = 0.0;
    int holdSamples = 0, hold = 0;
    bool open = true;

    void prepare (double fs)
    {
        envRelease = 1.0 - std::exp (-1.0 / (0.012 * fs));
        attack = 1.0 - std::exp (-1.0 / (0.0006 * fs));
        release = 1.0 - std::exp (-1.0 / (0.06 * fs));
        holdSamples = (int) std::lround (0.05 * fs);
        reset();
    }

    void reset() { env = 0.0; gain = 1.0; hold = 0; open = true; }

    /** Gain for one input sample. openLevel <= 0 bypasses the gate. */
    double process (double x, double openLevel)
    {
        const double a = std::abs (x);
        env = a > env ? a : env + (a - env) * envRelease;
        if (openLevel <= 0.0 || env > openLevel)
        {
            open = true;
            hold = holdSamples;
        }
        else if (open)
        {
            if (hold > 0)
                --hold;
            else if (env < openLevel * 0.5)  // 6 dB hysteresis
                open = false;
        }
        const double target = open ? 1.0 : 0.0;
        gain += (target - gain) * (target > gain ? attack : release);
        return gain;
    }
};

/** Interpolated circular delay line. */
struct Line
{
    std::vector<float> buf;
    int write = 0;
    void init (int size) { buf.assign ((size_t) size, 0.0f); write = 0; }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); write = 0; }
    void push (float v) { buf[(size_t) write] = v; if (++write >= (int) buf.size()) write = 0; }
    float read (double delay) const
    {
        const int size = (int) buf.size();
        double pos = (double) write - delay;
        while (pos < 0.0) pos += size;
        const int i0 = (int) pos % size;
        const double frac = pos - std::floor (pos);
        const int i1 = (i0 + 1) % size;
        return (float) (buf[(size_t) i0] * (1.0 - frac) + buf[(size_t) i1] * frac);
    }
};

/** Dual-voice chorus (opposite LFO phase per side) and a darkening stereo delay. */
struct Modulation
{
    double fs = 44100.0, phase = 0.0, lp = 0.0;
    Line chorus, delay;
    juce::SmoothedValue<double> chorusMix, delayMix, delayTime;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        chorus.init ((int) std::ceil (0.06 * fs) + 4);
        delay.init ((int) std::ceil (1.4 * fs) + 4);
        chorusMix.reset (fs, 0.05);
        delayMix.reset (fs, 0.05);
        delayTime.reset (fs, 0.25);
        chorusMix.setCurrentAndTargetValue (0.0);
        delayMix.setCurrentAndTargetValue (0.0);
        delayTime.setCurrentAndTargetValue (0.38 * fs);
        phase = lp = 0.0;
    }

    void reset()
    {
        chorus.clear();
        delay.clear();
        phase = lp = 0.0;
    }

    void process (const float* in, float* left, float* right, int n, bool stereo)
    {
        const double rate = 2.0 * pi * 0.82 / fs;
        const double base = 0.0085 * fs, depth = 0.0024 * fs;
        const double lpCoef = 1.0 - std::exp (-2.0 * pi * 3200.0 / fs);
        for (int i = 0; i < n; ++i)
        {
            const float dry = in[i];
            const double cm = chorusMix.getNextValue(), dm = delayMix.getNextValue(), dt = delayTime.getNextValue();

            chorus.push (dry);
            const float wetL = chorus.read (base + depth * std::sin (phase));
            const float wetR = chorus.read (base + depth * std::sin (phase + pi));
            phase += rate;
            if (phase > 2.0 * pi) phase -= 2.0 * pi;
            const float dryGain = (float) (1.0 - 0.5 * cm), wetGain = (float) (0.5 * cm);
            float l = dry * dryGain + wetL * wetGain;
            float r = dry * dryGain + wetR * wetGain;

            const float tapL = delay.read (dt);
            const float tapR = delay.read (dt * 0.75);
            lp += lpCoef * ((double) tapL - lp);
            delay.push (dry + (float) (lp * 0.36));
            l += tapL * (float) (dm * 0.8);
            r += tapR * (float) (dm * 0.8);

            left[i] = l;
            if (stereo) right[i] = r; else left[i] = 0.5f * (l + r);
        }
    }
};
} // namespace

struct AxiomDsp::Impl
{
    double fs = 44100.0, fsOs = 176400.0;
    int maxBlock = 512;
    juce::dsp::Oversampling<float> os { 1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };

    std::vector<float> mono, left, right;
    juce::SmoothedValue<float> drive, master, room;

    NoiseGate gate;
    double gateOpenLevel = 0.0, inputGain = 1.0;

    // Oversampled section
    OnePole preHp, boostHp, boostLp;
    Biquad emph, bass, mid, treble, presence;
    std::array<OnePole, 3> stageHp, stageLp;
    double stageGain[3] { 1, 1, 1 }, bias[3] { 0, 0, 0 };
    int stages = 1;
    double powerDrive = 1.0, sag = 0.0, outGain = 1.0, sagEnv = 0.0, sagAttack = 0.0, sagRelease = 0.0;
    double boostTarget = 0.0, boostNow = 0.0, boostCoef = 0.0;

    // Tone-stack knobs glide to their targets, so turning or automating them never zips.
    const VoiceConfig* voiceConfig = &voices[1];
    std::array<double, 4> eqTarget {}, eqNow {};
    double eqCoef = 0.0;

    // Base-rate cabinet / fizz filter
    std::array<Biquad, 10> cab;
    int cabCount = 0;

    Modulation mod;
    juce::Reverb reverb;
    bool reverbActive = false;

    // Physical engine: its own oversampler (same filters, same latency) and filter states, so
    // the Standard path above is untouched and the two can run side by side while crossfading.
    juce::dsp::Oversampling<float> osPhys { 1, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    physical::PhysicalTubeStage physicalAmp;
    std::vector<float> monoPhys;
    juce::SmoothedValue<float> physInput;
    juce::SmoothedValue<double> engineMix;      // 0 = Standard, 1 = Physical
    bool stdRunning = true, physRunning = false;
    OnePole physBoostHp, physBoostLp;
    double physBoostNow = 0.0;

    AxiomParams last;
    bool haveLast = false;

    void prepare (double sampleRate, int blockSize)
    {
        fs = sampleRate;
        maxBlock = std::max (blockSize, 32);
        os.initProcessing ((size_t) maxBlock);
        fsOs = fs * 8.0;
        osPhys.initProcessing ((size_t) maxBlock);
        monoPhys.assign ((size_t) maxBlock, 0.0f);
        physInput.reset (fs, 0.04);
        engineMix.reset (fs, 0.03);
        physicalAmp.prepare (fsOs, 8);
        mono.assign ((size_t) maxBlock, 0.0f);
        left.assign ((size_t) maxBlock, 0.0f);
        right.assign ((size_t) maxBlock, 0.0f);
        drive.reset (fs, 0.04);
        master.reset (fs, 0.025);
        room.reset (fs, 0.08);
        sagAttack = 1.0 - std::exp (-1.0 / (0.004 * fsOs));
        sagRelease = 1.0 - std::exp (-1.0 / (0.12 * fsOs));
        boostCoef = 1.0 - std::exp (-1.0 / (0.02 * fsOs));
        eqCoef = 1.0 - std::exp (-(double) eqStep / (0.03 * fsOs));
        gate.prepare (fs);
        mod.prepare (fs);
        reverb.setSampleRate (fs);
        haveLast = false;
        engineMixPrimed = false;
        reset();
    }

    void reset()
    {
        os.reset();
        preHp.reset(); boostHp.reset(); boostLp.reset();
        emph.reset(); bass.reset(); mid.reset(); treble.reset(); presence.reset();
        for (auto& f : stageHp) f.reset();
        for (auto& f : stageLp) f.reset();
        for (auto& f : cab) f.reset();
        sagEnv = 0.0;
        boostNow = boostTarget;
        eqNow = eqTarget;
        gate.reset();
        mod.reset();
        reverb.reset();
        reverbActive = false;
        resetPhysical();
    }

    void resetStandard()
    {
        os.reset();
        preHp.reset(); boostHp.reset(); boostLp.reset();
        emph.reset(); bass.reset(); mid.reset(); treble.reset(); presence.reset();
        for (auto& f : stageHp) f.reset();
        for (auto& f : stageLp) f.reset();
        sagEnv = 0.0;
        boostNow = boostTarget;
        eqNow = eqTarget;
        designEq();
    }

    void resetPhysical()
    {
        osPhys.reset();
        physicalAmp.reset();
        physBoostHp.reset(); physBoostLp.reset();
        physBoostNow = boostTarget;
    }

    static constexpr int eqStep = 32;  // oversampled samples between tone-stack updates

    void designEq()
    {
        const auto& v = *voiceConfig;
        bass.set (lowShelf (fsOs, v.bassHz, 0.7, eqNow[0]));
        mid.set (peak (fsOs, v.midHz, v.midQ, eqNow[1]));
        treble.set (highShelf (fsOs, v.trebleHz, 0.7, eqNow[2]));
        presence.set (highShelf (fsOs, v.presenceHz, 0.7, eqNow[3]));
    }

    void stepEq()
    {
        bool moved = false;
        for (size_t i = 0; i < eqNow.size(); ++i)
        {
            const double diff = eqTarget[i] - eqNow[i];
            if (diff == 0.0)
                continue;
            eqNow[i] = std::abs (diff) < 0.01 ? eqTarget[i] : eqNow[i] + diff * eqCoef;
            moved = true;
        }
        if (moved)
            designEq();
    }

    void designVoice (const AxiomParams& p)
    {
        const auto& v = voices[(size_t) juce::jlimit (0, 2, p.voice)];
        voiceConfig = &v;
        preHp.setHighPass (fsOs, v.preHpHz);
        boostHp.setHighPass (fsOs, 720.0);
        boostLp.setLowPass (fsOs, 4200.0);
        physBoostHp.setHighPass (fsOs, 720.0);
        physBoostLp.setLowPass (fsOs, 4200.0);
        physicalAmp.setVoice (p.voice);
        emph.set (highShelf (fsOs, v.emphHz, 0.7, v.emphDb));
        stages = v.stages;
        for (int i = 0; i < 3; ++i)
        {
            stageGain[i] = dbToLin (v.stageGainDb[i]);
            bias[i] = v.bias[i];
            stageHp[(size_t) i].setHighPass (fsOs, v.interHpHz[i]);
            stageLp[(size_t) i].setLowPass (fsOs, v.interLpHz[i]);
        }
        designEq();
        powerDrive = v.powerDrive;
        sag = v.sag;
        outGain = dbToLin (v.outDb);

        // Cabinet: 2x12 closed-back with a V30-style presence peak, steep fizz roll-off.
        cabCount = 0;
        const auto add = [this] (const Coeffs& c) { cab[(size_t) cabCount++].set (c); };
        if (p.cabinet)
        {
            add (highPass (fs, 64.0, 0.72));
            add (peak (fs, 100.0, 0.8, 12.0));
            add (peak (fs, 420.0, 0.9, -3.0));
            add (peak (fs, 640.0, 1.2, -3.0));
            add (peak (fs, 2600.0, 1.2, 3.0));
            add (peak (fs, 4200.0, 1.6, 3.0));
            add (lowPass (fs, 6000.0, 0.541));
            add (lowPass (fs, 6000.0, 1.307));
            add (peak (fs, 7800.0, 3.0, -4.0));
        }
        else
        {
            add (highPass (fs, 50.0, 0.707));
            add (lowPass (fs, 9500.0, 0.707));
        }
    }

    void update (const AxiomParams& p)
    {
        eqTarget = { (double) p.bassDb, (double) p.midDb, (double) p.trebleDb, (double) p.presenceDb };
        if (! haveLast)
            eqNow = eqTarget;
        if (! haveLast || p.voice != last.voice || p.cabinet != last.cabinet)
            designVoice (p);
        last = p;
        haveLast = true;

        const auto& v = voices[(size_t) juce::jlimit (0, 2, p.voice)];
        const double knob = juce::jlimit (0.0, 10.0, (double) p.gain) / 10.0;
        const double driveDb = v.driveLoDb + (v.driveHiDb - v.driveLoDb) * knob;
        inputGain = dbToLin (p.inputDb);
        drive.setTargetValue ((float) (dbToLin (driveDb) * inputGain));
        physInput.setTargetValue ((float) inputGain);
        // On the Physical engine the knobs turn the circuit's own pots: Gain is the volume pot,
        // Bass/Middle/Treble the tone stack, Presence the feedback network (0 dB = noon).
        const auto rotation = [] (float db) { return juce::jlimit (0.0, 1.0, ((double) db + 12.0) / 24.0); };
        physicalAmp.setPots ({ knob, rotation (p.bassDb), rotation (p.midDb), rotation (p.trebleDb), rotation (p.presenceDb) });
        physicalAmp.setMathMode (p.tubeMath == 1 ? physical::MathMode::exact : physical::MathMode::fast);
        engineMix.setTargetValue (p.engine == 1 ? 1.0 : 0.0);
        if (! engineMixPrimed)
        {
            // A session that opens on the Physical engine starts there, without a fade.
            engineMix.setCurrentAndTargetValue (p.engine == 1 ? 1.0 : 0.0);
            physInput.setCurrentAndTargetValue ((float) inputGain);
            engineMixPrimed = true;
        }
        gateOpenLevel = p.gateDb > gateOffDb ? dbToLin (juce::jlimit (-80.0, 0.0, (double) p.gateDb)) : 0.0;
        master.setTargetValue ((float) dbToLin (p.masterDb));
        room.setTargetValue (p.room * 0.01f);
        boostTarget = juce::jlimit (0.0, 1.0, p.boost * 0.01);
        mod.chorusMix.setTargetValue (juce::jlimit (0.0, 1.0, p.chorus * 0.01));
        mod.delayMix.setTargetValue (juce::jlimit (0.0, 1.0, p.delayMix * 0.01));
        mod.delayTime.setTargetValue (juce::jlimit (40.0, 1000.0, (double) p.delayMs) * 0.001 * fs);
    }

    double ampSample (double x)
    {
        x = preHp.process (x);

        boostNow += (boostTarget - boostNow) * boostCoef;
        const double hp = boostHp.process (x);
        double u = (x + hp * 1.4) * (1.0 + 14.0 * boostNow);
        u = boostLp.process (triode (u, 0.08));
        x = x * (1.0 - 0.5 * boostNow) + u * 0.9 * boostNow;

        x = emph.process (x);
        for (int s = 0; s < stages; ++s)
        {
            x *= stageGain[s];
            x = triode (x, bias[s]);
            x = stageHp[(size_t) s].process (x);
            x = stageLp[(size_t) s].process (x);
        }

        x = bass.process (x);
        x = mid.process (x);
        x = treble.process (x);

        x = std::tanh (x * powerDrive) / powerDrive;
        const double a = std::abs (x);
        sagEnv += (a - sagEnv) * (a > sagEnv ? sagAttack : sagRelease);
        x /= 1.0 + sag * sagEnv * 2.0;
        x = presence.process (x);
        return x * outGain;
    }

    bool engineMixPrimed = false;

    /** The boost pedal for the Physical engine: the same mid-focused filter, but clean. The
        tubes behind it do the clipping, as they would with a real clean boost. */
    double physBoost (double x)
    {
        physBoostNow += (boostTarget - physBoostNow) * boostCoef;
        const double b = physBoostNow;
        const double hp = physBoostHp.process (x);
        const double mids = physBoostLp.process (x + hp * 1.4);
        return x * (1.0 - b) + mids * b * (1.0 + 4.0 * b);
    }

    /** One oversampled block through the Physical engine, in place. The tone stack and the
        presence network are part of its circuits, so nothing else happens here but the pedal. */
    void processPhysical (float* u, size_t numSamples)
    {
        for (size_t k = 0; k < numSamples; ++k)
            u[k] = (float) physBoost ((double) u[k]);
        for (size_t k = 0; k < numSamples; k += (size_t) eqStep)
            physicalAmp.process (u + k, (int) std::min ((size_t) eqStep, numSamples - k));
    }

    double cabSample (double x)
    {
        for (int i = 0; i < cabCount; ++i)
            x = cab[(size_t) i].process (x);
        return x;
    }

    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, const AxiomParams& p)
    {
        juce::ScopedNoDenormals noDenormals;
        update (p);

        for (int start = 0; start < numSamples; start += maxBlock)
        {
            const int n = std::min (maxBlock, numSamples - start);
            const float* l = inL + start;
            const float* r = inR != nullptr ? inR + start : inL + start;

            // Which engines run: the selected one, plus the other while the crossfade lasts.
            const bool wantPhysical = engineMix.getTargetValue() > 0.5;
            const bool fading = engineMix.isSmoothing();
            const bool runStd = ! wantPhysical || fading;
            const bool runPhys = wantPhysical || fading;
            if (runStd && ! stdRunning)
                resetStandard();
            if (runPhys && ! physRunning)
                resetPhysical();
            stdRunning = runStd;
            physRunning = runPhys;

            for (int i = 0; i < n; ++i)
            {
                const float src = p.source == 1 ? r[i] : (p.source == 2 ? 0.5f * (l[i] + r[i]) : l[i]);
                const double open = gate.process ((double) src * inputGain, gateOpenLevel);
                const float gated = (float) (src * open);
                mono[(size_t) i] = gated * drive.getNextValue();
                monoPhys[(size_t) i] = gated * physInput.getNextValue();
            }

            if (runStd)
            {
                float* channels[1] { mono.data() };
                juce::dsp::AudioBlock<float> block (channels, 1, (size_t) n);
                auto up = os.processSamplesUp (block);
                float* u = up.getChannelPointer (0);
                for (size_t k = 0; k < up.getNumSamples(); ++k)
                {
                    if (k % (size_t) eqStep == 0)
                        stepEq();
                    u[k] = (float) ampSample ((double) u[k]);
                }
                os.processSamplesDown (block);
            }

            if (runPhys)
            {
                float* channels[1] { monoPhys.data() };
                juce::dsp::AudioBlock<float> block (channels, 1, (size_t) n);
                auto up = osPhys.processSamplesUp (block);
                processPhysical (up.getChannelPointer (0), up.getNumSamples());
                osPhys.processSamplesDown (block);

                if (runStd)
                    for (int i = 0; i < n; ++i)
                    {
                        const auto m = (float) engineMix.getNextValue();
                        mono[(size_t) i] = mono[(size_t) i] * (1.0f - m) + monoPhys[(size_t) i] * m;
                    }
                else
                    std::copy (monoPhys.begin(), monoPhys.begin() + n, mono.begin());
            }
            if (! (runStd && runPhys))
                engineMix.skip (n);

            for (int i = 0; i < n; ++i)
                mono[(size_t) i] = softLimit ((float) cabSample ((double) mono[(size_t) i]) * master.getNextValue());

            const bool stereo = outR != nullptr;
            mod.process (mono.data(), left.data(), right.data(), n, stereo);

            const float roomNow = room.skip (n);
            if (roomNow > 0.0001f)
            {
                // Coming back from zero: start from an empty room, not a tail from long ago.
                if (! reverbActive)
                    reverb.reset();
                reverbActive = true;
                juce::Reverb::Parameters rp;
                rp.roomSize = 0.25f + roomNow * 0.65f;
                rp.damping = 0.52f;
                rp.wetLevel = roomNow * 0.28f;
                rp.dryLevel = 1.0f;
                rp.width = 1.0f;
                reverb.setParameters (rp);
                if (stereo)
                    reverb.processStereo (left.data(), right.data(), n);
                else
                    reverb.processMono (left.data(), n);
            }
            else
            {
                reverbActive = false;
            }

            // Final safety so room and delay tails can never push the output past full scale.
            for (int i = 0; i < n; ++i)
            {
                outL[start + i] = softLimit (left[(size_t) i]);
                if (stereo)
                    outR[start + i] = softLimit (right[(size_t) i]);
            }
        }
    }
};

AxiomDsp::AxiomDsp() : impl (std::make_unique<Impl>()) {}
AxiomDsp::~AxiomDsp() = default;
void AxiomDsp::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void AxiomDsp::reset() { impl->reset(); }
void AxiomDsp::setPhysicalNoise (bool on) { impl->physicalAmp.setNoise (on); impl->physicalAmp.setMainsRipple (on); }
int AxiomDsp::getLatencySamples() const { return (int) std::lround (impl->os.getLatencyInSamples()); }
void AxiomDsp::process (const float* inL, const float* inR, float* outL, float* outR, int n, const AxiomParams& p)
{
    impl->process (inL, inR, outL, outR, n, p);
}
}
