#pragma once
/*
    Netlists, Modified Nodal Analysis and the discrete K-method (DK) state-space form used by
    the Physical engine, plus the N-port Newton-Raphson solver that runs them.

    A circuit is written as a netlist. discretise() stamps it into an MNA matrix, discretises
    every capacitor and inductor with the trapezoidal rule (a conductance in parallel with a
    "history" current source; those sources are the states) and eliminates the linear part:

        x[n+1] = A x[n] + B u[n] + C i[n]          state update
        y[n]   = D x[n] + E u[n] + F i[n]          outputs
        v[n]   = G x[n] + H u[n] + K i[n]          voltages controlling the tubes
        i[n]   = f(v[n])                           tube equations

    discretise() writes into fixed-size arrays and a preallocated workspace, so it is real-time
    safe: when a pot moves (gain, tone stack, presence) the matrices are rebuilt on the audio
    thread. The states are capacitor/inductor history currents, which do not depend on any
    resistor, so a rebuild continues seamlessly from the same x.
*/
#include "PhysicalMath.h"
#include <string>
#include <vector>

namespace axiom::physical
{
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

/** Where discretise() writes. Everything is column-major (K also row-major, as K): every
    product then runs as a sum of scaled columns, which vectorises across the rows. */
struct DkTarget
{
    int nx = 0, nu = 0, nn = 0, ny = 0;
    double *A = nullptr, *B = nullptr, *C = nullptr, *D = nullptr, *E = nullptr, *F = nullptr;
    double *G = nullptr, *H = nullptr, *K = nullptr, *Kcm = nullptr;
};

/** Scratch space for discretise(); allocated once, in prepare(). */
struct Workspace
{
    static constexpr int maxNz = 48;
    std::vector<double> s, inv, scratch;
    std::vector<int> rows;
    Workspace() : s ((size_t) (maxNz * maxNz)), inv ((size_t) (maxNz * maxNz)), scratch ((size_t) (maxNz * maxNz)), rows ((size_t) maxNz) {}
};

class Netlist
{
public:
    static constexpr int gnd = -1;
    Netlist() = default;
    explicit Netlist (int numNodes) : nodes (numNodes) { names.resize ((size_t) numNodes); }

    void name (int node, const char* n) { names[(size_t) node] = n; }

    /** Returns a handle for setResistance(). */
    int resistor (int a, int b, double ohms)      { resistors.push_back ({ a, b, 1.0 / std::max (ohms, 1.0e-3) }); return (int) resistors.size() - 1; }
    void setResistance (int handle, double ohms) noexcept { resistors[(size_t) handle].g = 1.0 / std::max (ohms, 1.0); }
    double resistance (int handle) const noexcept { return 1.0 / resistors[(size_t) handle].g; }
    void capacitor (int a, int b, double farads)  { reactives.push_back ({ a, b, farads, true }); }
    void inductor (int a, int b, double henries)  { reactives.push_back ({ a, b, henries, false }); }
    /** u[k] = V(a) - V(b). */
    int voltageInput (int a, int b)               { inputs.push_back ({ a, b, true }); return (int) inputs.size() - 1; }
    /** u[k] amps flow into node a from outside and leave from node b. */
    int currentInput (int a, int b)               { inputs.push_back ({ a, b, false }); return (int) inputs.size() - 1; }
    /** A triode: ports (g,k) for grid current and (p,k) for plate current, in that order. Add
        all triodes before any grid diode: the solver relies on that port layout. */
    int triode (int g, int p, int k)              { ports.push_back ({ g, k }); ports.push_back ({ p, k }); devices.push_back ({ g, p, k, true }); return (int) devices.size() - 1; }
    /** A power-tube grid conducting to its (grounded) cathode: one port (g,k). */
    int gridDiode (int g, int k)                  { ports.push_back ({ g, k }); devices.push_back ({ g, -1, k, false }); return (int) devices.size() - 1; }
    /** y[k] = V(a) - V(b). */
    int output (int a, int b)                     { outputs.push_back ({ a, b, -1 }); return (int) outputs.size() - 1; }
    /** y[k] = current a voltage input delivers into its node a (e.g. a B+ node's supply current). */
    int sourceCurrent (int input)                 { outputs.push_back ({ -1, -1, input }); return (int) outputs.size() - 1; }

    int numStates() const noexcept  { return (int) reactives.size(); }
    int numInputs() const noexcept  { return (int) inputs.size(); }
    int numPorts() const noexcept   { return (int) ports.size(); }
    int numOutputs() const noexcept { return (int) outputs.size(); }

    /** Real-time safe given a Workspace: no allocation. Returns false if the system is singular. */
    bool discretise (double sampleRate, const DkTarget& out, Workspace& w) const noexcept;

    /** Prepare-time: x* = Xu u + Xi i and v* = Hdc u + Kdc i at a DC operating point. */
    bool dcMatrices (const DkTarget& dk, std::vector<double>& Xu, std::vector<double>& Xi,
                     std::vector<double>& Hdc, std::vector<double>& Kdc) const;

    /** A SPICE deck of this circuit (offline checks): tubes become behavioural sources with the
        same equations, inputs become the given source lines, outputs are written with wrdata. */
    std::string spice (const std::vector<std::string>& inputSources, const TriodeModel& triode,
                       const GridDiode& diode, double step, double duration, const std::string& dataFile) const;

private:
    struct Resistor { int a, b; double g; };
    struct Reactive { int a, b; double value; bool capacitor; };
    struct Input { int a, b; bool voltage; };
    struct Pair { int a, b; };
    struct Output { int a, b, sourceInput; };
    struct Device { int g, p, k; bool triode; };
    int nodes = 0;
    std::vector<std::string> names;
    std::vector<Resistor> resistors;
    std::vector<Reactive> reactives;
    std::vector<Input> inputs;
    std::vector<Pair> ports;
    std::vector<Output> outputs;
    std::vector<Device> devices;
};

/** y += M x for a column-major R x Cc matrix, with four independent partial sums. Each sample's
    work depends on the previous sample's, so these products sit on the critical path: one
    accumulator per row would chain all Cc multiply-adds back to back (~4 cycles each); four
    chains run in parallel and are added at the end. */
template <int R, int Cc>
AXIOM_HOT void accumulateColumns (std::array<double, R>& y, const double* m, const double* x) noexcept
{
    std::array<double, R> a0 {}, a1 {}, a2 {}, a3 {};
    constexpr int quad = Cc / 4 * 4;
    AXIOM_UNROLL for (int c = 0; c < quad; c += 4)
        AXIOM_UNROLL for (int r = 0; r < R; ++r)
        {
            a0[(size_t) r] += m[c * R + r] * x[c];
            a1[(size_t) r] += m[(c + 1) * R + r] * x[c + 1];
            a2[(size_t) r] += m[(c + 2) * R + r] * x[c + 2];
            a3[(size_t) r] += m[(c + 3) * R + r] * x[c + 3];
        }
    AXIOM_UNROLL for (int c = quad; c < Cc; ++c)
        AXIOM_UNROLL for (int r = 0; r < R; ++r) a0[(size_t) r] += m[c * R + r] * x[c];
    AXIOM_UNROLL for (int r = 0; r < R; ++r) y[(size_t) r] += (a0[(size_t) r] + a1[(size_t) r]) + (a2[(size_t) r] + a3[(size_t) r]);
}

/** A DK system with compile-time dimensions. */
template <int NX, int NU, int NN, int NY>
struct DkSystem
{
    // All column-major; K is kept in both layouts.
    std::array<double, NX * NX> A {};
    std::array<double, NX * NU> B {};
    std::array<double, NX * NN> C {};
    std::array<double, NY * NX> D {};
    std::array<double, NY * NU> E {};
    std::array<double, NY * NN> F {};
    std::array<double, NN * NX> G {};
    std::array<double, NN * NU> H {};
    std::array<double, NN * NN> K {};    // row-major
    std::array<double, NN * NN> Kcm {};

    DkTarget target() noexcept
    {
        return { NX, NU, NN, NY, A.data(), B.data(), C.data(), D.data(), E.data(), F.data(), G.data(), H.data(), K.data(), Kcm.data() };
    }

    bool build (const Netlist& net, double fs, Workspace& w) noexcept
    {
        if (net.numStates() != NX || net.numInputs() != NU || net.numPorts() != NN || net.numOutputs() != NY)
            return false;
        return net.discretise (fs, target(), w);
    }

    /** p = G x + H u */
    AXIOM_HOT std::array<double, NN> linearPart (const std::array<double, NX>& x, const std::array<double, NU>& u) const noexcept
    {
        std::array<double, NN> p {};
        accumulateColumns<NN, NU> (p, H.data(), u.data());
        accumulateColumns<NN, NX> (p, G.data(), x.data());
        return p;
    }

    /** y = D x + E u + F i (before advance()). */
    AXIOM_HOT std::array<double, NY> outputs (const std::array<double, NX>& x, const std::array<double, NU>& u, const std::array<double, NN>& i) const noexcept
    {
        std::array<double, NY> y {};
        accumulateColumns<NY, NX> (y, D.data(), x.data());
        accumulateColumns<NY, NU> (y, E.data(), u.data());
        accumulateColumns<NY, NN> (y, F.data(), i.data());
        return y;
    }

    /** x <- A x + B u + C i, as a sum of scaled columns. */
    AXIOM_HOT void advance (std::array<double, NX>& x, const std::array<double, NU>& u, const std::array<double, NN>& i) const noexcept
    {
        std::array<double, NX> next {};
        accumulateColumns<NX, NX> (next, A.data(), x.data());
        accumulateColumns<NX, NU> (next, B.data(), u.data());
        accumulateColumns<NX, NN> (next, C.data(), i.data());
        x = next;
    }
};

//==============================================================================
/** Dense LU with partial pivoting for the small Newton systems (N <= 8). Fixed size, so the
    compiler unrolls it; the pivot search is selects. */
template <int N>
struct SmallLu
{
    std::array<double, N * N> a {};
    std::array<double, N> invDiag {};   // 1 / U_kk: the solves multiply instead of divide
    std::array<int, N> piv {};

    /** LU of `a` in place. The fast path does not pivot: these Jacobians, J = K dI/dv - I, have
        diagonals of magnitude >= 1 and stay well conditioned without row exchanges, and skipping
        the pivot search shortens the critical path to one division per column. If a pivot does
        come out small relative to its row, the factorisation restarts with partial pivoting
        (a branch essentially never taken, so it costs nothing when it is not). */
    AXIOM_HOT void factor() noexcept
    {
        const auto saved = a;
        bool weak = false;
        AXIOM_UNROLL for (int k = 0; k < N; ++k)
        {
            piv[(size_t) k] = k;
            const double d = a[(size_t) (k * N + k)];
            weak = weak || ! (std::abs (d) > 1.0e-6);
            const double inv = 1.0 / d;
            invDiag[(size_t) k] = inv;
            AXIOM_UNROLL for (int r = k + 1; r < N; ++r)
            {
                const double f = a[(size_t) (r * N + k)] * inv;
                a[(size_t) (r * N + k)] = f;
                AXIOM_UNROLL for (int c = k + 1; c < N; ++c) a[(size_t) (r * N + c)] -= f * a[(size_t) (k * N + c)];
            }
        }
        if (weak) [[unlikely]]
        {
            a = saved;
            factorPivoted();
        }
    }

    void factorPivoted() noexcept
    {
        for (int k = 0; k < N; ++k)
        {
            int p = k;
            double best = std::abs (a[(size_t) (k * N + k)]);
            for (int r = k + 1; r < N; ++r)
            {
                const double m = std::abs (a[(size_t) (r * N + k)]);
                p = m > best ? r : p;
                best = m > best ? m : best;
            }
            piv[(size_t) k] = p;
            if (p != k)
                for (int c = 0; c < N; ++c) std::swap (a[(size_t) (k * N + c)], a[(size_t) (p * N + c)]);
            const double d = a[(size_t) (k * N + k)];
            const double inv = 1.0 / (std::abs (d) > 1.0e-300 ? d : 1.0e-300);
            invDiag[(size_t) k] = inv;
            for (int r = k + 1; r < N; ++r)
            {
                const double f = a[(size_t) (r * N + k)] * inv;
                a[(size_t) (r * N + k)] = f;
                for (int c = k + 1; c < N; ++c) a[(size_t) (r * N + c)] -= f * a[(size_t) (k * N + c)];
            }
        }
    }

    /** Solves A x = b in place. */
    AXIOM_HOT void solve (std::array<double, N>& b) const noexcept
    {
        AXIOM_UNROLL for (int k = 0; k < N; ++k)
            if (piv[(size_t) k] != k) std::swap (b[(size_t) k], b[(size_t) piv[(size_t) k]]);
        AXIOM_UNROLL for (int r = 1; r < N; ++r)
            AXIOM_UNROLL for (int c = 0; c < r; ++c) b[(size_t) r] -= a[(size_t) (r * N + c)] * b[(size_t) c];
        AXIOM_UNROLL for (int r = N - 1; r >= 0; --r)
        {
            AXIOM_UNROLL for (int c = r + 1; c < N; ++c) b[(size_t) r] -= a[(size_t) (r * N + c)] * b[(size_t) c];
            b[(size_t) r] *= invDiag[(size_t) r];
        }
    }
};

/** A DK circuit with NT triodes followed by ND grid diodes as its nonlinear ports, solved
    every sample by Newton-Raphson on v = p + K i(v).

    - Predictor: dv = (I - K dI/dv)^-1 dp = -J^-1 dp with the last factored Jacobian, so the
      first guess follows the circuit into cutoff or grid conduction.
    - Each pass evaluates every tube once (the evaluations are independent, so an out-of-order
      core runs them side by side), forms F = p + K i - v and J = K dI/dv - I column by column
      from the block-diagonal device Jacobian, LU-factors J and steps.
    - Stops at |F| < 1e-6 V, at a step below newtonAcceptStep (currents moved along the tangent),
      or at the hard cap, where the last evaluated point is kept so the currents handed to the
      state update belong to the voltages. A step that makes |F| worse is halved (backtracking). */
template <int NX, int NU, int NN, int NY, int NT, int ND>
struct CircuitSolver
{
    static_assert (NN == 2 * NT + ND);
    using System = DkSystem<NX, NU, NN, NY>;
    System sys;
    TriodeModel triode {};
    GridDiode diode { 2.0e-3, 0.2 };

    std::array<double, NX> x {};
    std::array<double, NN> v {}, pPrev {}, i {};
    std::array<double, NU> uPrev {};
    std::array<double, NY> y {};
    SmallLu<NN> lu {};
    SolverStats stats {};

    struct Jac { std::array<double, NT> g {}, pg {}, pp {}; std::array<double, ND> d {}; };

    /** Physical bounds of each port voltage, and the largest Newton step allowed on it. A tube's
        grid-cathode voltage lies between the deepest cutoff the supply can impose and a few tens
        of volts of grid conduction; its plate-cathode voltage between a few volts negative
        (cathode follower or inductive kick) and well above B+. Keeping every iterate inside
        these bounds keeps every tube current bounded, so even a solve that ends at its cap
        cannot inject runaway energy into the state. */
    static constexpr double gridLow = -600.0, gridHigh = 30.0, plateLow = -60.0, plateHigh = 1200.0;
    static constexpr double gridStep = 150.0, plateStep = 400.0;

    AXIOM_HOT static double bound (int port, double value) noexcept
    {
        const bool plate = port < 2 * NT && (port & 1) == 1;
        return std::clamp (value, plate ? plateLow : gridLow, plate ? plateHigh : gridHigh);
    }
    AXIOM_HOT static double limitStep (int port, double step) noexcept
    {
        const double m = port < 2 * NT && (port & 1) == 1 ? plateStep : gridStep;
        return std::clamp (step, -m, m);
    }

    template <TubeMath M>
    AXIOM_HOT void evaluate (const std::array<double, NN>& vv, std::array<double, NN>& ii, Jac& j) const noexcept
    {
        for (int t = 0; t < NT; ++t)
        {
            const auto r = triode.template evaluate<M> (vv[(size_t) (2 * t)], vv[(size_t) (2 * t + 1)]);
            ii[(size_t) (2 * t)] = r.ig;
            ii[(size_t) (2 * t + 1)] = r.ip;
            j.g[(size_t) t] = r.dIg;
            j.pg[(size_t) t] = r.dIpG;
            j.pp[(size_t) t] = r.dIpP;
        }
        for (int d = 0; d < ND; ++d)
        {
            const auto r = diode (vv[(size_t) (2 * NT + d)]);
            ii[(size_t) (2 * NT + d)] = r.i;
            j.d[(size_t) d] = r.di;
        }
    }

    /** J = K dI/dv - I (dense, row-major), built column by column from the block-diagonal
        device Jacobian. */
    AXIOM_HOT void buildJacobian (const Jac& j, std::array<double, NN * NN>& a) const noexcept
    {
        const auto& k = sys.Kcm;
        AXIOM_UNROLL for (int r = 0; r < NN; ++r)
            AXIOM_UNROLL for (int c = 0; c < NN; ++c) a[(size_t) (r * NN + c)] = r == c ? -1.0 : 0.0;
        AXIOM_UNROLL for (int t = 0; t < NT; ++t)
        {
            const int cg = 2 * t, cp = 2 * t + 1;
            AXIOM_UNROLL for (int r = 0; r < NN; ++r)
            {
                a[(size_t) (r * NN + cg)] += k[(size_t) (cg * NN + r)] * j.g[(size_t) t] + k[(size_t) (cp * NN + r)] * j.pg[(size_t) t];
                a[(size_t) (r * NN + cp)] += k[(size_t) (cp * NN + r)] * j.pp[(size_t) t];
            }
        }
        for (int d = 0; d < ND; ++d)
        {
            const int c = 2 * NT + d;
            for (int r = 0; r < NN; ++r) a[(size_t) (r * NN + c)] += k[(size_t) (c * NN + r)] * j.d[(size_t) d];
        }
    }

    /** J into lu.a, factored. */
    AXIOM_HOT void jacobian (const Jac& j) noexcept
    {
        buildJacobian (j, lu.a);
        lu.factor();
    }

    /** F(v) = p + K i(v) - v, for the offline Jacobian check. */
    std::array<double, NN> residual (const std::array<double, NN>& p, const std::array<double, NN>& vv) const noexcept
    {
        std::array<double, NN> ii {};
        Jac j {};
        evaluate<ExactMath> (vv, ii, j);
        std::array<double, NN> f {};
        for (int r = 0; r < NN; ++r) f[(size_t) r] = p[(size_t) r] - vv[(size_t) r];
        accumulateColumns<NN, NN> (f, sys.Kcm.data(), ii.data());
        return f;
    }

    /** One sample. Cap: the hard iteration cap (the math policy's by default). */
    template <TubeMath M, int Cap = M::maxIterations>
    AXIOM_HOT void step (const std::array<double, NU>& u) noexcept
    {
        const auto p = sys.linearPart (x, u);
        std::array<double, NN> dv {};
        for (int r = 0; r < NN; ++r) dv[(size_t) r] = p[(size_t) r] - pPrev[(size_t) r];
        lu.solve (dv);                                       // J^-1 dp
        std::array<double, NN> vv {};
        for (int r = 0; r < NN; ++r) vv[(size_t) r] = bound (r, v[(size_t) r] - limitStep (r, dv[(size_t) r]));

        Jac j {};
        std::array<double, NN> ii {}, last {};
        double lastNorm = 1.0e300;
        int iter = 0;
        bool converged = false;
        for (;;)
        {
            evaluate<M> (vv, ii, j);
            ++iter;
            std::array<double, NN> f {};
            AXIOM_UNROLL for (int r = 0; r < NN; ++r) f[(size_t) r] = p[(size_t) r] - vv[(size_t) r];
            accumulateColumns<NN, NN> (f, sys.Kcm.data(), ii.data());
            double norm = 0.0;
            AXIOM_UNROLL for (int r = 0; r < NN; ++r) norm = std::max (norm, std::abs (f[(size_t) r]));
            if (norm > lastNorm && iter < Cap)
            {
                for (int r = 0; r < NN; ++r) { last[(size_t) r] *= 0.5; vv[(size_t) r] -= last[(size_t) r]; }
                continue;
            }
            lastNorm = norm;
            converged = norm < newtonTolerance;
            if (converged || iter >= Cap)
                break;
            jacobian (j);
            lu.solve (f);                                    // J^-1 F; the step is -J^-1 F
            double biggest = 0.0;
            for (int r = 0; r < NN; ++r)
            {
                const double target = bound (r, vv[(size_t) r] + limitStep (r, -f[(size_t) r]));
                const double d = target - vv[(size_t) r];
                vv[(size_t) r] = target;
                last[(size_t) r] = d;
                biggest = std::max (biggest, std::abs (d));
            }
            if (biggest < newtonAcceptStep)
            {
                for (int t = 0; t < NT; ++t)
                {
                    const double dg = last[(size_t) (2 * t)], dp = last[(size_t) (2 * t + 1)];
                    ii[(size_t) (2 * t)] += j.g[(size_t) t] * dg;
                    ii[(size_t) (2 * t + 1)] += j.pg[(size_t) t] * dg + j.pp[(size_t) t] * dp;
                }
                for (int d = 0; d < ND; ++d) ii[(size_t) (2 * NT + d)] += j.d[(size_t) d] * last[(size_t) (2 * NT + d)];
                converged = true;
                break;
            }
        }
        stats.add (iter, ! converged);
        v = vv;
        pPrev = p;
        uPrev = u;
        i = ii;
        y = sys.outputs (x, u, ii);
        sys.advance (x, u, ii);
    }

    /** After the matrices change (a pot moved), re-anchor the predictor to the new linear part. */
    void rebased() noexcept { pPrev = sys.linearPart (x, uPrev); }

    /** Prepare-time DC solve for constant inputs u (damped Newton, exact math). */
    bool solveDc (const Netlist& net, const std::array<double, NU>& u)
    {
        std::vector<double> Xu, Xi, Hdc, Kdc;
        if (! net.dcMatrices (sys.target(), Xu, Xi, Hdc, Kdc))
            return false;
        std::array<double, NN> vv {};
        for (int t = 0; t < NT; ++t) { vv[(size_t) (2 * t)] = -1.0; vv[(size_t) (2 * t + 1)] = 100.0; }
        for (int d = 0; d < ND; ++d) vv[(size_t) (2 * NT + d)] = -10.0;
        std::array<double, NN> ii {};
        Jac j {};
        for (int it = 0; it < 2000; ++it)
        {
            evaluate<ExactMath> (vv, ii, j);
            std::array<double, NN> f {};
            double norm = 0.0;
            for (int r = 0; r < NN; ++r)
            {
                double acc = -vv[(size_t) r];
                for (int c = 0; c < NU; ++c) acc += Hdc[(size_t) (r * NU + c)] * u[(size_t) c];
                for (int c = 0; c < NN; ++c) acc += Kdc[(size_t) (r * NN + c)] * ii[(size_t) c];
                f[(size_t) r] = acc;
                norm = std::max (norm, std::abs (acc));
            }
            if (norm < 1.0e-10)
                break;
            // Jacobian from Kdc
            const auto saveK = sys.Kcm;
            for (int r = 0; r < NN; ++r)
                for (int c = 0; c < NN; ++c) sys.Kcm[(size_t) (c * NN + r)] = Kdc[(size_t) (r * NN + c)];
            jacobian (j);
            sys.Kcm = saveK;
            lu.solve (f);
            for (int r = 0; r < NN; ++r) vv[(size_t) r] += std::clamp (-f[(size_t) r], -5.0, 5.0);
        }
        evaluate<ExactMath> (vv, ii, j);
        for (int r = 0; r < NX; ++r)
        {
            double acc = 0.0;
            for (int c = 0; c < NU; ++c) acc += Xu[(size_t) (r * NU + c)] * u[(size_t) c];
            for (int c = 0; c < NN; ++c) acc += Xi[(size_t) (r * NN + c)] * ii[(size_t) c];
            x[(size_t) r] = acc;
        }
        v = vv;
        i = ii;
        uPrev = u;
        pPrev = sys.linearPart (x, u);
        y = sys.outputs (x, u, ii);
        jacobian (j);   // the predictor's Jacobian at the operating point
        return true;
    }
};
} // namespace axiom::physical
