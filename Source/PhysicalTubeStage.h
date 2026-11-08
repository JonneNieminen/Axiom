#pragma once
/*
    PhysicalTubeStage: a component-level model of a guitar amplifier's tube circuitry.

    Signal path (all at the 8x oversampled rate):

        guitar volts -> [12AX7 stage 1] -> gain pot -> [12AX7 stage 2] -> ([12AX7 stage 3])
                     -> coupling cap -> tone stack (supplied by the caller)
                     -> phase inverter (+ negative feedback from the speaker)
                     -> 2x [coupling cap + grid-leak + grid-stopper + power-tube grid]
                     -> push-pull pentodes -> output transformer -> reactive speaker load

        ...and every tube draws its current from a reactive power supply (rectifier, reservoir,
        choke and RC filter nodes), whose sagging voltages feed straight back into the stages.

    Method
    ------
    Each linear sub-circuit (resistors, capacitors, inductors, sources) is written down as a
    netlist and stamped into a Modified Nodal Analysis (MNA) matrix once, in prepare(). Every
    reactive element is discretised with the trapezoidal rule, which turns it into a conductance
    in parallel with a "history" current source; those history sources are the state variables.
    Eliminating the linear node voltages leaves the classic discrete K-method (DK) state-space
    form (Yeh, Abel & Smith 2010; Holters & Zoelzer 2011):

        x[n+1] = A x[n] + B u[n] + C i[n]          state update (capacitor / inductor memory)
        y[n]   = D x[n] + E u[n] + F i[n]          outputs (plate voltage, speaker voltage)
        v[n]   = G x[n] + H u[n] + K i[n]          voltages that control the nonlinearities
        i[n]   = f(v[n])                           tube equations (Koren plate, Child-law grid)

    u holds the sources: the signal arriving at a stage and that stage's B+ supply voltage.
    The only implicit part is v = p + K f(v) with p = G x + H u, which for a triode is a 2x2
    system in (Vgk, Vpk). It is solved every sample by Newton-Raphson with a hand-derived
    analytic Jacobian, a residual threshold of 1e-6 V and a hard iteration cap (4 in Fast
    mode, 6 in Exact mode; 6 for the cheap 1-D power-tube solves), so the worst-case cost per
    sample is fixed. A tangent predictor (last sample's Jacobian applied to the change in p)
    makes almost every solve finish in one pass; backtracking (2-D) and bracketing (1-D)
    keep the rare hard transient -- a stage thrown across cutoff or into grid conduction in
    one sample -- from oscillating.

    Why B+ is an input rather than a matrix coefficient: the matrices only depend on component
    values and the sample period. The supply voltage is a source, so it enters through u. When
    the supply sags, p = G x + H u moves, the Newton solve lands on a different (Vgk, Vpk)
    operating point, and the stage's bias, gain and clipping symmetry all shift with it -- which
    is exactly what the real circuit does. Grid current charging the coupling capacitors moves
    the bias the same way (blocking distortion), and a cold-biased stage clips asymmetrically.

    Real-time rules: nothing here allocates, locks or logs after prepare(). The math policy
    (Fast or Exact) and the stage count are template parameters, so those choices are made
    once per block, not per sample; inside the loop, data-dependent decisions are selects,
    apart from the Newton exits. The per-sample loop is software-pipelined across the stages
    (see run()) so an out-of-order core works on all of them at once, the state updates use
    column-major matrices that vectorise, and the hot path is force-inlined.

    Simplifications, stated plainly: each stage is solved on its own, driven by the previous
    plate as an ideal source (the next stage's input capacitance is lumped onto the plate);
    the rectifier is a DC source with a resistive or Child-law drop (sag dynamics, no mains
    ripple); the phase inverter is linear; the output transformer core does not saturate.
*/
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <numbers>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
 #define AXIOM_HOT [[gnu::always_inline]] inline
#else
 #define AXIOM_HOT inline
#endif

namespace axiom::physical
{
enum class MathMode { fast = 0, exact = 1 };

//==============================================================================
// Math policies
//==============================================================================

/** Table-driven exp and log and an Estrin-scheme atan. In the solver these sit in a serial
    dependency chain (the Koren equation is exp -> log -> log -> exp), so what matters is
    latency, not throughput: a 64-entry table shrinks the reduced argument to |r| < 0.008, which
    leaves a degree-4/5 polynomial evaluated in two or three FMA levels. They contain no library
    calls, inline completely and have no data-dependent branches (selects only). Accuracy:
    ~1e-13 relative for exp and log, 2e-8 absolute for atan -- the Newton tolerance is 1e-6 V. */
namespace fast
{
inline constexpr double shifter = 6755399441055744.0;   // 1.5 * 2^52: x + shifter rounds x to an integer in the low bits

/** 2^(j/64) and, for the log, 1/c_j and ln(c_j) at the centre c_j of each mantissa interval. */
struct Tables
{
    std::array<double, 64> exp2 {}, invC {}, logC {};
    Tables()
    {
        for (int j = 0; j < 64; ++j)
        {
            exp2[(size_t) j] = std::exp2 (j / 64.0);
            const double c = 1.0 + (j + 0.5) / 64.0;
            invC[(size_t) j] = 1.0 / c;
            logC[(size_t) j] = std::log (c);
        }
    }
};
inline const Tables tables;

AXIOM_HOT double exp (double x) noexcept
{
    // e^x = 2^(n/64) e^r,  n = round(64 x / ln2),  |r| <= ln2/128
    x = std::clamp (x, -708.0, 708.0);
    constexpr double toN = 64.0 / std::numbers::ln2, fromN = std::numbers::ln2 / 64.0;
    const double t = x * toN + shifter;
    const auto n = std::bit_cast<std::int64_t> (t) - std::bit_cast<std::int64_t> (shifter);
    const double r = x - (t - shifter) * fromN;
    const double r2 = r * r;
    // e^r ~ 1 + r + r^2/2 + r^3/6 + r^4/24 (error < r^5/120 ~ 4e-14), Estrin order
    const double p = (1.0 + r) + r2 * ((0.5 + r * (1.0 / 6.0)) + r2 * (1.0 / 24.0));
    const auto scaleBits = std::bit_cast<std::uint64_t> (tables.exp2[(size_t) (n & 63)]) + ((std::uint64_t) (n >> 6) << 52);
    return p * std::bit_cast<double> (scaleBits);
}

AXIOM_HOT double log (double x) noexcept
{
    // x = 2^e m, m in [1, 2); m = c_j (1 + r) with c_j the centre of m's 1/64 interval, |r| < 1/128
    x = std::max (x, 1.0e-300);
    const auto bits = std::bit_cast<std::uint64_t> (x);
    const auto e = (double) ((std::int64_t) (bits >> 52) - 1023);
    const auto j = (size_t) ((bits >> 46) & 63);
    const double m = std::bit_cast<double> ((bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull);
    const double r = m * tables.invC[j] - 1.0;
    const double r2 = r * r;
    // ln(1+r) ~ r - r^2/2 + r^3/3 - r^4/4 + r^5/5 (error < r^6/6 ~ 4e-14)
    const double p = (r - 0.5 * r2) + r2 * r * ((1.0 / 3.0 - 0.25 * r) + 0.2 * r2);
    return e * std::numbers::ln2 + tables.logC[j] + p;
}

AXIOM_HOT double atan (double x) noexcept
{
    // Abramowitz & Stegun 4.4.49 on [0, 1], |error| <= 2e-8, and atan(a) = pi/2 - atan(1/a) above.
    const double a = std::abs (x);
    const bool invert = a > 1.0;
    const double z = invert ? 1.0 / a : a;
    const double w = z * z, w2 = w * w, w4 = w2 * w2;
    const double q0 = (1.0 - 0.3333314528 * w) + w2 * (0.1999355085 - 0.1420889944 * w);
    const double q1 = (0.1065626393 - 0.0752896400 * w) + w2 * (0.0429096138 - 0.0161657367 * w);
    double p = z * (q0 + w4 * (q1 + w4 * 0.0028662257));
    p = invert ? std::numbers::pi * 0.5 - p : p;
    return std::copysign (p, x);
}
} // namespace fast

/** Table/polynomial math, capped at 4 Newton iterations. The default. */
struct FastMath
{
    static constexpr int maxIterations = 4;
    AXIOM_HOT static double exp (double x) noexcept { return fast::exp (x); }
    AXIOM_HOT static double log (double x) noexcept { return fast::log (x); }
    AXIOM_HOT static double atan (double x) noexcept { return fast::atan (x); }
};

/** The C library's exp, log and atan, capped at 6 Newton iterations. */
struct ExactMath
{
    static constexpr int maxIterations = 6;
    static double exp (double x) noexcept { return std::exp (x); }
    static double log (double x) noexcept { return std::log (std::max (x, 1.0e-300)); }
    static double atan (double x) noexcept { return std::atan (x); }
};

template <class M>
concept TubeMath = requires (double x)
{
    { M::exp (x) } -> std::same_as<double>;
    { M::log (x) } -> std::same_as<double>;
    { M::atan (x) } -> std::same_as<double>;
    { M::maxIterations } -> std::convertible_to<int>;
};

/** Newton-Raphson stops when every residual is below this many volts (spec: < 1e-6). */
inline constexpr double newtonTolerance = 1.0e-6;

/** Iteration cap for the one-dimensional solves (power-tube grids and plates) in both modes.
    They cost a few square roots and two atans per pass, so the extra headroom is cheap; the
    2-D triode solve, the expensive one, is capped by the math policy (4 Fast, 6 Exact). */
inline constexpr int maxIterations1D = 6;

/** A Newton step smaller than this is applied and the solve stops without evaluating the
    tubes again. Newton's error is quadratic: after a step d the residual is about
    |K| |d2I/dv2| d^2 / 2. With |K| <= 1e5 ohm and |d2I/dv2| <= 1e-3 A/V^2 for a 12AX7 (both
    worst cases, at the onset of grid conduction) that is <= 5e-8 V at d = 3e-5 V, well inside
    the 1e-6 V residual tolerance. The currents are moved along the same tangent, so the
    accepted point and the state update stay consistent to second order. */
inline constexpr double newtonAcceptStep = 3.0e-5;

/** ln(1 + e^t) and its derivative, the logistic sigma(t), computed without overflow:
    softplus(t) = max(t, 0) + ln(1 + e^-|t|). One exp and one log for both. */
struct Softplus { double value, slope; };

template <TubeMath M>
AXIOM_HOT Softplus softplus (double t) noexcept
{
    const double e = M::exp (-std::abs (t));
    const double onePlus = 1.0 + e;
    return { std::max (t, 0.0) + M::log (onePlus), (t >= 0.0 ? 1.0 : e) / onePlus };
}

/** Child-Langmuir grid conduction, I = G s^1.5, with a smooth algebraic knee
    s = (v + sqrt(v^2 + w^2)) / 2. Above the knee s -> v (the 3/2-power law of a vacuum diode);
    below it the current falls off as (w^2 / 4|v|)^1.5, i.e. nanoamps a few volts negative.
    Square roots only: grid current is evaluated on every Newton step of every stage, and an
    exp/log here would double the cost of the triode for no audible difference. */
struct GridDiode
{
    double g, knee;
    struct Result { double i, di; };
    AXIOM_HOT Result operator() (double v) const noexcept
    {
        const double root = std::sqrt (v * v + knee * knee);
        const double s = 0.5 * (v + root);
        const double rs = std::sqrt (s);
        return { g * s * rs, 1.5 * g * rs * 0.5 * (1.0 + v / root) };
    }
};

//==============================================================================
// Tube equations
//==============================================================================

/** Norman Koren's triode model (1996) for the plate current, and the Child-Langmuir grid
    diode above for grid conduction.

        E1 = (Vpk / kp) ln(1 + exp(kp (1/mu + Vgk / sqrt(kvb + Vpk^2))))
        Ip = 2 E1^ex / kg1                                   (E1 > 0, else 0)
        Ig = Gg s^1.5

    The sqrt(kvb + Vpk^2) term is what gives the triode its plate-resistance modulation: the
    amplification factor and rp both change with the operating point, so gain and asymmetry
    move as the signal (and the supply) swings the plate. Grid current is what clamps the
    positive grid swing and charges the coupling capacitor. */
struct TriodeModel
{
    double mu = 100.0, ex = 1.4, kg1 = 1060.0, kp = 600.0, kvb = 300.0;   // 12AX7 (Koren)
    GridDiode grid { 6.2e-4, 0.3 };                                       // A/V^1.5, onset width in volts

    struct Result
    {
        double ig, ip;        // grid current (g -> k) and plate current (p -> k), amps
        double dIgdVgk;       // analytic Jacobian entries; dIg/dVpk is identically zero
        double dIpdVgk, dIpdVpk;
    };

    template <TubeMath M>
    AXIOM_HOT Result evaluate (double vgk, double vpk) const noexcept
    {
        // Plate current. Hand-derived partial derivatives (no numerical differentiation):
        //   dE1/dVgk = Vpk sigma / S,   dE1/dVpk = L / kp - Vpk^2 Vgk sigma / S^3,   S = sqrt(kvb + Vpk^2)
        //   dIp/dE1  = 2 ex E1^(ex-1) / kg1
        const double invS = 1.0 / std::sqrt (kvb + vpk * vpk);
        const auto sp = softplus<M> (kp * (1.0 / mu + vgk * invS));
        const double e1Raw = vpk / kp * sp.value;
        const double on = e1Raw > 0.0 ? 1.0 : 0.0;          // cut off for E1 <= 0 (a select)
        const double e1 = std::max (e1Raw, 1.0e-12);
        const double e1Pow = M::exp (ex * M::log (e1));      // E1^ex
        const double ip = on * (2.0 / kg1) * e1Pow;
        const double dIpdE1 = ip * ex / e1;
        const double dE1dVgk = vpk * sp.slope * invS;
        const double dE1dVpk = sp.value / kp - vpk * vpk * vgk * sp.slope * invS * invS * invS;

        const auto g = grid (vgk);   // independent of the plate chain: runs in parallel on an OoO core
        return { g.i, ip, g.di, dIpdE1 * dE1dVgk, dIpdE1 * dE1dVpk };
    }
};

/** Koren's beam-tetrode/pentode model for the power tubes. The plate current is set by the
    control grid and screen voltages; the plate voltage only enters through the atan "knee",
    which is what makes a pentode behave like a current source driving the transformer until
    the plate swings down into the knee and the tube saturates.

        E1  = (Vg2 / kp) ln(1 + exp(kp (1/mu + Vg1 / Vg2)))
        Ip  = (2 E1^ex / kg1) atan(Vp / kvb)
        Ig2 = E1^ex / kg2       (Koren's screen law on the same softplus-smoothed E1: identical
                                 in conduction, and smooth instead of kinked through cutoff) */
struct PentodeModel
{
    double mu, ex, kg1, kg2, kp, kvb;
    GridDiode grid;

    struct Drive { double plateScale, screen; };   // plate current = plateScale * atan(Vp / kvb)

    template <TubeMath M>
    AXIOM_HOT Drive drive (double vg1, double vg2) const noexcept
    {
        const auto sp = softplus<M> (kp * (1.0 / mu + vg1 / vg2));
        const double e1Pow = M::exp (ex * M::log (std::max (vg2 / kp * sp.value, 1.0e-12)));
        return { (2.0 / kg1) * e1Pow, e1Pow / kg2 };
    }
};

//==============================================================================
// Discrete state-space (DK) systems
//==============================================================================

/** Matrices produced by Netlist::discretise(), with runtime sizes (prepare-time only). */
struct DkMatrices
{
    int nx = 0, nu = 0, nn = 0, ny = 0;
    std::vector<double> A, B, C, D, E, F, G, H, K;
    // DC operating point: x* = Xu u + Xi i,   v* = Hdc u + Kdc i  (fixed point of the update)
    std::vector<double> Xu, Xi, Hdc, Kdc;
    bool hasDc = false;
};

/** Builds MNA matrices from components and reduces them to DK form. Nodes are 0..n-1; the
    ground node is `gnd`. Reactive elements become states in the order they are added. */
class Netlist
{
public:
    static constexpr int gnd = -1;
    explicit Netlist (int numNodes) : nodes (numNodes) {}

    void resistor (int a, int b, double ohms)      { resistors.push_back ({ a, b, 1.0 / ohms }); }
    void capacitor (int a, int b, double farads)   { reactives.push_back ({ a, b, farads, true }); }
    void inductor (int a, int b, double henries)   { reactives.push_back ({ a, b, henries, false }); }
    /** u[k] = V(a) - V(b). Returns k. */
    int voltageInput (int a, int b)                { inputs.push_back ({ a, b, true }); return (int) inputs.size() - 1; }
    /** u[k] amps flow into node a from outside and leave from node b. Returns k. */
    int currentInput (int a, int b)                { inputs.push_back ({ a, b, false }); return (int) inputs.size() - 1; }
    /** Nonlinear device current i[k] flowing a -> b, controlled by V(a) - V(b). Returns k. */
    int nonlinearPort (int a, int b)               { ports.push_back ({ a, b }); return (int) ports.size() - 1; }
    /** y[k] = V(a) - V(b). Returns k. */
    int output (int a, int b)                      { outputs.push_back ({ a, b }); return (int) outputs.size() - 1; }

    DkMatrices discretise (double sampleRate, bool withDcPoint) const;

private:
    struct Resistor { int a, b; double g; };
    struct Reactive { int a, b; double value; bool capacitor; };
    struct Input { int a, b; bool voltage; };
    struct Pair { int a, b; };
    int nodes;
    std::vector<Resistor> resistors;
    std::vector<Reactive> reactives;
    std::vector<Input> inputs;
    std::vector<Pair> ports, outputs;
};

/** Fixed-size DK system. Compile-time dimensions let the compiler fully unroll and vectorise
    the small matrix-vector products in the per-sample update. Row-major storage. */
template <int NX, int NU, int NN, int NY>
struct DkSystem
{
    std::array<double, NX * NX> A {};   // column-major
    std::array<double, NX * NU> B {};   // column-major
    std::array<double, NX * NN> C {};   // column-major
    std::array<double, NY * NX> D {};
    std::array<double, NY * NU> E {};
    std::array<double, NY * NN> F {};
    std::array<double, NN * NX> G {};
    std::array<double, NN * NU> H {};
    std::array<double, NN * NN> K {};
    std::array<double, NX * NU> Xu {};
    std::array<double, NX * NN> Xi {};
    std::array<double, NN * NU> Hdc {};
    std::array<double, NN * NN> Kdc {};

    bool assign (const DkMatrices& m)
    {
        if (m.nx != NX || m.nu != NU || m.nn != NN || m.ny != NY)
            return false;
        const auto copy = [] (auto& dst, const std::vector<double>& src)
        {
            for (size_t i = 0; i < dst.size() && i < src.size(); ++i) dst[i] = src[i];
        };
        // A, B and C are stored column-major: the state update then runs as a sum of scaled
        // columns, which vectorises across the rows (2 x double NEON / SSE2 lanes).
        const auto transpose = [] (auto& dst, const std::vector<double>& src, int rows, int cols)
        {
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    dst[(size_t) (c * rows + r)] = src[(size_t) (r * cols + c)];
        };
        transpose (A, m.A, NX, NX); transpose (B, m.B, NX, NU); transpose (C, m.C, NX, NN);
        copy (D, m.D); copy (E, m.E); copy (F, m.F);
        copy (G, m.G); copy (H, m.H); copy (K, m.K);
        copy (Xu, m.Xu); copy (Xi, m.Xi); copy (Hdc, m.Hdc); copy (Kdc, m.Kdc);
        return true;
    }

    /** p = G x + H u: the part of the controlling voltages that is already known. */
    AXIOM_HOT std::array<double, NN> linearPart (const std::array<double, NX>& x, const std::array<double, NU>& u) const noexcept
    {
        std::array<double, NN> p {};
        for (int r = 0; r < NN; ++r)
        {
            double acc = 0.0;
            for (int c = 0; c < NU; ++c) acc += H[(size_t) (r * NU + c)] * u[(size_t) c];
            for (int c = 0; c < NX; ++c) acc += G[(size_t) (r * NX + c)] * x[(size_t) c];
            p[(size_t) r] = acc;
        }
        return p;
    }

    /** y = D x + E u + F i (call before advance()). */
    AXIOM_HOT double output (int row, const std::array<double, NX>& x, const std::array<double, NU>& u, const std::array<double, NN>& i) const noexcept
    {
        double acc = 0.0;
        for (int c = 0; c < NX; ++c) acc += D[(size_t) (row * NX + c)] * x[(size_t) c];
        for (int c = 0; c < NU; ++c) acc += E[(size_t) (row * NU + c)] * u[(size_t) c];
        for (int c = 0; c < NN; ++c) acc += F[(size_t) (row * NN + c)] * i[(size_t) c];
        return acc;
    }

    /** x <- A x + B u + C i. */
    AXIOM_HOT void advance (std::array<double, NX>& x, const std::array<double, NU>& u, const std::array<double, NN>& i) const noexcept
    {
        std::array<double, NX> next {};
        for (int c = 0; c < NX; ++c)
            for (int r = 0; r < NX; ++r) next[(size_t) r] += A[(size_t) (c * NX + r)] * x[(size_t) c];
        for (int c = 0; c < NU; ++c)
            for (int r = 0; r < NX; ++r) next[(size_t) r] += B[(size_t) (c * NX + r)] * u[(size_t) c];
        for (int c = 0; c < NN; ++c)
            for (int r = 0; r < NX; ++r) next[(size_t) r] += C[(size_t) (c * NX + r)] * i[(size_t) c];
        x = next;
    }

    /** Steady state for constant inputs u and nonlinear currents i. */
    std::array<double, NX> dcState (const std::array<double, NU>& u, const std::array<double, NN>& i) const noexcept
    {
        std::array<double, NX> x {};
        for (int r = 0; r < NX; ++r)
        {
            double acc = 0.0;
            for (int c = 0; c < NU; ++c) acc += Xu[(size_t) (r * NU + c)] * u[(size_t) c];
            for (int c = 0; c < NN; ++c) acc += Xi[(size_t) (r * NN + c)] * i[(size_t) c];
            x[(size_t) r] = acc;
        }
        return x;
    }
};

/** Newton-Raphson bookkeeping, read by the offline checks. */
struct SolverStats
{
    std::uint64_t solves = 0, iterations = 0, capHits = 0;
    void add (int iters, bool capped) noexcept
    {
        ++solves;
        iterations += (std::uint64_t) iters;
        capHits += capped ? 1u : 0u;
    }
};

//==============================================================================
// Circuit blocks
//==============================================================================

/** One common-cathode 12AX7 gain stage:

        Vin --Cin--+--Rs--+ g          B+ (from the supply node)
                   |      |                |
                 Rleak    +--Cgp-------+   Rp
                   |      |            |   |
                  gnd     +--Cgk--+    +---+ p ---> output (plate voltage)
                                  |        |
                                  k        Cp (stray / next-stage Miller) and Rload
                                  |
                              Rk || Ck
                                  |
                                 gnd

    States (5): Cin, Cgk, Cgp (Miller), Ck (cathode bypass), Cp.  Inputs (2): Vin, B+.
    Nonlinear ports (2): grid current g->k, plate current p->k.  Output (1): Vp.
    Cgk / Cgp are the inter-electrode capacitances: Cgp times the stage gain is the Miller
    capacitance that, with the grid stopper, sets the stage's natural treble roll-off. */
struct TriodeStage
{
    using System = DkSystem<5, 2, 2, 1>;
    System sys;
    TriodeModel tube;
    double rp = 100.0e3;

    // Dynamic state
    std::array<double, 5> x {};
    std::array<double, 2> vPrev {}, pPrev {};
    std::array<double, 4> tangent { 1.0, 0.0, 0.0, 1.0 };   // dv/dp = (I - K dI/dv)^-1 at the last solution
    double supplyCurrent = 0.0;

    template <TubeMath M>
    AXIOM_HOT double process (double vin, double vb, SolverStats& stats) noexcept
    {
        const std::array<double, 2> u { vin, vb };
        const auto p = sys.linearPart (x, u);
        const auto& K = sys.K;

        // Tangent predictor: v = p + K i(v) implies dv = (I - K dI/dv)^-1 dp, so last sample's
        // Jacobian maps the change in the linear part onto the solution. This follows a stage
        // into cutoff or grid conduction far better than v + dp, and costs no tube evaluation.
        const double dp0 = p[0] - pPrev[0], dp1 = p[1] - pPrev[1];
        double vgk = vPrev[0] + tangent[0] * dp0 + tangent[1] * dp1;
        double vpk = vPrev[1] + tangent[2] * dp0 + tangent[3] * dp1;

        // Newton-Raphson with analytic Jacobian. Each pass evaluates the tubes once and forms
        // the residual F = p + K i(v) - v and J = K dI/dv - I. It stops when
        //   - |F| < 1e-6 V at the evaluated point (the specification's threshold), or
        //   - the Newton step is below newtonAcceptStep: the step is taken and the currents are
        //     moved along the same tangent (residual afterwards ~1e-10 V, see above), or
        //   - the hard iteration cap is reached: then the last *evaluated* point is kept, so
        //     the voltages and currents handed to the state update always belong together.
        // With the tangent predictor almost every sample ends in the first pass. If a step
        // makes the residual worse (a stage thrown across the grid-conduction or cutoff knee
        // in one sample), it is halved and retried (backtracking line search).
        TriodeModel::Result t {};
        double j00 = -1.0, j01 = 0.0, j10 = 0.0, j11 = -1.0;
        double lastNorm = 1.0e300, lastD0 = 0.0, lastD1 = 0.0;
        int iter = 0;
        bool converged = false;
        for (;;)
        {
            t = tube.evaluate<M> (vgk, vpk);
            ++iter;
            const double f0 = p[0] + K[0] * t.ig + K[1] * t.ip - vgk;
            const double f1 = p[1] + K[2] * t.ig + K[3] * t.ip - vpk;
            const double norm = std::max (std::abs (f0), std::abs (f1));
            if (norm > lastNorm && iter < M::maxIterations)
            {
                lastD0 *= 0.5;
                lastD1 *= 0.5;
                vgk -= lastD0;
                vpk -= lastD1;
                continue;
            }
            lastNorm = norm;
            // dI/dv = [[dIg/dVgk, 0], [dIp/dVgk, dIp/dVpk]]
            j00 = K[0] * t.dIgdVgk + K[1] * t.dIpdVgk - 1.0;
            j01 = K[1] * t.dIpdVpk;
            j10 = K[2] * t.dIgdVgk + K[3] * t.dIpdVgk;
            j11 = K[3] * t.dIpdVpk - 1.0;
            converged = norm < newtonTolerance;
            if (converged || iter >= M::maxIterations)
                break;
            // dv = -J^-1 F, the 2x2 inverse written out. The limits only catch a pathological step.
            const double invDet = 1.0 / (j00 * j11 - j01 * j10);
            const double d0 = std::clamp ((j01 * f1 - j11 * f0) * invDet, -100.0, 100.0);
            const double d1 = std::clamp ((j10 * f0 - j00 * f1) * invDet, -500.0, 500.0);
            vgk += d0;
            vpk += d1;
            lastD0 = d0;
            lastD1 = d1;
            if (std::max (std::abs (d0), std::abs (d1)) < newtonAcceptStep)
            {
                t.ig += t.dIgdVgk * d0;
                t.ip += t.dIpdVgk * d0 + t.dIpdVpk * d1;
                converged = true;
                break;
            }
        }
        stats.add (iter, ! converged);

        {
            // (I - K dI/dv)^-1 = -J^-1, for the next sample's predictor.
            const double invDet = -1.0 / (j00 * j11 - j01 * j10);
            tangent = { j11 * invDet, -j01 * invDet, -j10 * invDet, j00 * invDet };
        }
        vPrev = { vgk, vpk };
        pPrev = p;
        const std::array<double, 2> i { t.ig, t.ip };
        const double vp = sys.output (0, x, u, i);
        sys.advance (x, u, i);
        supplyCurrent = (vb - vp) / rp;
        return vp;
    }
};

/** A power tube's input: phase-inverter output -> Rpi -> Cc -> grid leak (to the negative
    bias supply) -> grid stopper -> grid (with input capacitance). When the grid is driven
    positive it conducts, charges Cc and pushes the bias colder after the transient: the
    "blocking" that makes a cranked amp's decay bloom and fart. 1 nonlinear port. */
struct GridStage
{
    using System = DkSystem<2, 2, 1, 0>;
    System sys;
    std::array<double, 2> x {};
    double vPrev = 0.0, pPrev = 0.0, tangent = 1.0;

    template <TubeMath M>
    AXIOM_HOT double process (double vdrive, double vbias, const PentodeModel& tube, SolverStats& stats) noexcept
    {
        const std::array<double, 2> u { vdrive, vbias };
        const double p = sys.linearPart (x, u)[0];
        const double k = sys.K[0];
        double v = vPrev + tangent * (p - pPrev);   // dv/dp = 1 / (1 - k dIg/dv)
        GridDiode::Result g {};
        // Bracket from the physics: grid current only ever pulls the grid down (k < 0, Ig >= 0),
        // so the root is at or below p; and a volt below both p and 0 V the diode carries
        // microamps at most, so the residual there is still positive.
        double lo = std::min (p, 0.0) - 1.0, hi = p;
        // Driven hard positive, the grid clamps where |k| G v^1.5 ~ p, i.e. near
        // (p / |k| G)^(2/3): start no higher than that (plus the knee), or Newton walks down
        // the steep 3/2-power curve a third of the way per step.
        const double clampEstimate = std::cbrt (std::max (p, 0.0) / (-k * tube.grid.g));
        hi = std::min (hi, clampEstimate * clampEstimate + tube.grid.knee);
        v = std::clamp (v, lo, hi);
        int iter = 0;
        bool converged = false;
        for (;;)   // same scheme as the triode, safeguarded by bracketing instead of backtracking
        {
            g = tube.grid (v);
            ++iter;
            const double f = p + k * g.i - v;
            converged = std::abs (f) < newtonTolerance;
            if (converged || iter >= maxIterations1D)
                break;
            (f > 0.0 ? lo : hi) = v;                         // f is strictly decreasing (k < 0): bracket the root
            const double newton = v - f / (k * g.di - 1.0);
            // Inclusive test: a point exactly on the root collapses the bracket onto itself.
            const double next = (newton >= lo && newton <= hi) ? newton : 0.5 * (lo + hi);
            const double step = next - v;
            v = next;
            if (std::abs (step) < newtonAcceptStep)
            {
                g.i += g.di * step;
                converged = true;
                break;
            }
        }
        stats.add (iter, ! converged);
        tangent = 1.0 / (1.0 - k * g.di);
        vPrev = v;
        pPrev = p;
        sys.advance (x, u, { g.i });
        return v;
    }
};

/** Output transformer and speaker, referred to the secondary. Driven by the current the
    push-pull pair forces through the primary.

        Is -> s --+-- Lm (magnetising) -- Rcore (core loss) -- Cw (winding capacitance), each to gnd
                  |
                  +-- Rw -- Lleak -- n2 (speaker terminals) -- Re -- (Le || R2) -- (Res || Les || Ces) -- gnd

    The speaker's motional resonance (Res/Les/Ces) and voice-coil inductance (Le, with eddy
    losses R2) make the load reactive: impedance peaks at the cone resonance and rises in the
    treble. A pentode is close to a current source, so that impedance curve shapes the power
    amp's voltage response -- the low-end thump and the top-end bite of a real amp. */
struct LoadNetwork
{
    using System = DkSystem<6, 1, 0, 2>;
    System sys;
    std::array<double, 6> x {};
};

//==============================================================================
// Circuit descriptions
//==============================================================================

struct StageSpec
{
    double rk = 1.5e3, ck = 22.0e-6;       // cathode resistor and bypass capacitor (0 = unbypassed)
    double cin = 22.0e-9;                  // input coupling capacitor
    double rleak = 1.0e6;                  // grid leak / volume pot
    double rs = 47.0e3;                    // grid stopper plus the previous plate's source impedance
    double rp = 100.0e3, rload = 1.0e6;
    double cp = 125.0e-12;                 // plate load: wiring plus the next stage's Miller capacitance
                                           // (a treble-bleed cap across Rp adds to it: at AC, B+ is ground)
                                           // (Cgp (1 + A) + Cgk ~ 105 pF), which the partitioned model
                                           // would otherwise drop; it sets the ~25 kHz bandwidth of a
                                           // loaded 12AX7 stage
    double cgk = 1.6e-12, cgp = 1.7e-12;   // 12AX7 inter-electrode capacitances
    int supplyNode = 3;                    // which B+ filter node feeds the plate resistor
};

struct PowerSpec
{
    PentodeModel tube {};
    double raa = 3.4e3;            // plate-to-plate load
    double idleCurrent = 0.040;    // per tube; the fixed bias is calibrated to hit this at idle
    double rscreen = 1.0e3;
    double rpi = 40.0e3, cc = 22.0e-9, rgl = 220.0e3, rstop = 5.6e3, cgrid = 20.0e-12;
    double piGain = 1.2;           // phase-inverter swing per unit of drive, in multiples of the bias voltage
    double nfb = 0.35;             // negative feedback (normalised speaker voltage subtracted at the PI)
    double nfbCompensationHz = 6000.0;  // feedback-path low-pass: less feedback in the treble keeps the
                                        // loop stable into the rising speaker impedance (presence network)
    double piBandwidthHz = 24000.0;
    double piCurrent = 0.004;      // phase-inverter draw on its supply node
    // Output transformer (referred to the full primary) and speaker
    double lPrimary = 30.0, lLeakPrimary = 0.015, cPrimary = 0.8e-9, rWinding = 0.4, rCorePrimary = 80.0e3;
    double speakerOhms = 8.0, re = 6.4, le = 0.5e-3, r2 = 30.0, res = 40.0, resonanceHz = 85.0, resonanceQ = 4.0;
    double ratedWatts = 50.0;
};

/** Rectifier -> reservoir C1 (power-tube plates) -> choke or resistor -> C2 (screens)
    -> R -> C3 (phase inverter) -> R -> C4 (later preamp stages) -> R -> C5 (first stage). */
struct SupplySpec
{
    bool tubeRectifier = false;
    double vNoLoad = 470.0;
    double rSource = 35.0;         // solid-state: transformer winding + diode resistance
    double perveance = 1.0e-3;     // tube rectifier: I = P (Vnl - V1)^1.5 (Child-Langmuir)
    double c1 = 70.0e-6, l12 = 0.0, r12 = 100.0, c2 = 47.0e-6;
    double r23 = 4.7e3, c3 = 22.0e-6, r34 = 10.0e3, c4 = 22.0e-6, r45 = 10.0e3, c5 = 22.0e-6;
};

struct VoiceCircuit
{
    int numStages = 2;
    std::array<StageSpec, 3> stages {};
    double gainLoDb = -40.0, gainHiDb = 0.0;   // gain pot attenuation between stages 1 and 2
    double inputVolts = 2.0;                   // volts at the guitar input for a full-scale sample
    double toneScale = 0.01;                   // plate volts -> tone stack units
    double powerDrive = 1.0;                   // tone stack -> phase inverter
    double outputDb = 0.0;                     // level trim so the voices sit next to the Standard engine's
    PowerSpec power {};
    SupplySpec supply {};
};

/** The factory circuits behind the Clean, Classic and Modern voices. */
const std::array<VoiceCircuit, 3>& voiceCircuits();

//==============================================================================
// The whole amp
//==============================================================================

class PhysicalTubeStage
{
public:
    static constexpr int numSupplyNodes = 5;

    /** Builds every voice's matrices and DC operating point. Allocates; not real-time. */
    void prepare (double oversampledRate);
    /** Returns the active voice to its DC operating point (no allocation). */
    void reset() noexcept;
    /** Switching voice re-initialises the new circuit at its operating point. */
    void setVoice (int voiceIndex) noexcept;
    /** Gain knob 0..1: the volume pot between the first and second stage (audio taper). */
    void setGain (double knob01) noexcept;
    void setMathMode (MathMode m) noexcept { mathMode = m; }
    MathMode getMathMode() const noexcept { return mathMode; }

    /** In-place on oversampled samples: full-scale input in, roughly +-1 at full power out.
        `tone` is called once per sample between the preamp and the phase inverter. */
    template <class ToneStack>
    void process (float* io, int numSamples, ToneStack&& tone) noexcept
    {
        // The math policy and the stage count are template parameters: the choice is made
        // here, once per block, and the per-sample loop contains neither decision.
        const bool three = circuit->numStages == 3;
        if (mathMode == MathMode::exact)
            three ? run<ExactMath, 3> (io, numSamples, tone) : run<ExactMath, 2> (io, numSamples, tone);
        else
            three ? run<FastMath, 3> (io, numSamples, tone) : run<FastMath, 2> (io, numSamples, tone);
    }

    // Telemetry for the offline checks
    double supplyVoltage (int node) const noexcept { return live.supply.v[(size_t) node]; }
    double idleSupplyVoltage (int node) const noexcept { return initial[(size_t) voice].supply.v[(size_t) node]; }
    double biasVoltage() const noexcept { return live.bias; }
    double idlePlateVoltage (int stage) const noexcept { return derived[(size_t) voice].idlePlate[(size_t) stage]; }
    int numStages() const noexcept { return circuit != nullptr ? circuit->numStages : 0; }
    SolverStats preampStats() const noexcept { return live.preampStats; }
    SolverStats powerStats() const noexcept { return live.powerStats; }
    SolverStats gridStats() const noexcept { return live.gridStats; }
    void clearStats() noexcept { live.preampStats = {}; live.powerStats = {}; live.gridStats = {}; }

private:
    struct Supply
    {
        std::array<double, numSupplyNodes> v {};
        double chokeCurrent = 0.0;
    };

    /** Everything that changes while playing. Trivially copyable, so reset() is a copy. */
    struct State
    {
        std::array<TriodeStage, 3> stages {};
        GridStage gridA {}, gridB {};
        LoadNetwork load {};
        Supply supply {};
        double bias = 0.0;                 // magnitude of the fixed negative grid bias
        // power-stage Newton predictor: last solution, slope dR/dVh, drive factors and atan terms
        double vhPrev = 0.0, qPrev = 0.0, slopePrev = -1.0, kAPrev = 0.0, kBPrev = 0.0, atanA = 0.0, atanB = 0.0;
        double screenA = 0.0, screenB = 0.0, plateCurrent = 0.0;
        double speakerPrev = 0.0;          // one-sample-delayed feedback (2.8 us at 8x 44.1 kHz)
        double piLp = 0.0, couple = 0.0, coupleIn = 0.0, feedback = 0.0;
        std::array<double, 3> pipe {};       // each stage's previous output (pipeline registers)
        std::array<double, 2> gridPipe {};   // power-tube grid voltages, likewise
        SolverStats preampStats {}, powerStats {}, gridStats {};
    };

    /** Per-voice constants derived in prepare(). */
    struct Derived
    {
        double turns = 1.0;          // half-primary : secondary turns ratio
        double fullScaleVolts = 28.0;
        double dt = 0.0, piCoef = 0.0, couplingCoef = 0.0, nfbCoef = 0.0, outputGain = 1.0;
        std::array<double, 3> idlePlate {};
        struct SupplyConstants
        {
            std::array<double, numSupplyNodes> dtOverC {};
            double gSource = 0.0, dtOverL12 = 0.0, g12 = 0.0, g23 = 0.0, g34 = 0.0, g45 = 0.0;
        } supply;
    };

    /** The per-sample loop, software-pipelined across the circuit.

        Within one sample the circuit is a single serial chain (stage 1 -> stage 2 -> stage 3 ->
        grids -> power tubes), and each link is itself a chain of exp/log latencies. Run in that
        order, the core waits on one stage at a time. So every block in the chain here works on
        the value its predecessor produced in the *previous* iteration: within one iteration the
        stages are independent dependency chains that an out-of-order core executes side by
        side (on an M1 this roughly halves the cost of the Modern voice). The price is one
        oversampled sample of delay per block -- numStages + 1 samples, 11 us at 8x 44.1 kHz,
        well under one base-rate sample -- and nothing else: each stage still sees its own input
        sample by sample, in order. The supply, which moves on a millisecond scale, sees the
        stage currents a few microseconds apart; the feedback loop gains one sample of delay,
        which its compensation network already accounts for. */
    template <TubeMath M, int Stages, class ToneStack>
    void run (float* io, int numSamples, ToneStack& tone) noexcept
    {
        static_assert (Stages >= 2 && Stages <= 3);
        const auto& c = *circuit;
        auto& s = live;
        const auto& d = derived[(size_t) voice];
        for (int n = 0; n < numSamples; ++n)
        {
            gainNow += (gainTarget - gainNow) * gainCoef;
            const auto& v = s.supply.v;

            //-- Preamp. Each stage's B+ comes from its own filter node of the sagging supply.
            // The next stage's coupling cap blocks DC, so it is handed the plate's AC swing:
            // subtracting the constant idle voltage removes only an offset Cin blocks anyway,
            // and it means turning the gain pot (between stages 1 and 2) never steps the DC.
            const double in = (double) io[n] * c.inputVolts;
            std::array<double, 3> out {};
            out[0] = s.stages[0].template process<M> (in, v[(size_t) c.stages[0].supplyNode], s.preampStats);
            out[1] = s.stages[1].template process<M> ((s.pipe[0] - d.idlePlate[0]) * gainNow, v[(size_t) c.stages[1].supplyNode], s.preampStats);
            if constexpr (Stages == 3)
                out[2] = s.stages[2].template process<M> (s.pipe[1] - d.idlePlate[1], v[(size_t) c.stages[2].supplyNode], s.preampStats);

            //-- Output coupling capacitor of the last stage (first-order high-pass), then tone stack.
            const double last = s.pipe[(size_t) (Stages - 1)];
            const double ac = d.couplingCoef * (s.couple + last - s.coupleIn);
            s.coupleIn = last;
            s.couple = ac;
            const double toned = tone (ac * c.toneScale);

            //-- Phase inverter with global negative feedback from the speaker terminals.
            s.feedback += (s.speakerPrev - s.feedback) * d.nfbCoef;
            const double piIn = toned * c.powerDrive - c.power.nfb * s.feedback;
            s.piLp += (piIn - s.piLp) * d.piCoef;
            const double swing = c.power.piGain * s.bias * s.piLp;
            const double gA = s.gridA.template process<M> (swing, -s.bias, c.power.tube, s.gridStats);
            const double gB = s.gridB.template process<M> (-swing, -s.bias, c.power.tube, s.gridStats);

            //-- Push-pull pentodes into the reactive output transformer + speaker load, driven by
            //   the grid voltages from the previous iteration (pipelined like the preamp).
            const double speaker = powerStage<M> (s.gridPipe[0], s.gridPipe[1], d);
            s.speakerPrev = speaker / d.fullScaleVolts;

            //-- Every tube's current comes out of the supply capacitors.
            stepSupply (d);

            for (int st = 0; st < Stages; ++st)
                s.pipe[(size_t) st] = out[(size_t) st];
            s.gridPipe = { gA, gB };
            io[n] = (float) (s.speakerPrev * d.outputGain);
        }
    }

    /** Solves the plate side of the push-pull pair for the half-primary voltage Vh:

            plate A = B+ - Vh,  plate B = B+ + Vh,  Vh = n Vs,  Is = n (Ia - Ib)
            Vs = q + E Is   (q = D x: the load network's state contribution)

        The pentode currents factor into a grid/screen part (computed once) times
        atan(Vp / kvb), so each Newton iteration costs two atans. dR/dVh is always negative,
        so the 1-D Newton iteration converges monotonically. */
    template <TubeMath M>
    AXIOM_HOT double powerStage (double gA, double gB, const Derived& d) noexcept
    {
        const auto& p = circuit->power;
        auto& s = live;
        const double vScreen = s.supply.v[1];
        const double vg2A = vScreen - p.rscreen * s.screenA;
        const double vg2B = vScreen - p.rscreen * s.screenB;
        const auto driveA = p.tube.drive<M> (gA, vg2A);
        const auto driveB = p.tube.drive<M> (gB, vg2B);
        const double kA = driveA.plateScale, kB = driveB.plateScale;
        s.screenA = driveA.screen;
        s.screenB = driveB.screen;

        const auto& load = s.load.sys;
        const std::array<double, 0> none {};
        const double q = load.output (0, s.load.x, { 0.0 }, none);
        const double e = load.E[0];
        const double n = d.turns, bplus = s.supply.v[0], kvb = p.tube.kvb;

        // Tangent predictor: dVh = -(dR/dq dq + dR/dkA dkA + dR/dkB dkB) / (dR/dVh), all at
        // the last solution. The kA/kB terms carry the grid drive, i.e. the actual signal.
        double vh = s.vhPrev - (n * (q - s.qPrev)
                                + n * n * e * (s.atanA * (kA - s.kAPrev) - s.atanB * (kB - s.kBPrev))) / s.slopePrev;
        double ia = 0.0, ib = 0.0, slope = -1.0;
        int iter = 0;
        bool converged = false;
        // A plate cannot go below 0 V while conducting. A hard max() there leaves a kink that
        // Newton bounces across when a tube bottoms out, so the floor is smooth (same algebraic
        // knee as the grid diode, 2 V wide): plate = (v + sqrt(v^2 + w^2)) / 2.
        constexpr double w2 = 4.0;
        // Bracket from the physics: each pentode's current lies between 0 and its saturation
        // value k pi/2, which bounds Vh = n (q + E n (Ia - Ib)) on both sides.
        const double satA = n * n * e * kA * std::numbers::pi * 0.5, satB = n * n * e * kB * std::numbers::pi * 0.5;
        double lo = n * q - satB - 1.0, hi = n * q + satA + 1.0;
        vh = std::clamp (vh, lo, hi);
        for (;;)
        {
            const double rawA = bplus - vh, rawB = bplus + vh;
            const double rootA = std::sqrt (rawA * rawA + w2), rootB = std::sqrt (rawB * rawB + w2);
            const double va = 0.5 * (rawA + rootA), vb = 0.5 * (rawB + rootB);
            ia = kA * M::atan (va / kvb);
            ib = kB * M::atan (vb / kvb);
            ++iter;
            const double r = n * (q + e * n * (ia - ib)) - vh;
            // dIa/dVh = -dA, dIb/dVh = +dB
            const double dA = kA * kvb / (kvb * kvb + va * va) * 0.5 * (1.0 + rawA / rootA);
            const double dB = kB * kvb / (kvb * kvb + vb * vb) * 0.5 * (1.0 + rawB / rootB);
            slope = -n * n * e * (dA + dB) - 1.0;
            converged = std::abs (r) < newtonTolerance;
            if (converged || iter >= maxIterations1D)
                break;
            // Safeguarded Newton (rtsafe): R(Vh) is strictly decreasing, so each evaluation
            // tells which side of the root it is on. A Newton step that would leave the bracket
            // -- the 2-cycle a tube bottoming out in its knee otherwise causes -- is replaced by
            // bisection. Selects only, no extra evaluations.
            (r > 0.0 ? lo : hi) = vh;
            const double newton = vh - r / slope;
            const double next = (newton >= lo && newton <= hi) ? newton : 0.5 * (lo + hi);
            const double step = next - vh;
            vh = next;
            if (std::abs (step) < newtonAcceptStep)
            {
                ia -= dA * step;
                ib += dB * step;
                converged = true;
                break;
            }
        }
        s.powerStats.add (iter, ! converged);
        s.slopePrev = slope;
        s.kAPrev = kA;
        s.kBPrev = kB;
        s.atanA = ia / std::max (kA, 1.0e-30);
        s.atanB = ib / std::max (kB, 1.0e-30);
        s.vhPrev = vh;
        s.qPrev = q;

        const std::array<double, 1> u { n * (ia - ib) };
        const double speaker = load.output (1, s.load.x, u, none);
        load.advance (s.load.x, u, none);
        s.plateCurrent = ia + ib;
        return speaker;
    }

    /** Semi-implicit Euler on the supply network. The smallest time constant (rectifier
        resistance x reservoir) is milliseconds against a 2.8 us step, so explicit current
        evaluation is accurate and unconditionally stable here. */
    AXIOM_HOT void stepSupply (const Derived& d) noexcept
    {
        const auto& sp = circuit->supply;
        const auto& k = d.supply;
        auto& v = live.supply.v;

        // Rectifier: it only ever charges the reservoir. Solid state: a resistance.
        // Tube (GZ34-style): a vacuum diode, Child-Langmuir I = P (Vnl - V1)^1.5.
        const double head = std::max (sp.vNoLoad - v[0], 0.0);
        const double rect = sp.tubeRectifier ? sp.perveance * head * std::sqrt (head) : head * k.gSource;

        double& il = live.supply.chokeCurrent;
        il = sp.l12 > 0.0 ? il + k.dtOverL12 * (v[0] - v[1] - sp.r12 * il)    // choke: L di/dt = v - R i
                          : (v[0] - v[1]) * k.g12;

        double late = 0.0;
        for (int st = 1; st < circuit->numStages; ++st)
            late += live.stages[(size_t) st].supplyCurrent;
        const double first = live.stages[0].supplyCurrent;
        const double i23 = (v[1] - v[2]) * k.g23, i34 = (v[2] - v[3]) * k.g34, i45 = (v[3] - v[4]) * k.g45;

        v[0] += k.dtOverC[0] * (rect - il - live.plateCurrent);
        v[1] += k.dtOverC[1] * (il - i23 - live.screenA - live.screenB);
        v[2] += k.dtOverC[2] * (i23 - i34 - circuit->power.piCurrent);
        v[3] += k.dtOverC[3] * (i34 - i45 - late);
        v[4] += k.dtOverC[4] * (i45 - first);
    }

    void initialiseVoice (int index);

    std::array<State, 3> initial {};
    std::array<Derived, 3> derived {};
    State live {};
    const VoiceCircuit* circuit = nullptr;
    int voice = 1;
    MathMode mathMode = MathMode::fast;
    double fs = 352800.0;
    double gainKnob = 0.35, gainTarget = 0.1, gainNow = 0.1, gainCoef = 0.0;
};
} // namespace axiom::physical

#undef AXIOM_HOT
