#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace axiom
{
struct PitchReading
{
    double hz = 0.0;       // 0 when no clear pitch was found
    double clarity = 0.0;  // 0..1, how periodic the signal is
};

/** Note name and tuning offset of a frequency, relative to A4 = 440 Hz. */
struct NoteReading
{
    int midi = 0;          // nearest MIDI note
    double cents = 0.0;    // -50..50 from that note
};

inline NoteReading noteFor (double hz, double a4 = 440.0)
{
    const double midi = 69.0 + 12.0 * std::log2 (hz / a4);
    const double nearest = std::round (midi);
    return { (int) nearest, (midi - nearest) * 100.0 };
}

inline const char* noteName (int midi)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midi % 12) + 12) % 12];
}

/**
    YIN pitch estimate for a monophonic guitar signal (de Cheveigné & Kawahara, 2002).
    Covers drop tunings down to about 60 Hz and the top of the neck. Runs on the message
    thread, never in the audio callback: it allocates and costs about a millisecond.
*/
/** How many input samples detectPitch wants at a sample rate: about 85 ms of audio. */
inline int pitchWindowFor (double sampleRate)
{
    return 4096 * std::max (1, (int) std::lround (sampleRate / 48000.0));
}

inline PitchReading detectPitch (const float* input, int count, double sampleRate,
                                 double minHz = 60.0, double maxHz = 1400.0)
{
    if (input == nullptr || count <= 0 || sampleRate <= 0.0)
        return {};

    // Work at about 48 kHz whatever the host rate; averaging is enough of a low-pass here.
    const int factor = std::max (1, (int) std::lround (sampleRate / 48000.0));
    const double fs = sampleRate / factor;
    std::vector<double> x ((size_t) (count / factor));
    double mean = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        double acc = 0.0;
        for (int k = 0; k < factor; ++k)
            acc += input[i * (size_t) factor + (size_t) k];
        x[i] = acc / factor;
        mean += x[i];
    }
    mean /= (double) std::max<size_t> (1, x.size());
    double energy = 0.0;
    for (auto& v : x)
    {
        v -= mean;
        energy += v * v;
    }

    const int n = (int) x.size();
    const int minTau = std::max (2, (int) std::floor (fs / maxHz));
    const int maxTau = (int) std::ceil (fs / minHz);
    const int window = std::min (2048, n - maxTau - 2);
    if (window < maxTau / 2 || std::sqrt (energy / n) < 0.001)  // too short or below about -60 dBFS
        return {};

    std::vector<double> d ((size_t) maxTau + 2, 1.0);
    double running = 0.0;
    for (int tau = 1; tau <= maxTau + 1; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double diff = x[(size_t) j] - x[(size_t) (j + tau)];
            sum += diff * diff;
        }
        running += sum;
        d[(size_t) tau] = running > 0.0 ? sum * tau / running : 1.0;
    }

    int best = -1;
    for (int tau = minTau; tau <= maxTau; ++tau)
        if (d[(size_t) tau] < 0.12)
        {
            while (tau + 1 <= maxTau && d[(size_t) tau + 1] < d[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    if (best < 0)
    {
        best = minTau;
        for (int tau = minTau; tau <= maxTau; ++tau)
            if (d[(size_t) tau] < d[(size_t) best])
                best = tau;
        if (d[(size_t) best] > 0.3)
            return {};
    }

    // Parabolic interpolation around the minimum for sub-sample accuracy.
    double tau = best;
    const double a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
    const double denom = a - 2.0 * b + c;
    if (std::abs (denom) > 1.0e-12)
        tau += std::clamp (0.5 * (a - c) / denom, -1.0, 1.0);

    return { fs / tau, std::clamp (1.0 - b, 0.0, 1.0) };
}
}
