#include "PhysicalTubeStage.h"
#include <cassert>

namespace axiom::physical
{
namespace
{
/** Small dense row-major matrix for prepare-time algebra (never used on the audio thread). */
struct Mat
{
    int rows = 0, cols = 0;
    std::vector<double> a;
    Mat (int r, int c) : rows (r), cols (c), a ((size_t) (r * c), 0.0) {}
    double& operator() (int r, int c) { return a[(size_t) (r * cols + c)]; }
    double operator() (int r, int c) const { return a[(size_t) (r * cols + c)]; }
};

Mat operator* (const Mat& x, const Mat& y)
{
    assert (x.cols == y.rows);
    Mat out (x.rows, y.cols);
    for (int r = 0; r < x.rows; ++r)
        for (int k = 0; k < x.cols; ++k)
        {
            const double v = x (r, k);
            if (v != 0.0)
                for (int c = 0; c < y.cols; ++c)
                    out (r, c) += v * y (k, c);
        }
    return out;
}

Mat operator+ (Mat x, const Mat& y)
{
    for (size_t i = 0; i < x.a.size(); ++i) x.a[i] += y.a[i];
    return x;
}

/** Gauss-Jordan inversion with partial pivoting. MNA matrices mix conductances of 1e-6 S
    (picofarads at 352 kHz) with unit voltage-source stamps, so pivoting matters. */
bool invert (Mat& m)
{
    const int n = m.rows;
    Mat inv (n, n);
    for (int i = 0; i < n; ++i) inv (i, i) = 1.0;
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs (m (r, col)) > std::abs (m (pivot, col))) pivot = r;
        if (std::abs (m (pivot, col)) < 1.0e-300)
            return false;
        if (pivot != col)
            for (int c = 0; c < n; ++c)
            {
                std::swap (m (pivot, c), m (col, c));
                std::swap (inv (pivot, c), inv (col, c));
            }
        const double scale = 1.0 / m (col, col);
        for (int c = 0; c < n; ++c) { m (col, c) *= scale; inv (col, c) *= scale; }
        for (int r = 0; r < n; ++r)
        {
            if (r == col) continue;
            const double f = m (r, col);
            if (f == 0.0) continue;
            for (int c = 0; c < n; ++c) { m (r, c) -= f * m (col, c); inv (r, c) -= f * inv (col, c); }
        }
    }
    m = inv;
    return true;
}
} // namespace

DkMatrices Netlist::discretise (double sampleRate, bool withDcPoint) const
{
    int numVoltageSources = 0;
    for (const auto& in : inputs) numVoltageSources += in.voltage ? 1 : 0;
    const int nz = nodes + numVoltageSources;
    const int nx = (int) reactives.size(), nu = (int) inputs.size(), nn = (int) ports.size(), ny = (int) outputs.size();

    //-- MNA system matrix S: node conductances plus voltage-source rows/columns.
    Mat S (nz, nz);
    const auto stamp = [&S] (int a, int b, double g)
    {
        if (a >= 0) S (a, a) += g;
        if (b >= 0) S (b, b) += g;
        if (a >= 0 && b >= 0) { S (a, b) -= g; S (b, a) -= g; }
    };
    for (const auto& r : resistors) stamp (r.a, r.b, r.g);

    // Trapezoidal companion models.
    //   capacitor: i = (2C/T) v - x,   x' = 2 (2C/T) v - x
    //   inductor:  i = (T/2L) v - x,   x' = x - 2 (T/2L) v      (x is minus the usual history term)
    // so both inject +x into node a, and x' = rho (2 g v - x) with rho = +1 (C) or -1 (L).
    std::vector<double> g ((size_t) nx), rho ((size_t) nx);
    for (int j = 0; j < nx; ++j)
    {
        const auto& e = reactives[(size_t) j];
        g[(size_t) j] = e.capacitor ? 2.0 * e.value * sampleRate : 1.0 / (2.0 * e.value * sampleRate);
        rho[(size_t) j] = e.capacitor ? 1.0 : -1.0;
        stamp (e.a, e.b, g[(size_t) j]);
    }
    {
        int k = 0;
        for (const auto& in : inputs)
        {
            if (! in.voltage) continue;
            const int row = nodes + k++;
            if (in.a >= 0) { S (in.a, row) += 1.0; S (row, in.a) += 1.0; }
            if (in.b >= 0) { S (in.b, row) -= 1.0; S (row, in.b) -= 1.0; }
        }
    }

    //-- Right-hand-side incidence: states, inputs and nonlinear currents.
    Mat Tx (nz, nx), Tu (nz, nu), Tn (nz, nn);
    for (int j = 0; j < nx; ++j)
    {
        const auto& e = reactives[(size_t) j];
        if (e.a >= 0) Tx (e.a, j) += 1.0;
        if (e.b >= 0) Tx (e.b, j) -= 1.0;
    }
    {
        int k = 0;
        for (int q = 0; q < nu; ++q)
        {
            const auto& in = inputs[(size_t) q];
            if (in.voltage)
                Tu (nodes + k++, q) = 1.0;
            else
            {
                if (in.a >= 0) Tu (in.a, q) += 1.0;
                if (in.b >= 0) Tu (in.b, q) -= 1.0;
            }
        }
    }
    for (int j = 0; j < nn; ++j)
    {
        const auto& p = ports[(size_t) j];
        if (p.a >= 0) Tn (p.a, j) -= 1.0;   // device current leaves node a ...
        if (p.b >= 0) Tn (p.b, j) += 1.0;   // ... and enters node b
    }

    //-- Voltage pick-off rows.
    const auto selector = [nz] (const auto& list)
    {
        Mat P ((int) list.size(), nz);
        for (int r = 0; r < (int) list.size(); ++r)
        {
            if (list[(size_t) r].a >= 0) P (r, list[(size_t) r].a) += 1.0;
            if (list[(size_t) r].b >= 0) P (r, list[(size_t) r].b) -= 1.0;
        }
        return P;
    };
    const Mat Pr = selector (reactives), Pn = selector (ports), Po = selector (outputs);

    Mat Sinv = S;
    const bool ok = invert (Sinv);
    assert (ok);
    (void) ok;
    const Mat SiTx = Sinv * Tx, SiTu = Sinv * Tu, SiTn = Sinv * Tn;

    DkMatrices m;
    m.nx = nx; m.nu = nu; m.nn = nn; m.ny = ny;
    const Mat G = Pn * SiTx, H = Pn * SiTu, K = Pn * SiTn;
    const Mat D = Po * SiTx, E = Po * SiTu, F = Po * SiTn;
    const Mat Vx = Pr * SiTx, Vu = Pr * SiTu, Vn = Pr * SiTn;
    Mat A (nx, nx), B (nx, nu), C (nx, nn);
    for (int r = 0; r < nx; ++r)
    {
        const double s = rho[(size_t) r] * 2.0 * g[(size_t) r];
        for (int c = 0; c < nx; ++c) A (r, c) = s * Vx (r, c) - (r == c ? rho[(size_t) r] : 0.0);
        for (int c = 0; c < nu; ++c) B (r, c) = s * Vu (r, c);
        for (int c = 0; c < nn; ++c) C (r, c) = s * Vn (r, c);
    }
    m.A = A.a; m.B = B.a; m.C = C.a; m.D = D.a; m.E = E.a; m.F = F.a; m.G = G.a; m.H = H.a; m.K = K.a;

    //-- DC operating point of the discrete system: x = A x + B u + C i  =>  x = (I-A)^-1 (B u + C i).
    if (withDcPoint)
    {
        Mat M (nx, nx);
        for (int r = 0; r < nx; ++r)
            for (int c = 0; c < nx; ++c)
                M (r, c) = (r == c ? 1.0 : 0.0) - A (r, c);
        if (invert (M))
        {
            const Mat Xu = M * B, Xi = M * C;
            m.Xu = Xu.a; m.Xi = Xi.a;
            m.Hdc = (G * Xu + H).a;
            m.Kdc = (G * Xi + K).a;
            m.hasDc = true;
        }
    }
    return m;
}

//==============================================================================
const std::array<VoiceCircuit, 3>& voiceCircuits()
{
    // Koren pentode parameters; the grid terms are a Child-law fit for driven-positive grids.
    static constexpr PentodeModel el34 { .mu = 11.0, .ex = 1.35, .kg1 = 650.0, .kg2 = 4200.0, .kp = 60.0, .kvb = 24.0,
                                         .grid = { 2.0e-3, 0.2 } };
    static constexpr PentodeModel sixL6 { .mu = 8.7, .ex = 1.35, .kg1 = 1460.0, .kg2 = 4500.0, .kp = 48.0, .kvb = 12.0,
                                          .grid = { 2.0e-3, 0.2 } };

    static std::array<VoiceCircuit, 3> circuits {{   // not const: the offline tuning tools adjust it
        // Clean: two fully bypassed stages (blackface-style), 6L6 pair, stiff solid-state supply
        // with a choke. Lots of headroom; it breaks up late and evenly.
        {
            .numStages = 2,
            .stages = {{
                { .rk = 1.5e3, .ck = 25.0e-6, .cin = 0.1e-6, .rs = 34.0e3, .supplyNode = 4 },
                { .rk = 1.5e3, .ck = 25.0e-6, .cin = 22.0e-9, .rs = 47.0e3, .cp = 250.0e-12, .supplyNode = 3 },  // tone-stack treble cap
                {},
            }},
            .gainLoDb = -40.0, .gainHiDb = -6.0,
            .toneScale = 0.01, .powerDrive = 0.9, .outputDb = -1.0,
            .power = { .tube = sixL6, .raa = 4.2e3, .idleCurrent = 0.035, .rscreen = 470.0, .nfb = 0.4 },
            .supply = { .tubeRectifier = false, .vNoLoad = 465.0, .rSource = 35.0, .c1 = 70.0e-6, .l12 = 4.0, .r12 = 100.0,
                        .c2 = 20.0e-6, .r23 = 4.7e3, .c3 = 20.0e-6, .r34 = 10.0e3, .c4 = 20.0e-6, .r45 = 10.0e3, .c5 = 20.0e-6 },
        },
        // Classic: plexi-style. A bright, partially bypassed first stage, a second stage with a
        // small bypass cap, EL34 pair and a GZ34 tube rectifier: the supply sags and blooms.
        {
            .numStages = 2,
            .stages = {{
                { .rk = 2.7e3, .ck = 0.68e-6, .cin = 0.1e-6, .rs = 34.0e3, .supplyNode = 4 },
                { .rk = 820.0, .ck = 2.2e-6, .cin = 22.0e-9, .rs = 47.0e3, .supplyNode = 3 },
                {},
            }},
            .gainLoDb = -24.0, .gainHiDb = -4.0,
            .toneScale = 0.01, .powerDrive = 1.0, .outputDb = -12.0,
            .power = { .tube = el34, .raa = 3.4e3, .idleCurrent = 0.040, .rscreen = 1.0e3, .nfb = 0.35 },
            .supply = { .tubeRectifier = true, .vNoLoad = 500.0, .perveance = 1.0e-3, .c1 = 32.0e-6, .l12 = 0.0, .r12 = 1.0e3,
                        .c2 = 32.0e-6, .r23 = 10.0e3, .c3 = 32.0e-6, .r34 = 10.0e3, .c4 = 32.0e-6, .r45 = 10.0e3, .c5 = 32.0e-6 },
        },
        // Modern: hot-rodded. Three stages, the middle one a cold clipper (10 k cathode, so it
        // idles near cutoff and clips one side first), small coupling caps to keep the low end
        // tight, treble-bleed caps across the later plate resistors to keep three cascaded
        // stages from fizzing, and an EL34 pair on a stiff solid-state supply with a choke.
        {
            .numStages = 3,
            .stages = {{
                { .rk = 2.7e3, .ck = 0.68e-6, .cin = 0.1e-6, .rs = 34.0e3, .supplyNode = 4 },
                { .rk = 10.0e3, .ck = 0.68e-6, .cin = 4.7e-9, .rs = 47.0e3, .cp = 680.0e-12, .supplyNode = 3 },
                { .rk = 1.5e3, .ck = 1.0e-6, .cin = 10.0e-9, .rs = 47.0e3, .cp = 680.0e-12, .supplyNode = 3 },
            }},
            .gainLoDb = -48.0, .gainHiDb = -12.0,
            .toneScale = 0.01, .powerDrive = 0.9, .outputDb = -19.0,
            .power = { .tube = el34, .raa = 3.4e3, .idleCurrent = 0.040, .rscreen = 1.0e3, .nfb = 0.3 },
            .supply = { .tubeRectifier = false, .vNoLoad = 480.0, .rSource = 25.0, .c1 = 100.0e-6, .l12 = 5.0, .r12 = 60.0,
                        .c2 = 50.0e-6, .r23 = 10.0e3, .c3 = 32.0e-6, .r34 = 10.0e3, .c4 = 32.0e-6, .r45 = 10.0e3, .c5 = 32.0e-6 },
        },
    }};
    return circuits;
}

//==============================================================================
namespace
{
DkMatrices buildTriode (const StageSpec& s, double fs)
{
    enum { in, a, gr, k, p, b, count };
    Netlist net (count);
    constexpr int gnd = Netlist::gnd;
    net.voltageInput (in, gnd);           // u0: signal from the previous stage
    net.voltageInput (b, gnd);            // u1: B+ from the supply node
    net.capacitor (in, a, s.cin);         // x0
    net.resistor (a, gnd, s.rleak);
    net.resistor (a, gr, s.rs);
    net.capacitor (gr, k, s.cgk);         // x1
    net.capacitor (gr, p, s.cgp);         // x2 (Miller)
    net.resistor (k, gnd, s.rk);
    net.capacitor (k, gnd, s.ck);         // x3 (0 F = unbypassed: zero conductance, state stays 0)
    net.resistor (b, p, s.rp);
    net.capacitor (p, gnd, s.cp);         // x4
    net.resistor (p, gnd, s.rload);
    net.nonlinearPort (gr, k);            // i0: grid current, controlled by Vgk
    net.nonlinearPort (p, k);             // i1: plate current, controlled by Vpk
    net.output (p, gnd);                  // y0: plate voltage
    return net.discretise (fs, true);
}

DkMatrices buildGrid (const PowerSpec& s, double fs)
{
    enum { drive, d, a, gr, bias, count };
    Netlist net (count);
    constexpr int gnd = Netlist::gnd;
    net.voltageInput (drive, gnd);        // u0: phase-inverter output
    net.voltageInput (bias, gnd);         // u1: negative bias supply
    net.resistor (drive, d, s.rpi);
    net.capacitor (d, a, s.cc);           // x0: coupling cap (charges on grid current)
    net.resistor (a, bias, s.rgl);
    net.resistor (a, gr, s.rstop);
    net.capacitor (gr, gnd, s.cgrid);     // x1: grid input capacitance
    net.nonlinearPort (gr, gnd);          // i0: grid current (cathode at ground, fixed bias)
    return net.discretise (fs, true);
}

DkMatrices buildLoad (const PowerSpec& s, double fs, double turns)
{
    enum { sec, n1, n2, n3, n4, count };
    Netlist net (count);
    constexpr int gnd = Netlist::gnd;
    const double ratio2 = 4.0 * turns * turns;   // full primary : secondary impedance ratio
    const double w0 = 2.0 * std::numbers::pi * s.resonanceHz;
    const double les = s.res / (s.resonanceQ * w0), ces = 1.0 / (w0 * w0 * les);
    net.currentInput (sec, gnd);                     // u0: reflected plate current
    net.inductor (sec, gnd, s.lPrimary / ratio2);    // x0: magnetising inductance
    net.capacitor (sec, gnd, s.cPrimary * ratio2);   // x1: winding capacitance
    net.resistor (sec, gnd, s.rCorePrimary / ratio2);  //     core (eddy + hysteresis) loss
    net.resistor (sec, n1, s.rWinding);
    net.inductor (n1, n2, s.lLeakPrimary / ratio2);  // x2: leakage inductance
    net.resistor (n2, n3, s.re);
    net.inductor (n3, n4, s.le);                     // x3: voice-coil inductance
    net.resistor (n3, n4, s.r2);                     //     ... with eddy-current loss
    net.resistor (n4, gnd, s.res);
    net.inductor (n4, gnd, les);                     // x4: motional compliance
    net.capacitor (n4, gnd, ces);                    // x5: motional mass
    net.output (sec, gnd);                           // y0: ideal-transformer voltage (reflects to the plates)
    net.output (n2, gnd);                            // y1: speaker terminals
    return net.discretise (fs, false);
}

/** Static solve of v = Hdc u + Kdc i(v) for a triode stage (prepare-time; exact math, damped). */
std::array<double, 2> triodeDc (const TriodeStage& st, const std::array<double, 2>& u)
{
    const auto& s = st.sys;
    double vgk = -1.0, vpk = 0.5 * u[1];
    for (int it = 0; it < 500; ++it)
    {
        const auto t = st.tube.evaluate<ExactMath> (vgk, vpk);
        const double f0 = s.Hdc[0] * u[0] + s.Hdc[1] * u[1] + s.Kdc[0] * t.ig + s.Kdc[1] * t.ip - vgk;
        const double f1 = s.Hdc[2] * u[0] + s.Hdc[3] * u[1] + s.Kdc[2] * t.ig + s.Kdc[3] * t.ip - vpk;
        if (std::max (std::abs (f0), std::abs (f1)) < 1.0e-11)
            break;
        const double j00 = s.Kdc[0] * t.dIgdVgk + s.Kdc[1] * t.dIpdVgk - 1.0;
        const double j01 = s.Kdc[1] * t.dIpdVpk;
        const double j10 = s.Kdc[2] * t.dIgdVgk + s.Kdc[3] * t.dIpdVgk;
        const double j11 = s.Kdc[3] * t.dIpdVpk - 1.0;
        const double invDet = 1.0 / (j00 * j11 - j01 * j10);
        vgk += std::clamp ((j01 * f1 - j11 * f0) * invDet, -0.5, 0.5);
        vpk += std::clamp ((j10 * f0 - j00 * f1) * invDet, -20.0, 20.0);
    }
    return { vgk, vpk };
}
} // namespace

void PhysicalTubeStage::initialiseVoice (int index)
{
    const auto& c = voiceCircuits()[(size_t) index];
    auto& s = initial[(size_t) index];
    auto& d = derived[(size_t) index];
    s = State {};

    d.turns = std::sqrt (c.power.raa / (4.0 * c.power.speakerOhms));
    d.fullScaleVolts = std::sqrt (2.0 * c.power.ratedWatts * c.power.speakerOhms);
    d.dt = 1.0 / fs;
    d.piCoef = 1.0 - std::exp (-2.0 * std::numbers::pi * c.power.piBandwidthHz / fs);
    d.couplingCoef = 1.0 / (1.0 + 2.0 * std::numbers::pi * 12.0 / fs);
    d.nfbCoef = 1.0 - std::exp (-2.0 * std::numbers::pi * c.power.nfbCompensationHz / fs);
    d.outputGain = std::pow (10.0, c.outputDb / 20.0);
    {
        const auto& sp = c.supply;
        auto& k = d.supply;
        k.dtOverC = { d.dt / sp.c1, d.dt / sp.c2, d.dt / sp.c3, d.dt / sp.c4, d.dt / sp.c5 };
        k.gSource = 1.0 / sp.rSource;
        k.dtOverL12 = sp.l12 > 0.0 ? d.dt / sp.l12 : 0.0;
        k.g12 = 1.0 / sp.r12; k.g23 = 1.0 / sp.r23; k.g34 = 1.0 / sp.r34; k.g45 = 1.0 / sp.r45;
    }

    for (int st = 0; st < c.numStages; ++st)
    {
        auto& stage = s.stages[(size_t) st];
        stage.rp = c.stages[(size_t) st].rp;
        stage.sys.assign (buildTriode (c.stages[(size_t) st], fs));
    }
    s.gridA.sys.assign (buildGrid (c.power, fs));
    s.gridB.sys = s.gridA.sys;
    s.load.sys.assign (buildLoad (c.power, fs, d.turns));

    //-- DC operating point. The preamp currents set the supply drops, which set the preamp
    //   currents, so iterate the two to a fixed point. The power tubes' fixed bias is
    //   calibrated each round, the way a tech sets idle current on a real amp.
    const auto& sp = c.supply;
    const auto& tube = c.power.tube;
    std::array<double, numSupplyNodes> v {};
    v.fill (sp.vNoLoad * 0.92);
    double bias = 30.0, screen = 0.0, i12 = 0.0;
    std::array<std::array<double, 2>, 3> op {};
    for (int round = 0; round < 60; ++round)
    {
        std::array<double, 3> stageCurrent {};
        for (int st = 0; st < c.numStages; ++st)
        {
            auto& stage = s.stages[(size_t) st];
            const std::array<double, 2> u { 0.0, v[(size_t) c.stages[(size_t) st].supplyNode] };
            op[(size_t) st] = triodeDc (stage, u);
            const auto t = stage.tube.evaluate<ExactMath> (op[(size_t) st][0], op[(size_t) st][1]);
            const std::array<double, 2> i { t.ig, t.ip };
            stage.x = stage.sys.dcState (u, i);
            stage.pPrev = stage.sys.linearPart (stage.x, u);
            stage.vPrev = op[(size_t) st];
            {
                const auto& K = stage.sys.K;
                const double j00 = K[0] * t.dIgdVgk + K[1] * t.dIpdVgk - 1.0, j01 = K[1] * t.dIpdVpk;
                const double j10 = K[2] * t.dIgdVgk + K[3] * t.dIpdVgk, j11 = K[3] * t.dIpdVpk - 1.0;
                const double invDet = -1.0 / (j00 * j11 - j01 * j10);
                stage.tangent = { j11 * invDet, -j01 * invDet, -j10 * invDet, j00 * invDet };
            }
            const double vp = stage.sys.output (0, stage.x, u, i);
            stage.supplyCurrent = (u[1] - vp) / stage.rp;
            stageCurrent[(size_t) st] = stage.supplyCurrent;
            d.idlePlate[(size_t) st] = vp;
        }

        // Bias: bisection on the grid voltage for the target idle plate current.
        const auto idleAt = [&] (double vbias, double& screenOut)
        {
            double ig2 = 0.0, vg2 = v[1];
            for (int k = 0; k < 20; ++k)
            {
                vg2 = v[1] - c.power.rscreen * ig2;
                ig2 = tube.drive<ExactMath> (-vbias, vg2).screen;
            }
            screenOut = ig2;
            return tube.drive<ExactMath> (-vbias, vg2).plateScale * std::atan (v[0] / tube.kvb);
        };
        double lo = 0.0, hi = 250.0;
        for (int k = 0; k < 80; ++k)
        {
            const double mid = 0.5 * (lo + hi);
            double scr = 0.0;
            (idleAt (mid, scr) > c.power.idleCurrent ? lo : hi) = mid;
        }
        bias = 0.5 * (lo + hi);
        idleAt (bias, screen);

        double late = 0.0;
        for (int st = 1; st < c.numStages; ++st) late += stageCurrent[(size_t) st];
        const double i45 = stageCurrent[0];
        const double i34 = late + i45;
        const double i23 = c.power.piCurrent + i34;
        i12 = 2.0 * screen + i23;
        const double rect = 2.0 * c.power.idleCurrent + i12;
        v[0] = sp.tubeRectifier ? sp.vNoLoad - std::pow (rect / sp.perveance, 2.0 / 3.0) : sp.vNoLoad - sp.rSource * rect;
        v[1] = v[0] - sp.r12 * i12;
        v[2] = v[1] - sp.r23 * i23;
        v[3] = v[2] - sp.r34 * i34;
        v[4] = v[3] - sp.r45 * i45;
    }

    s.supply.v = v;
    s.supply.chokeCurrent = i12;
    s.bias = bias;
    s.screenA = s.screenB = screen;
    s.plateCurrent = 2.0 * c.power.idleCurrent;
    {
        // Power-stage predictor state at idle (both tubes identical, Vh = 0).
        const double kA = tube.drive<ExactMath> (-bias, v[1] - c.power.rscreen * screen).plateScale;
        const double dI = kA * tube.kvb / (tube.kvb * tube.kvb + v[0] * v[0]);
        const auto load = buildLoad (c.power, fs, d.turns);
        s.slopePrev = -2.0 * d.turns * d.turns * load.E[0] * dI - 1.0;
        s.kAPrev = s.kBPrev = kA;
        s.atanA = s.atanB = std::atan (v[0] / tube.kvb);
    }

    for (auto* grid : { &s.gridA, &s.gridB })
    {
        const std::array<double, 2> u { 0.0, -bias };
        double vg = -bias;
        for (int it = 0; it < 200; ++it)
        {
            const auto gc = tube.grid (vg);
            const double f = grid->sys.Hdc[0] * u[0] + grid->sys.Hdc[1] * u[1] + grid->sys.Kdc[0] * gc.i - vg;
            if (std::abs (f) < 1.0e-12) break;
            vg -= f / (grid->sys.Kdc[0] * gc.di - 1.0);
        }
        const std::array<double, 1> i { tube.grid (vg).i };
        grid->x = grid->sys.dcState (u, i);
        grid->vPrev = vg;
        grid->tangent = 1.0 / (1.0 - grid->sys.K[0] * tube.grid (vg).di);
        grid->pPrev = grid->sys.linearPart (grid->x, u)[0];
    }

    // The last stage's output coupling capacitor sits charged to its idle plate voltage, and
    // the pipeline registers hold the idle plate and grid voltages.
    s.coupleIn = d.idlePlate[(size_t) (c.numStages - 1)];
    s.pipe = d.idlePlate;
    s.gridPipe = { s.gridA.vPrev, s.gridB.vPrev };
}

void PhysicalTubeStage::prepare (double oversampledRate)
{
    fs = oversampledRate;
    gainCoef = 1.0 - std::exp (-1.0 / (0.02 * fs));
    for (int v = 0; v < 3; ++v)
        initialiseVoice (v);
    circuit = &voiceCircuits()[(size_t) voice];
    reset();
}

void PhysicalTubeStage::reset() noexcept
{
    live = initial[(size_t) voice];
    setGain (gainKnob);
    gainNow = gainTarget;
}

void PhysicalTubeStage::setVoice (int voiceIndex) noexcept
{
    voiceIndex = std::clamp (voiceIndex, 0, 2);
    if (voiceIndex == voice && circuit != nullptr)
        return;
    voice = voiceIndex;
    circuit = &voiceCircuits()[(size_t) voice];
    reset();
}

void PhysicalTubeStage::setGain (double knob01) noexcept
{
    gainKnob = std::clamp (knob01, 0.0, 1.0);
    const auto& c = voiceCircuits()[(size_t) voice];
    gainTarget = std::pow (10.0, (c.gainLoDb + (c.gainHiDb - c.gainLoDb) * gainKnob) / 20.0);
}
} // namespace axiom::physical
