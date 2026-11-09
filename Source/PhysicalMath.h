#pragma once
/*
    Math and device equations for the Physical engine (see PhysicalTubeStage.h).

    Everything here is evaluated inside the per-sample Newton-Raphson loops, so it is written
    to inline completely, to avoid library calls in the Fast policy, and to make data-dependent
    choices with selects rather than branches.
*/
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <numbers>

#if defined(__GNUC__) || defined(__clang__)
 #define AXIOM_HOT [[gnu::always_inline]] inline
#else
 #define AXIOM_HOT inline
#endif
#if defined(__clang__)
 // Full unrolling of the small fixed-size matrix loops: their trip counts are compile-time
 // constants once the outer loop is unrolled, and straight-line code lets the compiler schedule
 // and vectorise the ~200 multiply-adds of an 8x8 LU instead of paying loop overhead on each.
 #define AXIOM_UNROLL _Pragma ("clang loop unroll(full)")
#else
 #define AXIOM_UNROLL
#endif

namespace axiom::physical
{
enum class MathMode { fast = 0, exact = 1 };

//==============================================================================
// Math policies
//==============================================================================

/** Table-driven exp and log and an Estrin-scheme atan. Inside the solver these sit in serial
    dependency chains (a cathode-current evaluation is exp -> log -> log -> exp), so latency is
    what matters: a 64-entry table shrinks the reduced argument to |r| < 0.008, leaving a
    degree-4/5 polynomial evaluated in two or three FMA levels. No library calls, no branches.
    Accuracy: ~1e-13 relative for exp and log, 2e-8 absolute for atan; the Newton tolerance is
    1e-6 V. */
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

/** Table/polynomial math. The default.

    Iteration cap (both modes): 10. A cap of 4-6 is plenty for a single triode, and it is the
    usual figure, but these are coupled 6- and 8-port circuits: when a high-gain preamp is hit
    with hard high-frequency drive, a grid can swing across cutoff and into conduction within
    one sample, and Newton then needs up to ~10 passes. Capped at 6, ~2% of such samples ended
    short, and the small charge errors they left on the coupling capacitors added up to clearly
    audible low-frequency junk (measured against a 30-iteration reference: gone at 10, still
    there at 8). On ordinary playing the solve still finishes in ~1.1-1.4 passes; the cap only
    bounds the rare worst case. The two modes differ in how exp, log and atan are computed. */
struct FastMath
{
    static constexpr int maxIterations = 10;
    AXIOM_HOT static double exp (double x) noexcept { return fast::exp (x); }
    AXIOM_HOT static double log (double x) noexcept { return fast::log (x); }
    AXIOM_HOT static double atan (double x) noexcept { return fast::atan (x); }
};

/** The C library's exp, log and atan. Same iteration cap. */
struct ExactMath
{
    static constexpr int maxIterations = 10;
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

/** Iteration cap for the one-dimensional push-pull plate solve, in both modes: a pass costs two
    atans and two square roots, so the headroom is cheap. */
inline constexpr int maxIterations1D = 10;

/** A Newton step smaller than this is applied and the solve stops without evaluating the tubes
    again. Newton's error is quadratic: after a step d the residual is about |K| |d2I/dv2| d^2/2.
    With |K| <= 1e5 ohm and |d2I/dv2| <= 1e-3 A/V^2 for a 12AX7 (worst cases, at the onset of
    grid conduction) that is <= 5e-8 V at d = 3e-5 V, well inside the 1e-6 V tolerance. The
    currents are moved along the same tangent, keeping the state update consistent. */
inline constexpr double newtonAcceptStep = 3.0e-5;

/** ln(1 + e^t) and its derivative, the logistic sigma(t), without overflow:
    softplus(t) = max(t, 0) + ln(1 + e^-|t|). One exp and one log for both. */
struct Softplus { double value, slope; };

template <TubeMath M>
AXIOM_HOT Softplus softplus (double t) noexcept
{
    const double e = M::exp (-std::abs (t));
    const double onePlus = 1.0 + e;
    return { std::max (t, 0.0) + M::log (onePlus), (t >= 0.0 ? 1.0 : e) / onePlus };
}

//==============================================================================
// Device equations
//==============================================================================

/** Child-Langmuir grid conduction, I = G s^1.5, with a smooth algebraic onset
    s = (v + sqrt(v^2 + w^2)) / 2. Above the onset s -> v (the 3/2-power law of a vacuum diode);
    below it the current falls off as (w^2 / 4|v|)^1.5, i.e. nanoamps a few volts negative.
    Square roots only: it is evaluated on every Newton pass of every tube. */
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

/** The 12AX7 (ECC83), after Dempwolf & Zoelzer (2011), whose parameters were fitted to measured
    tubes. The cathode current is a smoothed 3/2-power-style law in the effective grid voltage

        Ik = G ( ln(1 + exp(C (Vpk/mu + Vgk))) / C )^gamma

    and the grid takes its share of it: Ip = Ik - Ig. So when the grid conducts, the plate
    current drops -- the physical coupling a separate-equations model misses. Grid current uses
    the Child-law diode above, matched to Dempwolf's measured grid curve over 0..2 V (within
    ~15%) at a quarter of the cost of a second exp/log/pow chain.

    Ports per triode: grid current g->k (controlled by Vgk), plate current p->k (by Vpk).
    Analytic Jacobian, hand-derived:
        u = C (Vpk/mu + Vgk),  s = softplus(u)/C,  dIk/dVgk = G gamma s^(gamma-1) sigma(u),
        dIk/dVpk = dIk/dVgk / mu,  dIp/dVgk = dIk/dVgk - dIg/dVgk,  dIp/dVpk = dIk/dVpk. */
struct TriodeModel
{
    double G = 2.242e-3, mu = 103.2, gamma = 1.26, C = 3.40;
    GridDiode grid { 6.2e-4, 0.3 };   // A/V^1.5; 0.3 V onset (contact potential + electron velocity spread)

    struct Result { double ig, ip, dIg, dIpG, dIpP; };

    template <TubeMath M>
    AXIOM_HOT Result evaluate (double vgk, double vpk) const noexcept
    {
        const auto sp = softplus<M> (C * (vpk / mu + vgk));
        const double s = std::max (sp.value / C, 1.0e-300);
        const double sPow = M::exp ((gamma - 1.0) * M::log (s));   // s^(gamma-1)
        const double ik = G * sPow * s;
        const double dIk = G * gamma * sPow * sp.slope;
        const auto g = grid (vgk);   // independent of the cathode chain: overlaps on an OoO core
        return { g.i, ik - g.i, g.di, dIk - g.di, dIk / mu };
    }
};

/** Koren's beam-tetrode/pentode equations for the power tubes, with the cathode current shared
    between plate and screen:

        E1  = (Vg2 / kp) ln(1 + exp(kp (1/mu + Vg1 / Vg2)))
        Ip  = (2 E1^ex / kg1) atan(Vp / kvb)
        Ig2 = E1^ex / kg2 + capture (2 E1^ex / kg1) (pi/2 - atan(Vp / kvb))

    The second screen term is current the plate fails to collect once it swings down into the
    knee: it lands on the screen instead, which is what overloads the screen supply and
    compresses a cranked power section. `capture` is the share that does. */
struct PentodeModel
{
    double mu, ex, kg1, kg2, kp, kvb;
    GridDiode grid;
    double capture = 0.5;

    struct Drive { double plateScale, screenBase; };   // plate current = plateScale atan(Vp / kvb)

    template <TubeMath M>
    AXIOM_HOT Drive drive (double vg1, double vg2) const noexcept
    {
        const auto sp = softplus<M> (kp * (1.0 / mu + vg1 / vg2));
        const double e1Pow = M::exp (ex * M::log (std::max (vg2 / kp * sp.value, 1.0e-12)));
        return { (2.0 / kg1) * e1Pow, e1Pow / kg2 };
    }

    double screenCurrent (const Drive& d, double plateAtan) const noexcept
    {
        return d.screenBase + capture * d.plateScale * (std::numbers::pi * 0.5 - plateAtan);
    }
};
} // namespace axiom::physical
