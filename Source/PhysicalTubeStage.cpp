#include "PhysicalTubeStage.h"

namespace axiom::physical
{
namespace
{
constexpr int gnd = Netlist::gnd;

//==============================================================================
// Component values. Original circuits in the style of each amp family, not copies.
//==============================================================================

struct SupplySpec
{
    bool tubeRectifier;
    double vPeak;          // rectified mains peak (no-load B+)
    double rSource;        // solid state: winding + diode resistance
    double perveance;      // tube rectifier: I = P (Vac - V1)^1.5 while conducting
    double c1, l12, r12, c2, r23, c3, r34, c4, r45, c5;
};

struct PowerSpec
{
    PentodeModel tube;
    double mismatch;       // second tube's kg1 relative to the first: an unmatched pair
    double raa, idleCurrent, rscreen, ratedWatts, plateDissipation;
    // Output transformer, referred to the full primary
    double lPrimary, lLeakPrimary, cPrimary, rWinding, rCorePrimary, saturationHz;
    // Speaker (8 ohm)
    double re, le, r2, res, resonanceHz, resonanceQ;
};

struct PiSpec
{
    double rPlate1, rPlate2, rkBias, rTail, rShunt, rFeedback, cIn, cPresence, presencePot;
    double cCouple, rGridLeak, rStopper, cGrid, feedbackHz;
};

struct VoiceSpec
{
    SupplySpec supply;
    PowerSpec power;
    PiSpec pi;
    double inputVolts, outputDb;
    double treblePot, bassPot, midPot;   // tone-stack pot values (gain pot: 1M audio throughout)
};

constexpr PentodeModel el34 { .mu = 11.0, .ex = 1.35, .kg1 = 650.0, .kg2 = 4200.0, .kp = 60.0, .kvb = 24.0, .grid = { 2.0e-3, 0.2 }, .capture = 0.5 };
constexpr PentodeModel sixL6 { .mu = 8.7, .ex = 1.35, .kg1 = 1460.0, .kg2 = 4500.0, .kp = 48.0, .kvb = 12.0, .grid = { 2.0e-3, 0.2 }, .capture = 0.5 };

constexpr PowerSpec powerSpec (PentodeModel tube, double raa, double idle, double rscreen, double dissipation)
{
    return { tube, 1.05, raa, idle, rscreen, 50.0, dissipation,
             30.0, 0.015, 0.8e-9, 0.4, 80.0e3, 30.0,
             6.4, 0.5e-3, 30.0, 40.0, 85.0, 4.0 };
}

constexpr PiSpec marshallPi { 82.0e3, 100.0e3, 470.0, 10.0e3, 4.7e3, 68.0e3, 22.0e-9, 0.1e-6, 5.0e3, 22.0e-9, 220.0e3, 5.6e3, 20.0e-12, 20000.0 };
constexpr PiSpec fenderPi   { 82.0e3, 100.0e3, 1.0e3, 22.0e3, 4.7e3, 47.0e3, 22.0e-9, 0.1e-6, 5.0e3, 0.1e-6, 220.0e3, 1.5e3, 20.0e-12, 20000.0 };

const std::array<VoiceSpec, numVoices>& specs()
{
    static const std::array<VoiceSpec, numVoices> s {{
        // Clean: blackface-style. V1A -> Fender tone stack -> volume (120 pF bright cap) -> V1B.
        // 6L6 pair on a stiff solid-state supply with a choke.
        { { false, 470.0, 40.0, 0.0, 70.0e-6, 4.0, 100.0, 20.0e-6, 4.7e3, 20.0e-6, 10.0e3, 20.0e-6, 10.0e3, 20.0e-6 },
          powerSpec (sixL6, 4.2e3, 0.035, 470.0, 30.0), fenderPi, 2.0, -18.8, 250.0e3, 250.0e3, 10.0e3 },
        // Classic: plexi-style. V1A -> volume (5 nF bright cap) -> 470k mixer -> V2A -> cathode
        // follower -> Marshall tone stack. EL34 pair on a GZ34-style tube rectifier: it sags.
        { { true, 500.0, 0.0, 1.0e-3, 32.0e-6, 0.0, 1.0e3, 32.0e-6, 10.0e3, 32.0e-6, 10.0e3, 32.0e-6, 10.0e3, 32.0e-6 },
          powerSpec (el34, 3.4e3, 0.040, 1.0e3, 25.0), marshallPi, 2.0, -18.5, 220.0e3, 1.0e6, 25.0e3 },
        // Modern: hot-rodded. V1A -> gain (470 pF bright cap) -> V1B, a cold clipper (10k
        // cathode) -> 470k||470pF interstage -> V2A -> cathode follower -> Marshall tone stack.
        // EL34 pair on a stiff solid-state supply with a choke.
        { { false, 480.0, 25.0, 0.0, 100.0e-6, 5.0, 60.0, 50.0e-6, 10.0e3, 32.0e-6, 10.0e3, 32.0e-6, 10.0e3, 32.0e-6 },
          powerSpec (el34, 3.4e3, 0.040, 1.0e3, 25.0), marshallPi, 2.0, -19.2, 220.0e3, 1.0e6, 25.0e3 },
    }};
    return s;
}

/** A logarithmic ("audio", 10%) taper. */
double audioTaper (double a) { return (std::pow (10.0, 2.0 * std::clamp (a, 0.0, 1.0)) - 1.0) / 99.0; }

/** Resistances of every pot section for a set of rotations. */
struct PotValues { double gainTop, gainBottom, trebleTop, trebleBottom, bass, mid, presence; };

PotValues potValues (int v, const Pots& p)
{
    const auto& s = specs()[(size_t) v];
    const double g = audioTaper (p.gain), t = std::clamp (p.treble, 0.0, 1.0);
    return { 1.0e6 * (1.0 - g), 1.0e6 * g,
             s.treblePot * (1.0 - t), s.treblePot * t,
             s.bassPot * audioTaper (p.bass),
             s.midPot * std::clamp (p.mid, 0.0, 1.0),
             s.pi.presencePot * (1.0 - std::clamp (p.presence, 0.0, 1.0)) };
}

/** Handles of the pot resistors: gainTop, gainBottom, trebleTop, trebleBottom, bass, mid, presence. */
using Handles = std::array<int, 7>;

void stage (Netlist& n, int g, int p, int k, int b, double rk, double ck)
{
    n.resistor (k, gnd, rk);
    if (ck > 0.0) n.capacitor (k, gnd, ck);
    n.resistor (b, p, 100.0e3);
    n.capacitor (g, p, 1.7e-12);   // Cgp: its Miller multiple loads whatever drives the grid
}

/** Passive treble/bass/middle network: C1 from the driver to the treble pot, slope resistor to
    the bass and middle caps, bass and middle pots as rheostats to ground. */
void toneStack (Netlist& n, int in, int T, int O, int B, int Cn, int A, double c1, double slope, double c2, double c3,
                const PotValues& pv, Handles& h)
{
    n.capacitor (in, T, c1);
    h[2] = n.resistor (T, O, pv.trebleTop);
    h[3] = n.resistor (O, B, pv.trebleBottom);
    n.resistor (in, A, slope);
    n.capacitor (A, B, c2);
    n.capacitor (A, Cn, c3);
    h[4] = n.resistor (B, Cn, pv.bass);
    h[5] = n.resistor (Cn, gnd, pv.mid);
}

Netlist buildPreamp (int v, const Pots& pots, Handles& h)
{
    const auto pv = potValues (v, pots);
    h.fill (-1);
    if (v == 0)
    {
        enum { in, g1, k1, p1, b1, T, O, B, Cn, A, w, g2, k2, p2, b2, mc, mx, count };
        Netlist n (count);
        const char* names[] { "in", "g1", "k1", "p1", "b1", "tt", "o", "tb", "tm", "ts", "w", "g2", "k2", "p2", "b2", "mc", "mx" };
        for (int i = 0; i < count; ++i) n.name (i, names[i]);
        n.voltageInput (in, gnd); n.voltageInput (b1, gnd); n.voltageInput (b2, gnd);
        n.triode (g1, p1, k1); n.triode (g2, p2, k2);
        n.resistor (in, g1, 34.0e3);
        stage (n, g1, p1, k1, b1, 1.5e3, 25.0e-6);
        toneStack (n, p1, T, O, B, Cn, A, 250.0e-12, 100.0e3, 0.1e-6, 0.047e-6, pv, h);
        h[0] = n.resistor (O, w, pv.gainTop);
        h[1] = n.resistor (w, gnd, pv.gainBottom);
        n.capacitor (O, w, 120.0e-12);   // bright cap
        n.resistor (w, g2, 1.0e3);
        stage (n, g2, p2, k2, b2, 1.5e3, 25.0e-6);
        n.capacitor (p2, mc, 22.0e-9);   // blackface-style mixer: 220k into 220k, about -6 dB
        n.resistor (mc, mx, 220.0e3);
        n.resistor (mx, gnd, 220.0e3);
        n.output (mx, gnd); n.sourceCurrent (1); n.sourceCurrent (2);
        return n;
    }
    if (v == 1)
    {
        enum { in, g1, k1, p1, b1, c, w, g2, k2, p2, b2, kc, T, O, B, Cn, A, count };
        Netlist n (count);
        const char* names[] { "in", "g1", "k1", "p1", "b1", "c", "w", "g2", "k2", "p2", "b2", "kc", "tt", "o", "tb", "tm", "ts" };
        for (int i = 0; i < count; ++i) n.name (i, names[i]);
        n.voltageInput (in, gnd); n.voltageInput (b1, gnd); n.voltageInput (b2, gnd);
        n.triode (g1, p1, k1); n.triode (g2, p2, k2); n.triode (p2, b2, kc);   // last: the cathode follower
        n.resistor (in, g1, 34.0e3);
        stage (n, g1, p1, k1, b1, 2.7e3, 0.68e-6);
        n.capacitor (p1, c, 22.0e-9);
        h[0] = n.resistor (c, w, pv.gainTop);
        h[1] = n.resistor (w, gnd, pv.gainBottom);
        n.capacitor (c, w, 470.0e-12);   // bright cap
        n.resistor (w, g2, 470.0e3);     // mixing resistor: with V2A's Miller capacitance, the plexi's treble roll-off
        stage (n, g2, p2, k2, b2, 820.0, 0.68e-6);
        n.resistor (kc, gnd, 100.0e3);   // (a follower's Cgp has no Miller gain: left out)
        toneStack (n, kc, T, O, B, Cn, A, 470.0e-12, 33.0e3, 22.0e-9, 22.0e-9, pv, h);
        n.output (O, gnd); n.sourceCurrent (1); n.sourceCurrent (2);
        return n;
    }
    enum { in, g1, k1, p1, b1, c, w, g2, k2, p2, b2, X, g3, k3, p3, kc, T, O, B, Cn, A, mv, count };
    Netlist n (count);
    const char* names[] { "in", "g1", "k1", "p1", "b1", "c", "w", "g2", "k2", "p2", "b2", "x", "g3", "k3", "p3", "kc", "tt", "o", "tb", "tm", "ts", "mv" };
    for (int i = 0; i < count; ++i) n.name (i, names[i]);
    n.voltageInput (in, gnd); n.voltageInput (b1, gnd); n.voltageInput (b2, gnd);
    n.triode (g1, p1, k1); n.triode (g2, p2, k2); n.triode (g3, p3, k3); n.triode (p3, b2, kc);
    n.resistor (in, g1, 34.0e3);
    stage (n, g1, p1, k1, b1, 2.7e3, 0.68e-6);
    n.capacitor (p1, c, 22.0e-9);
    h[0] = n.resistor (c, w, pv.gainTop);
    h[1] = n.resistor (w, gnd, pv.gainBottom);
    n.capacitor (c, w, 470.0e-12);
    n.resistor (w, g2, 47.0e3);
    stage (n, g2, p2, k2, b2, 10.0e3, 0.68e-6);   // cold clipper: idles near cutoff
    n.capacitor (p2, X, 22.0e-9);
    n.resistor (X, g3, 470.0e3);                  // 470k || 470 pF: attenuates, passes treble
    n.capacitor (X, g3, 470.0e-12);
    n.resistor (g3, gnd, 1.0e6);
    stage (n, g3, p3, k3, b2, 820.0, 0.68e-6);
    n.resistor (kc, gnd, 100.0e3);
    toneStack (n, kc, T, O, B, Cn, A, 470.0e-12, 33.0e3, 22.0e-9, 22.0e-9, pv, h);
    // Master volume (1M), as on the amps this voice is in the style of, at a fixed typical
    // setting (-30 dB): the preamp does the distorting and the power amp is driven, not slammed.
    // Axiom's own Master knob stays an output level, so presets keep their meaning.
    n.resistor (O, mv, 970.0e3);
    n.resistor (mv, gnd, 30.0e3);
    n.output (mv, gnd); n.sourceCurrent (1); n.sourceCurrent (2);
    return n;
}

/** Long-tail pair: input grid g1 coupled from the preamp, grid g2 AC-referenced to the
    feedback node f, which the speaker drives through Rfb against Rshunt and the presence pot +
    cap (the presence control is the frequency-dependent part of the negative feedback). Plates
    couple through Cc, grid leaks (to the bias supply) and stoppers to the power-tube grids. */
Netlist buildPi (int v, const Pots& pots, Handles& h)
{
    const auto& s = specs()[(size_t) v].pi;
    const auto pv = potValues (v, pots);
    enum { in, g1, g2, k, t, f, m, p1, p2, b, c1, c2, gA, gB, bias, fb, count };
    Netlist n (count);
    const char* names[] { "in", "g1", "g2", "k", "t", "f", "m", "p1", "p2", "b", "c1", "c2", "ga", "gb", "bias", "fb" };
    for (int i = 0; i < count; ++i) n.name (i, names[i]);
    n.voltageInput (in, gnd); n.voltageInput (b, gnd); n.voltageInput (bias, gnd); n.voltageInput (fb, gnd);
    n.currentInput (gA, gnd); n.currentInput (gB, gnd);   // grid emission of hot power tubes
    n.triode (g1, p1, k); n.triode (g2, p2, k);
    n.gridDiode (gA, gnd); n.gridDiode (gB, gnd);
    n.capacitor (in, g1, s.cIn);
    n.resistor (g1, t, 1.0e6);
    n.resistor (g2, t, 1.0e6);
    n.capacitor (g2, f, 0.1e-6);
    n.resistor (k, t, s.rkBias);
    n.resistor (t, gnd, s.rTail);
    n.resistor (f, gnd, s.rShunt);
    n.resistor (fb, f, s.rFeedback);
    n.capacitor (f, m, s.cPresence);
    h[6] = n.resistor (m, gnd, pv.presence + 10.0);
    n.resistor (b, p1, s.rPlate1);
    n.resistor (b, p2, s.rPlate2);
    n.capacitor (g1, p1, 1.7e-12);
    n.capacitor (p1, c1, s.cCouple);
    n.resistor (c1, bias, s.rGridLeak);
    n.resistor (c1, gA, s.rStopper);
    n.capacitor (gA, gnd, s.cGrid);
    n.capacitor (p2, c2, s.cCouple);
    n.resistor (c2, bias, s.rGridLeak);
    n.resistor (c2, gB, s.rStopper);
    n.capacitor (gB, gnd, s.cGrid);
    n.sourceCurrent (1); n.output (gA, gnd); n.output (gB, gnd);
    return n;
}

/** Output transformer and speaker referred to the secondary, without the magnetising branch
    (nonlinear: the power stage handles it from the core flux):
        Is -> s: Cw (winding capacitance) and Rcore (core loss) to ground,
                 Rw -- Lleak -- speaker: Re -- (Le || R2) -- (Res || Les || Ces) -- ground */
Netlist buildLoad (const PowerSpec& p, double turns)
{
    enum { sNode, n1, n2, n3, n4, count };
    Netlist n (count);
    const double ratio2 = 4.0 * turns * turns;
    const double w0 = 2.0 * std::numbers::pi * p.resonanceHz;
    const double les = p.res / (p.resonanceQ * w0), ces = 1.0 / (w0 * w0 * les);
    n.currentInput (sNode, gnd);
    n.capacitor (sNode, gnd, p.cPrimary * ratio2);
    n.resistor (sNode, gnd, p.rCorePrimary / ratio2);
    n.resistor (sNode, n1, p.rWinding);
    n.inductor (n1, n2, p.lLeakPrimary / ratio2);
    n.resistor (n2, n3, p.re);
    n.inductor (n3, n4, p.le);
    n.resistor (n3, n4, p.r2);
    n.resistor (n4, gnd, p.res);
    n.inductor (n4, gnd, les);
    n.capacitor (n4, gnd, ces);
    n.output (sNode, gnd);
    n.output (n2, gnd);
    return n;
}

/** The fixed grid bias that gives the first power tube its idle current. */
double calibrateBias (const PentodeModel& tube, double vPlate, double vScreen, double rscreen, double idle, double& screenOut)
{
    const auto idleAt = [&] (double vbias, double& screen)
    {
        double ig2 = 0.0, vg2 = vScreen;
        for (int k = 0; k < 30; ++k)
        {
            vg2 = vScreen - rscreen * ig2;
            const auto d = tube.drive<ExactMath> (-vbias, vg2);
            ig2 = tube.screenCurrent (d, std::atan (vPlate / tube.kvb));
        }
        screen = ig2;
        return tube.drive<ExactMath> (-vbias, vg2).plateScale * std::atan (vPlate / tube.kvb);
    };
    double lo = 0.0, hi = 250.0, s = 0.0;
    for (int k = 0; k < 80; ++k)
    {
        const double mid = 0.5 * (lo + hi);
        (idleAt (mid, s) > idle ? lo : hi) = mid;
    }
    const double bias = 0.5 * (lo + hi);
    idleAt (bias, screenOut);
    return bias;
}
} // namespace

//==============================================================================
template <> auto& PhysicalTubeStage::live<0>() noexcept { return clean; }
template <> auto& PhysicalTubeStage::live<1>() noexcept { return classic; }
template <> auto& PhysicalTubeStage::live<2>() noexcept { return modern; }
template <> auto& PhysicalTubeStage::settled<0>() noexcept { return cleanSettled; }
template <> auto& PhysicalTubeStage::settled<1>() noexcept { return classicSettled; }
template <> auto& PhysicalTubeStage::settled<2>() noexcept { return modernSettled; }
template <> const auto& PhysicalTubeStage::live<0>() const noexcept { return clean; }
template <> const auto& PhysicalTubeStage::live<1>() const noexcept { return classic; }
template <> const auto& PhysicalTubeStage::live<2>() const noexcept { return modern; }
template <> const auto& PhysicalTubeStage::settled<0>() const noexcept { return cleanSettled; }
template <> const auto& PhysicalTubeStage::settled<1>() const noexcept { return classicSettled; }
template <> const auto& PhysicalTubeStage::settled<2>() const noexcept { return modernSettled; }

/** Push-pull plates. Solves for the half-primary voltage Vh:
        plate A = B+ - Vh,  plate B = B+ + Vh,  Vh = n Vs,  Vs = q + E (n (Ia - Ib) - Im)
    with q the load network's state part and Im the magnetising current of the core (from its
    flux, explicit: the core's time constant is milliseconds). Pentode current factors into a
    grid/screen part (computed once) times atan(Vp / kvb), so a pass costs two atans.
    R(Vh) is strictly decreasing: safeguarded Newton (rtsafe) with a bracket from each tube's
    saturation current, a tangent predictor that includes the change in grid drive, and a
    smooth 0 V plate floor so a tube bottoming out in its knee does not kink the residual. */
template <TubeMath M, class S>
AXIOM_HOT double PhysicalTubeStage::powerStage (S& s, const Derived& d) noexcept
{
    auto& pw = s.power;
    const double vScreen = s.supply.v[1], bplus = s.supply.v[0];
    const auto driveA = d.tubeA.template drive<M> (s.gridPipe[0], vScreen - d.rscreen * pw.screenA);
    const auto driveB = d.tubeB.template drive<M> (s.gridPipe[1], vScreen - d.rscreen * pw.screenB);
    const double kA = driveA.plateScale, kB = driveB.plateScale;

    const std::array<double, 0> none {};
    const double e = pw.load.E[0];
    const double q = pw.load.outputs (pw.x, { 0.0 }, none)[0] - e * pw.magnetising;
    const double n = d.turns, kvb = d.tubeA.kvb;

    double vh = pw.vhPrev - (n * (q - pw.qPrev) + n * n * e * (pw.atanA * (kA - pw.kAPrev) - pw.atanB * (kB - pw.kBPrev))) / pw.slopePrev;
    const double satA = n * n * e * kA * std::numbers::pi * 0.5, satB = n * n * e * kB * std::numbers::pi * 0.5;
    double lo = n * q - satB - 1.0, hi = n * q + satA + 1.0;
    vh = std::clamp (vh, lo, hi);
    constexpr double w2 = 4.0;   // 2 V wide plate floor
    double ia = 0.0, ib = 0.0, slope = -1.0, atA = 0.0, atB = 0.0, va = bplus, vb = bplus;
    int iter = 0;
    bool converged = false;
    for (;;)
    {
        const double rawA = bplus - vh, rawB = bplus + vh;
        const double rootA = std::sqrt (rawA * rawA + w2), rootB = std::sqrt (rawB * rawB + w2);
        va = 0.5 * (rawA + rootA);
        vb = 0.5 * (rawB + rootB);
        atA = M::atan (va / kvb);
        atB = M::atan (vb / kvb);
        ia = kA * atA;
        ib = kB * atB;
        ++iter;
        const double r = n * (q + e * n * (ia - ib)) - vh;
        const double dA = kA * kvb / (kvb * kvb + va * va) * 0.5 * (1.0 + rawA / rootA);
        const double dB = kB * kvb / (kvb * kvb + vb * vb) * 0.5 * (1.0 + rawB / rootB);
        slope = -n * n * e * (dA + dB) - 1.0;
        converged = std::abs (r) < newtonTolerance;
        if (converged || iter >= maxIterations1D)
            break;
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
    pw.stats.add (iter, ! converged);
    pw.vhPrev = vh;
    pw.qPrev = q;
    pw.slopePrev = slope;
    pw.kAPrev = kA;
    pw.kBPrev = kB;
    pw.atanA = atA;
    pw.atanB = atB;

    const std::array<double, 1> u { n * (ia - ib) - pw.magnetising };
    const auto y = pw.load.outputs (pw.x, u, none);
    pw.load.advance (pw.x, u, none);

    // Core: flux is the integral of the winding voltage. The magnetising current rises steeply
    // past saturation (anhysteretic term) and lags the flux by a coercive current that flips
    // with direction (a Prandtl play hysteron), so low notes at high power saturate first and
    // small signals see the core's hysteresis.
    pw.flux = std::clamp (pw.flux + 0.5 * d.dt * (y[0] + pw.vsPrev), -2.5 * d.fluxSaturation, 2.5 * d.fluxSaturation);
    pw.vsPrev = y[0];
    pw.hysteron = std::clamp (pw.hysteron, pw.flux - d.hysteronWidth, pw.flux + d.hysteronWidth);
    const double rel = pw.flux / d.fluxSaturation, rel2 = rel * rel;
    pw.magnetising = pw.flux / d.lMagnetising * (1.0 + rel2 * rel2 * rel2)
                   + d.coerciveCurrent * (pw.flux - pw.hysteron) / d.hysteronWidth;

    pw.screenA = d.tubeA.screenCurrent (driveA, atA);
    pw.screenB = d.tubeB.screenCurrent (driveB, atB);
    pw.plateA = ia;
    pw.plateB = ib;
    pw.vPlateA = va;
    pw.vPlateB = vb;
    return y[1];
}

/** The supply at the base rate (its fastest time constant is milliseconds): 50 Hz mains through
    the rectifier into the reservoir, then choke/resistor and RC nodes, with every load current
    averaged over the oversampled samples since the last step. Semi-implicit Euler. Also the
    power tubes' slow thermal state: plate dissipation heats them (4 s), and above half their
    rated dissipation the grids start to emit, pushing the bias up through the grid leaks. */
template <class S>
void PhysicalTubeStage::stepSupply (S& s, const Derived& d) noexcept
{
    auto& su = s.supply;
    auto& v = su.v;
    const double inv = 1.0 / (double) su.count;

    const double ac = mainsRipple ? d.vPeak * std::abs (std::sin (su.phase)) : d.vPeak * 0.94;   // 0.94: same average B+
    su.phase += d.mainsStep;
    su.phase -= su.phase > 2.0 * std::numbers::pi ? 2.0 * std::numbers::pi : 0.0;
    const double head = std::max (ac - v[0], 0.0);
    const double rect = d.tubeRectifier ? d.perveance * head * std::sqrt (head) : head * d.gSource;

    su.choke = d.choke ? su.choke + d.dtOverL12 * (v[0] - v[1] - d.r12 * su.choke) : (v[0] - v[1]) * d.g12;
    const double i23 = (v[1] - v[2]) * d.g23, i34 = (v[2] - v[3]) * d.g34, i45 = (v[3] - v[4]) * d.g45;
    v[0] += d.dtOverC[0] * (rect - su.choke - su.load[0] * inv);
    v[1] += d.dtOverC[1] * (su.choke - i23 - su.load[1] * inv);
    v[2] += d.dtOverC[2] * (i23 - i34 - su.load[2] * inv);
    v[3] += d.dtOverC[3] * (i34 - i45 - su.load[3] * inv);
    v[4] += d.dtOverC[4] * (i45 - su.load[4] * inv);

    s.temperatureA += (su.dissipationA * inv - s.temperatureA) * d.thermalCoef;
    s.temperatureB += (su.dissipationB * inv - s.temperatureB) * d.thermalCoef;
    s.emission[0] = d.emissionPerWatt * std::max (s.temperatureA - d.emissionThreshold, 0.0);
    s.emission[1] = d.emissionPerWatt * std::max (s.temperatureB - d.emissionThreshold, 0.0);

    su.load.fill (0.0);
    su.dissipationA = su.dissipationB = 0.0;
    su.count = 0;
}

template <TubeMath M, int V>
void PhysicalTubeStage::run (float* io, int numSamples) noexcept
{
    auto& s = live<V>();
    const auto& d = derived[(size_t) V];
    for (int n = 0; n < numSamples; ++n)
    {
        //-- Guitar volts, plus the first stage's own noise: thermal noise of the input
        //   resistance and the tube's equivalent noise resistance, and its 1/f flicker.
        double vin = (double) io[n] * d.inputVolts;
        if (noiseOn)
        {
            const double w = s.noise.white();
            auto& pk = s.noise.pink;
            for (int k = 0; k < 3; ++k) pk[(size_t) k] = d.pinkPole[(size_t) k] * pk[(size_t) k] + d.pinkGain[(size_t) k] * w;
            vin += d.noiseSigma * (w + d.pinkScale * (pk[0] + pk[1] + pk[2]));
        }
        const auto& v = s.supply.v;

        //-- Three pipelined blocks: each works on what its predecessor produced last iteration.
        s.pre.template step<M> ({ vin, v[4], v[3] });
        s.feedback += (-s.power.speaker - s.feedback) * d.feedbackCoef;   // speaker phase chosen for negative feedback
        s.pi.template step<M> ({ s.prePipe, v[2], -s.bias, s.feedback, s.emission[0], s.emission[1] });
        const double speaker = powerStage<M> (s, d);
        s.power.speaker = speaker;

        //-- Every current comes out of the supply.
        auto& su = s.supply;
        su.load[0] += s.power.plateA + s.power.plateB;
        su.load[1] += s.power.screenA + s.power.screenB;
        su.load[2] += s.pi.y[0];
        su.load[3] += s.pre.y[2];
        su.load[4] += s.pre.y[1];
        su.dissipationA += s.power.plateA * s.power.vPlateA;
        su.dissipationB += s.power.plateB * s.power.vPlateB;
        if (++su.count >= osFactor)
            stepSupply (s, d);

        s.prePipe = s.pre.y[0];
        s.gridPipe = { s.pi.v[4], s.pi.v[5] };
        io[n] = (float) (speaker / d.fullScaleVolts * d.outputGain);
    }
}

void PhysicalTubeStage::process (float* io, int numSamples) noexcept
{
    glidePots (numSamples);
    const bool exact = mathMode == MathMode::exact;
    switch (voice)
    {
        case 0: exact ? run<ExactMath, 0> (io, numSamples) : run<FastMath, 0> (io, numSamples); break;
        case 1: exact ? run<ExactMath, 1> (io, numSamples) : run<FastMath, 1> (io, numSamples); break;
        default: exact ? run<ExactMath, 2> (io, numSamples) : run<FastMath, 2> (io, numSamples); break;
    }
}

//==============================================================================
void PhysicalTubeStage::glidePots (int numSamples) noexcept
{
    const double c = 1.0 - std::exp (-(double) numSamples / (0.03 * fs));
    double moved = 0.0;
    const auto glide = [&] (double& now, double target, double built)
    {
        now += (target - now) * c;
        if (std::abs (target - now) < 1.0e-6) now = target;
        moved = std::max (moved, std::abs (now - built));
    };
    glide (potNow.gain, potTarget.gain, potBuilt.gain);
    glide (potNow.bass, potTarget.bass, potBuilt.bass);
    glide (potNow.mid, potTarget.mid, potBuilt.mid);
    glide (potNow.treble, potTarget.treble, potBuilt.treble);
    glide (potNow.presence, potTarget.presence, potBuilt.presence);
    samplesSinceRebuild += numSamples;
    if (moved > 1.0e-5 && samplesSinceRebuild >= 128)
    {
        applyPots (voice, true);
        potBuilt = potNow;
        samplesSinceRebuild = 0;
    }
}

void PhysicalTubeStage::applyPots (int v, bool rebase) noexcept
{
    auto& h = nets[(size_t) v];
    const auto pv = potValues (v, potNow);
    h.pre.setResistance (h.gainTop, pv.gainTop);
    h.pre.setResistance (h.gainBottom, pv.gainBottom);
    h.pre.setResistance (h.trebleTop, pv.trebleTop);
    h.pre.setResistance (h.trebleBottom, pv.trebleBottom);
    h.pre.setResistance (h.bass, pv.bass);
    h.pre.setResistance (h.mid, pv.mid);
    h.pi.setResistance (h.presence, pv.presence + 10.0);
    const auto rebuild = [&] (auto& s)
    {
        s.pre.sys.build (h.pre, fs, workspace);
        s.pi.sys.build (h.pi, fs, workspace);
        if (rebase)
        {
            s.pre.rebased();
            s.pi.rebased();
        }
    };
    switch (v)
    {
        case 0: rebuild (clean); break;
        case 1: rebuild (classic); break;
        default: rebuild (modern); break;
    }
}

template <int V>
void PhysicalTubeStage::initialiseVoice()
{
    const auto& sp = specs()[(size_t) V];
    auto& d = derived[(size_t) V];
    auto& h = nets[(size_t) V];
    auto& s = live<V>();
    s = {};

    //-- Constants
    d.inputVolts = sp.inputVolts;
    d.outputGain = std::pow (10.0, sp.outputDb / 20.0);
    d.turns = std::sqrt (sp.power.raa / (4.0 * 8.0));
    d.fullScaleVolts = std::sqrt (2.0 * sp.power.ratedWatts * 8.0);
    d.dt = 1.0 / fs;
    d.dtBase = (double) osFactor / fs;
    d.feedbackCoef = 1.0 - std::exp (-2.0 * std::numbers::pi * sp.pi.feedbackHz / fs);
    d.lMagnetising = sp.power.lPrimary / (4.0 * d.turns * d.turns);
    d.fluxSaturation = d.fullScaleVolts / (2.0 * std::numbers::pi * sp.power.saturationHz);
    d.hysteronWidth = 0.02 * d.fluxSaturation;
    d.coerciveCurrent = 0.01 * d.fullScaleVolts / 8.0;
    d.thermalCoef = 1.0 - std::exp (-d.dtBase / 4.0);
    d.emissionThreshold = 0.5 * sp.power.plateDissipation;
    d.emissionPerWatt = 2.0e-6 / d.emissionThreshold;   // 2 uA at full rated dissipation
    d.tubeA = sp.power.tube;
    d.tubeB = sp.power.tube;
    d.tubeB.kg1 *= sp.power.mismatch;
    {
        const auto& su = sp.supply;
        d.dtOverC = { d.dtBase / su.c1, d.dtBase / su.c2, d.dtBase / su.c3, d.dtBase / su.c4, d.dtBase / su.c5 };
        d.gSource = su.rSource > 0.0 ? 1.0 / su.rSource : 0.0;
        d.dtOverL12 = su.l12 > 0.0 ? d.dtBase / su.l12 : 0.0;
        d.g12 = 1.0 / su.r12; d.g23 = 1.0 / su.r23; d.g34 = 1.0 / su.r34; d.g45 = 1.0 / su.r45;
        d.mainsStep = 2.0 * std::numbers::pi * 50.0 * d.dtBase;
        d.vPeak = su.vPeak; d.perveance = su.perveance; d.r12 = su.r12; d.tubeRectifier = su.tubeRectifier; d.choke = su.l12 > 0.0;
        d.rscreen = sp.power.rscreen;
    }
    {
        // White: 4kTR over the oversampled band (input resistance + the 12AX7's ~1.6k equivalent
        // noise resistance). Flicker: three one-pole sections (Kellet's pinking filter, re-poled
        // for this rate), scaled so the 1/f part equals the white floor at 2 kHz.
        d.noiseSigma = std::sqrt (4.0 * 1.380649e-23 * 300.0 * (34.0e3 + 1.6e3) * fs * 0.5);
        const double f[3] { 16.5, 260.0, 3000.0 }, dcGain[3] { 42.1, 8.0, 2.45 };
        double re = 0.0, im = 0.0;
        const double w = 2.0 * std::numbers::pi * 2000.0 / fs;
        for (int k = 0; k < 3; ++k)
        {
            const double a = std::exp (-2.0 * std::numbers::pi * f[k] / fs);
            d.pinkPole[(size_t) k] = a;
            d.pinkGain[(size_t) k] = (1.0 - a) * dcGain[k];
            const double dr = 1.0 - a * std::cos (w), di = a * std::sin (w), mag = dr * dr + di * di;
            re += d.pinkGain[(size_t) k] * dr / mag;
            im -= d.pinkGain[(size_t) k] * di / mag;
        }
        d.pinkScale = 1.0 / std::sqrt (re * re + im * im);
    }

    //-- Circuits
    Handles hp {}, hi {};
    h.pre = buildPreamp (V, potNow, hp);
    h.pi = buildPi (V, potNow, hi);
    h.gainTop = hp[0]; h.gainBottom = hp[1]; h.trebleTop = hp[2]; h.trebleBottom = hp[3]; h.bass = hp[4]; h.mid = hp[5];
    h.presence = hi[6];
    s.pre.sys.build (h.pre, fs, workspace);
    s.pi.sys.build (h.pi, fs, workspace);
    s.power.load.build (buildLoad (sp.power, d.turns), fs, workspace);
    potBuilt = potNow;

    //-- DC operating point: preamp and phase-inverter currents set the supply drops, which set
    //   the currents; iterate. The fixed bias is calibrated each round, as a tech would.
    auto& v = s.supply.v;
    v.fill (sp.supply.vPeak * 0.92);
    double screen = 0.0;
    std::array<double, numSupplyNodes> load {};
    const auto settleDc = [&]
    {
        s.pre.solveDc (h.pre, { 0.0, v[4], v[3] });
        s.bias = calibrateBias (d.tubeA, v[0], v[1], sp.power.rscreen, sp.power.idleCurrent, screen);
        s.pi.solveDc (h.pi, { s.pre.y[0], v[2], -s.bias, 0.0, 0.0, 0.0 });
        load = { 2.0 * sp.power.idleCurrent, 2.0 * screen, s.pi.y[0], s.pre.y[2], s.pre.y[1] };
    };
    for (int round = 0; round < 30; ++round)
    {
        settleDc();
        const double i45 = load[4], i34 = load[3] + i45, i23 = load[2] + i34, i12 = load[1] + i23, rect = load[0] + i12;
        // Capacitor-input rectifier, static estimate; the warm-up below finds the real ripple.
        v[0] = sp.supply.tubeRectifier ? sp.supply.vPeak - 1.5 * std::pow (rect / sp.supply.perveance, 2.0 / 3.0)
                                       : sp.supply.vPeak - 4.0 * sp.supply.rSource * rect;
        v[1] = v[0] - sp.supply.r12 * i12;
        v[2] = v[1] - sp.supply.r23 * i23;
        v[3] = v[2] - sp.supply.r34 * i34;
        v[4] = v[3] - sp.supply.r45 * i45;
        s.supply.choke = i12;
    }

    //-- Supply alone, with the idle loads, until its ripple is periodic (2 s); then the DC point
    //   again at the average supply, bias recalibrated.
    {
        std::array<double, numSupplyNodes> sum {};
        const int steps = (int) (2.0 / d.dtBase), tail = (int) (0.1 / d.dtBase);
        for (int k = 0; k < steps; ++k)
        {
            s.supply.load = load;
            s.supply.count = 1;
            stepSupply (s, d);
            if (k >= steps - tail)
                for (int j = 0; j < numSupplyNodes; ++j) sum[(size_t) j] += v[(size_t) j];
        }
        const auto rippled = v;
        for (int j = 0; j < numSupplyNodes; ++j) v[(size_t) j] = sum[(size_t) j] / tail;
        settleDc();
        v = rippled;
    }

    //-- Power stage at idle
    {
        auto& pw = s.power;
        const auto dA = d.tubeA.drive<ExactMath> (-s.bias, v[1] - sp.power.rscreen * screen);
        const auto dB = d.tubeB.drive<ExactMath> (-s.bias, v[1] - sp.power.rscreen * screen);
        const double at = std::atan (v[0] / d.tubeA.kvb);
        pw.kAPrev = dA.plateScale;
        pw.kBPrev = dB.plateScale;
        pw.atanA = pw.atanB = at;
        pw.plateA = dA.plateScale * at;
        pw.plateB = dB.plateScale * at;
        pw.screenA = d.tubeA.screenCurrent (dA, at);
        pw.screenB = d.tubeB.screenCurrent (dB, at);
        pw.vPlateA = pw.vPlateB = v[0];
        const double dI = dA.plateScale * d.tubeA.kvb / (d.tubeA.kvb * d.tubeA.kvb + v[0] * v[0]);
        pw.slopePrev = -2.0 * d.turns * d.turns * pw.load.E[0] * dI - 1.0;
        s.temperatureA = pw.plateA * v[0];
        s.temperatureB = pw.plateB * v[0];
    }
    s.prePipe = s.pre.y[0];
    s.gridPipe = { s.pi.v[4], s.pi.v[5] };

    //-- Then the whole amp, silent, until coupling caps and supply have settled together.
    {
        const bool noise = noiseOn;
        noiseOn = false;
        std::vector<float> zeros (256, 0.0f);
        for (int k = 0; k < (int) (0.25 * fs); k += 256)
        {
            std::fill (zeros.begin(), zeros.end(), 0.0f);
            run<ExactMath, V> (zeros.data(), 256);
        }
        noiseOn = noise;
    }
    s.pre.stats = {};
    s.pi.stats = {};
    s.power.stats = {};
    settled<V>() = s;
}

void PhysicalTubeStage::prepare (double oversampledRate, int oversamplingFactor)
{
    fs = oversampledRate;
    osFactor = std::max (1, oversamplingFactor);
    potNow = potTarget;
    initialiseVoice<0>();
    initialiseVoice<1>();
    initialiseVoice<2>();
    reset();
}

void PhysicalTubeStage::reset() noexcept
{
    switch (voice)
    {
        case 0: clean = cleanSettled; break;
        case 1: classic = classicSettled; break;
        default: modern = modernSettled; break;
    }
    applyPots (voice, true);
    potBuilt = potNow;
    samplesSinceRebuild = 0;
}

void PhysicalTubeStage::setVoice (int voiceIndex) noexcept
{
    voiceIndex = std::clamp (voiceIndex, 0, numVoices - 1);
    if (voiceIndex == voice)
        return;
    voice = voiceIndex;
    reset();
}

template <int V>
PhysicalTubeStage::Telemetry PhysicalTubeStage::telemetryOf (const VoiceState<Preamp<V>>& s) const noexcept
{
    Telemetry t;
    t.supply = s.supply.v;
    t.bias = s.bias;
    t.flux = s.power.flux;
    t.fluxSaturation = derived[(size_t) V].fluxSaturation;
    t.temperatureA = s.temperatureA;
    t.emissionA = s.emission[0];
    t.triodes = PreampSize<V>::nt;
    for (int k = 0; k < 2 * PreampSize<V>::nt; ++k) t.preampPorts[(size_t) k] = s.pre.v[(size_t) k];
    t.preamp = s.pre.stats;
    t.phaseInverter = s.pi.stats;
    t.plates = s.power.stats;
    return t;
}

PhysicalTubeStage::Telemetry PhysicalTubeStage::telemetry() const noexcept
{
    switch (voice)
    {
        case 0: return telemetryOf<0> (clean);
        case 1: return telemetryOf<1> (classic);
        default: return telemetryOf<2> (modern);
    }
}

PhysicalTubeStage::Telemetry PhysicalTubeStage::idleTelemetry (int v) const noexcept
{
    switch (v)
    {
        case 0: return telemetryOf<0> (cleanSettled);
        case 1: return telemetryOf<1> (classicSettled);
        default: return telemetryOf<2> (modernSettled);
    }
}

void PhysicalTubeStage::clearStats() noexcept
{
    const auto clear = [] (auto& s) { s.pre.stats = {}; s.pi.stats = {}; s.power.stats = {}; };
    switch (voice)
    {
        case 0: clear (clean); break;
        case 1: clear (classic); break;
        default: clear (modern); break;
    }
}

//==============================================================================
// Offline-check hooks
//==============================================================================

Netlist PhysicalTubeStage::preampNetlist (int v, const Pots& p) { Handles h {}; return buildPreamp (v, p, h); }
Netlist PhysicalTubeStage::phaseInverterNetlist (int v, const Pots& p) { Handles h {}; return buildPi (v, p, h); }
double PhysicalTubeStage::inputVolts (int v) { return specs()[(size_t) v].inputVolts; }

namespace
{
template <class Solver, class Inputs>
std::vector<double> runCircuit (Solver& solver, const Netlist& net, const std::vector<double>& volts, double fs,
                                Inputs u, int output, MathMode mode)
{
    Workspace w;
    solver.sys.build (net, fs, w);
    solver.solveDc (net, u);
    std::vector<double> out;
    out.reserve (volts.size());
    for (double x : volts)
    {
        u[0] = x;
        if (mode == MathMode::exact) solver.template step<ExactMath> (u); else solver.template step<FastMath> (u);
        out.push_back (solver.y[(size_t) output]);
    }
    return out;
}

template <int V>
std::vector<double> runPreampVoice (const Pots& p, const std::vector<double>& volts, double fs, double b1, double b2, MathMode mode)
{
    auto solver = std::make_unique<Preamp<V>>();
    Handles h {};
    return runCircuit (*solver, buildPreamp (V, p, h), volts, fs, std::array<double, 3> { 0.0, b1, b2 }, 0, mode);
}
} // namespace

std::vector<double> PhysicalTubeStage::runPreamp (int v, const Pots& p, const std::vector<double>& volts, double fs,
                                                  double b1, double b2, MathMode mode)
{
    switch (v)
    {
        case 0: return runPreampVoice<0> (p, volts, fs, b1, b2, mode);
        case 1: return runPreampVoice<1> (p, volts, fs, b1, b2, mode);
        default: return runPreampVoice<2> (p, volts, fs, b1, b2, mode);
    }
}

std::vector<double> PhysicalTubeStage::runPhaseInverter (int v, const Pots& p, const std::vector<double>& volts, double fs,
                                                         double bplus, double bias, MathMode mode)
{
    auto solver = std::make_unique<PhaseInverter>();
    Handles h {};
    return runCircuit (*solver, buildPi (v, p, h), volts, fs, std::array<double, 6> { 0.0, bplus, -bias, 0.0, 0.0, 0.0 }, 1, mode);
}
} // namespace axiom::physical
