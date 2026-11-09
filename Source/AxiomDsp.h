#pragma once
#include <juce_dsp/juce_dsp.h>
#include <memory>

namespace axiom
{
/** Everything the signal path needs, in user-facing units. */
struct AxiomParams
{
    int voice = 1;          // 0 Clean, 1 Classic, 2 Modern
    int source = 0;         // 0 Left, 1 Right, 2 Left + Right
    float inputDb = 0.0f;
    float gain = 3.5f;      // 0..10
    float bassDb = 0.0f;
    float midDb = 0.0f;
    float trebleDb = 0.0f;
    float presenceDb = 0.0f;
    float masterDb = -3.0f;
    float room = 16.0f;     // %
    float boost = 0.0f;     // % (mid-focused overdrive in front of the amp)
    float chorus = 0.0f;    // %
    float delayMix = 0.0f;  // %
    float delayMs = 380.0f;
    bool cabinet = true;
    float gateDb = -80.0f;  // noise gate threshold; at or below gateOffDb the gate is off
    int engine = 0;         // 0 Standard (waveshaper stages), 1 Physical (component-level tube model)
    int tubeMath = 0;       // Physical engine only: 0 Fast (polynomial math), 1 Exact (libm)
};

/** Gate thresholds at or below this are treated as "Off". */
constexpr float gateOffDb = -79.5f;

/**
    Guitar amp signal path: mono input -> noise gate -> boost -> oversampled tube-style preamp
    -> tone stack -> power amp with sag -> cabinet -> chorus / delay -> stereo room.

    Two amp engines share everything outside the 8x oversampled section. Standard is the
    original cascade of asymmetric saturators. Physical (PhysicalTubeStage.h) solves the
    12AX7 stages, power supply and push-pull power amp as circuits. Switching crossfades.

    The amp is mono, as a real one is: the selected input is processed once and
    fed to both outputs, so a single-input audio interface sounds centred.
    All processing is allocation-free after prepare().
*/
class AxiomDsp
{
public:
    AxiomDsp();
    ~AxiomDsp();

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    /** The Physical engine models the first stage's thermal and flicker noise and the supply's
        mains ripple; the offline checks switch both off where they would mask what they measure. */
    void setPhysicalNoise (bool on);
    int getLatencySamples() const;

    /** inR and outR may be null for a mono bus. Input and output may alias. */
    void process (const float* inL, const float* inR, float* outL, float* outR,
                  int numSamples, const AxiomParams& params);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
