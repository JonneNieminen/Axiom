#pragma once
/*
    PhysicalTubeStage: a component-level model of a guitar amplifier, solved as circuits.

        guitar -> [ PREAMP: every 12AX7 stage, the gain pot and its bright cap, the cathode
                    follower (Classic, Modern) and the passive tone stack -- ONE coupled circuit ]
               -> [ PHASE INVERTER: long-tail pair, presence/negative-feedback network, coupling
                    caps, grid leaks and both power-tube grids -- ONE coupled circuit ]
               -> [ POWER AMP: push-pull EL34/6L6 with screen-current split and a mismatched
                    pair, into an output transformer whose core saturates with hysteresis, and
                    a reactive speaker ]
        and all of it draws on a POWER SUPPLY: 50 Hz mains through a solid-state or tube
        rectifier into a reservoir capacitor, choke and RC filter nodes. Its ripple and sag
        are the B+ every stage sees.

    Method. Each circuit is a netlist (PhysicalCircuit.h): Modified Nodal Analysis, trapezoidal
    companion models for every capacitor/inductor, reduced to discrete K-method state space
        x' = A x + B u + C i,   y = D x + E u + F i,   v = G x + H u + K i,   i = f(v)
    with the tube currents i as nonlinear ports. Every sample, each coupled circuit solves
    v = p + K i(v) (8 dimensions for the Modern preamp) by Newton-Raphson with a hand-derived
    analytic Jacobian: tolerance 1e-6 V, hard cap 4 (Fast) / 6 (Exact) iterations, tangent
    predictor, backtracking. Pots are resistors in the netlists; moving one rebuilds the
    matrices on the audio thread without allocating.

    Why a coupled solve: grid current in one stage loads the plate before it, the Miller
    capacitance of each grid hangs on the previous plate, a cathode follower's grid current
    loads its driver, and the tone stack's impedance loads whatever drives it. Solving stage
    by stage drops all of that; solving the circuit keeps it.

    B+ (from the supply) is an input u of each circuit, not a matrix coefficient: the matrices
    depend only on component values and the sample period. When the supply sags or ripples,
    p = G x + H u moves and every operating point moves with it on the next sample.

    Real-time: nothing allocates, locks or logs after prepare(). The math policy and the voice
    are template parameters, chosen once per block. The three blocks (preamp, phase inverter,
    power amp) are software-pipelined: each works on the sample its predecessor finished in the
    previous iteration, so an out-of-order core runs all three chains at once (one oversampled
    sample of delay per block, ~8.5 us at 44.1 kHz x 8, not reported to the host).

    Remaining simplifications, stated plainly: the phase inverter's input is driven by the
    preamp output as an ideal source (its grid current does not load the tone stack); the two
    primary halves of the output transformer share one leakage inductance; the core uses a
    single-hysteron (Prandtl) hysteresis rather than Jiles-Atherton; the supply rectifier is
    driven by an ideal sine (no mains impedance or transformer saturation).
*/
#include "PhysicalCircuit.h"
#include <memory>

namespace axiom::physical
{
inline constexpr int numVoices = 3;
inline constexpr int numSupplyNodes = 5;   // 0 plates, 1 screens, 2 phase inverter, 3 later preamp, 4 first stage

/** Preamp circuit sizes per voice: states, nonlinear ports, triodes. Inputs are always
    (guitar volts, B+ first stage, B+ later stages); outputs (signal, the two supply currents). */
template <int Voice> struct PreampSize;
template <> struct PreampSize<0> { static constexpr int nx = 9,  nt = 2; };   // Clean: V1A -> tone stack -> volume -> V1B
template <> struct PreampSize<1> { static constexpr int nx = 9,  nt = 3; };   // Classic: V1A -> volume -> V2A -> CF -> tone stack
template <> struct PreampSize<2> { static constexpr int nx = 13, nt = 4; };   // Modern: V1A -> gain -> V1B (cold) -> V2A -> CF -> tone stack

template <int Voice>
using Preamp = CircuitSolver<PreampSize<Voice>::nx, 3, 2 * PreampSize<Voice>::nt, 3, PreampSize<Voice>::nt, 0>;

/** Long-tail pair + coupling to both power grids. Inputs: preamp signal, B+, bias supply,
    speaker feedback, grid-emission current A, B. Ports: 2 triodes, 2 power-tube grids.
    Outputs: supply current, grid A, grid B. */
using PhaseInverter = CircuitSolver<8, 6, 6, 3, 2, 2>;

/** Output transformer (less its magnetising branch, which is nonlinear and handled explicitly)
    and speaker, referred to the secondary. */
using LoadSystem = DkSystem<5, 1, 0, 2>;

/** Rotation (0..1) of every pot the circuits contain. */
struct Pots
{
    double gain = 0.35, bass = 0.5, mid = 0.5, treble = 0.5, presence = 0.5;
};

class PhysicalTubeStage
{
public:
    /** Builds every voice's circuits and settles them at their operating point (runs the supply
        and then the whole amp briefly). Allocates; not real-time. */
    void prepare (double oversampledRate, int oversamplingFactor);
    /** Returns the active voice to its settled operating point (a copy, no allocation). */
    void reset() noexcept;
    void setVoice (int voiceIndex) noexcept;
    /** Pot rotations 0..1. They glide, and the circuits are rebuilt as they move. */
    void setPots (const Pots& p) noexcept { potTarget = p; }
    void setMathMode (MathMode m) noexcept { mathMode = m; }
    MathMode getMathMode() const noexcept { return mathMode; }
    /** The first stage's thermal and flicker noise on/off (on by default). */
    void setNoise (bool on) noexcept { noiseOn = on; }
    /** Mains ripple on/off (on by default). Off, the rectifier sees DC at the same average
        level: the offline checks use it to measure aliasing without hum sidebands. */
    void setMainsRipple (bool on) noexcept { mainsRipple = on; }

    /** In place on oversampled samples: full-scale input in, roughly +-1 at full power out. */
    void process (float* io, int numSamples) noexcept;

    // Telemetry for the offline checks
    struct Telemetry
    {
        std::array<double, numSupplyNodes> supply {};
        double bias = 0.0, flux = 0.0, fluxSaturation = 0.0, temperatureA = 0.0, emissionA = 0.0;
        std::array<double, 8> preampPorts {};   // (Vgk, Vpk) per triode
        int triodes = 0;
        SolverStats preamp, phaseInverter, plates;
    };
    Telemetry telemetry() const noexcept;
    Telemetry idleTelemetry (int voiceIndex) const noexcept;
    void clearStats() noexcept;

    //-- Hooks for the offline checks (ngspice comparison): the circuits as netlists, and the
    //   preamp / phase inverter run on their own with fixed supplies.
    static Netlist preampNetlist (int voiceIndex, const Pots&);
    static Netlist phaseInverterNetlist (int voiceIndex, const Pots&);
    static std::vector<double> runPreamp (int voiceIndex, const Pots&, const std::vector<double>& volts, double fs,
                                          double bplusFirst, double bplusLater, MathMode);
    static std::vector<double> runPhaseInverter (int voiceIndex, const Pots&, const std::vector<double>& volts, double fs,
                                                 double bplus, double bias, MathMode);
    static double inputVolts (int voiceIndex);

private:
    struct PowerAmp
    {
        LoadSystem load {};
        std::array<double, 5> x {};
        // Newton predictor: last solution, dR/dVh, drive factors and their atan terms
        double vhPrev = 0.0, qPrev = 0.0, slopePrev = -1.0, kAPrev = 0.0, kBPrev = 0.0, atanA = 0.0, atanB = 0.0;
        double screenA = 0.0, screenB = 0.0, plateA = 0.0, plateB = 0.0, vPlateA = 0.0, vPlateB = 0.0;
        // Transformer core: secondary-referred flux, hysteron state, last secondary voltage
        double flux = 0.0, hysteron = 0.0, vsPrev = 0.0, magnetising = 0.0, speaker = 0.0;
        SolverStats stats {};
    };

    struct Supply
    {
        std::array<double, numSupplyNodes> v {};
        double choke = 0.0, phase = 0.0;
        // load currents accumulated over one base-rate step
        std::array<double, numSupplyNodes> load {};
        double dissipationA = 0.0, dissipationB = 0.0;
        int count = 0;
    };

    struct Noise
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ull;
        std::array<double, 3> pink {};
        double white() noexcept   // ~N(0,1) from four uniforms (Irwin-Hall), cheap and branch-free
        {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
                acc += (double) ((state * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0) - 0.5;
            }
            return acc * 1.7320508075688772;
        }
    };

    template <class Pre>
    struct VoiceState
    {
        Pre pre {};
        PhaseInverter pi {};
        PowerAmp power {};
        Supply supply {};
        Noise noise {};
        double bias = 0.0, prePipe = 0.0, feedback = 0.0, temperatureA = 0.0, temperatureB = 0.0;
        std::array<double, 2> gridPipe {};
        std::array<double, 2> emission {};
    };

    struct VoiceNets
    {
        Netlist pre, pi;
        int gainTop = -1, gainBottom = -1, trebleTop = -1, trebleBottom = -1, bass = -1, mid = -1, presence = -1;
    };

    /** Per-voice constants derived in prepare(). */
    struct Derived
    {
        double turns = 1.0, fullScaleVolts = 28.0, outputGain = 1.0, inputVolts = 2.0;
        double dt = 0.0, dtBase = 0.0, feedbackCoef = 0.0;
        double fluxSaturation = 0.1, hysteronWidth = 0.002, coerciveCurrent = 0.02, lMagnetising = 0.06;
        double thermalCoef = 0.0, emissionPerWatt = 0.0, emissionThreshold = 0.0;
        double noiseSigma = 0.0, pinkScale = 0.0;
        std::array<double, 3> pinkPole {}, pinkGain {};
        std::array<double, numSupplyNodes> dtOverC {};
        double gSource = 0.0, dtOverL12 = 0.0, g12 = 0.0, g23 = 0.0, g34 = 0.0, g45 = 0.0, mainsStep = 0.0;
        double vPeak = 0.0, perveance = 0.0, r12 = 0.0, rscreen = 0.0;
        bool tubeRectifier = false, choke = false;
        PentodeModel tubeA {}, tubeB {};
    };

    template <int V> auto& live() noexcept;
    template <int V> auto& settled() noexcept;
    template <int V> const auto& live() const noexcept;
    template <int V> const auto& settled() const noexcept;

    template <TubeMath M, int V> void run (float* io, int numSamples) noexcept;
    template <TubeMath M, class S> AXIOM_HOT double powerStage (S& s, const Derived& d) noexcept;
    template <class S> void stepSupply (S& s, const Derived& d) noexcept;
    void glidePots (int numSamples) noexcept;
    void applyPots (int voiceIndex, bool rebase) noexcept;
    template <int V> void initialiseVoice();
    template <int V> Telemetry telemetryOf (const VoiceState<Preamp<V>>&) const noexcept;

    VoiceState<Preamp<0>> clean {}, cleanSettled {};
    VoiceState<Preamp<1>> classic {}, classicSettled {};
    VoiceState<Preamp<2>> modern {}, modernSettled {};
    std::array<VoiceNets, numVoices> nets {};
    std::array<Derived, numVoices> derived {};
    Workspace workspace;
    Pots potNow {}, potTarget {}, potBuilt {};
    int voice = 1, osFactor = 8, samplesSinceRebuild = 0;
    MathMode mathMode = MathMode::fast;
    bool noiseOn = true, mainsRipple = true;
    double fs = 352800.0, potCoef = 0.0;
};
} // namespace axiom::physical
