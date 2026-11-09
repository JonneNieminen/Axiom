#include "PhysicalCircuit.h"
#include <sstream>

namespace axiom::physical
{
namespace
{
/** One or two weighted entries of an incidence vector (index -1 = ground, skipped). */
struct Sel
{
    int i0 = -1; double w0 = 0.0; int i1 = -1; double w1 = 0.0;
};

Sel pair (int a, int b, double sign = 1.0) { return { a, sign, b, -sign }; }

/** Gauss-Jordan inversion with partial pivoting, in place on `m` (n x n), using `inv`. MNA
    matrices mix picofarad conductances (~1e-6 S) with unit source stamps, so pivoting matters. */
bool invertInPlace (double* m, double* inv, int n) noexcept
{
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) inv[r * n + c] = r == c ? 1.0 : 0.0;
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs (m[r * n + col]) > std::abs (m[pivot * n + col])) pivot = r;
        if (std::abs (m[pivot * n + col]) < 1.0e-300)
            return false;
        if (pivot != col)
            for (int c = 0; c < n; ++c)
            {
                std::swap (m[pivot * n + c], m[col * n + c]);
                std::swap (inv[pivot * n + c], inv[col * n + c]);
            }
        const double scale = 1.0 / m[col * n + col];
        for (int c = 0; c < n; ++c) { m[col * n + c] *= scale; inv[col * n + c] *= scale; }
        for (int r = 0; r < n; ++r)
        {
            if (r == col) continue;
            const double f = m[r * n + col];
            if (f == 0.0) continue;
            for (int c = 0; c < n; ++c) { m[r * n + c] -= f * m[col * n + c]; inv[r * n + c] -= f * inv[col * n + c]; }
        }
    }
    return true;
}
} // namespace

bool Netlist::discretise (double fs, const DkTarget& t, Workspace& w) const noexcept
{
    int nv = 0;
    for (const auto& in : inputs) nv += in.voltage ? 1 : 0;
    const int nz = nodes + nv;
    const int nx = (int) reactives.size(), nu = (int) inputs.size(), nn = (int) ports.size(), ny = (int) outputs.size();
    if (nz > Workspace::maxNz || nx > Workspace::maxNz || nu > Workspace::maxNz || t.nx != nx || t.nu != nu || t.nn != nn || t.ny != ny)
        return false;

    //-- MNA matrix: conductances, trapezoidal companions, voltage-source rows/columns.
    double* S = w.s.data();
    std::fill (S, S + nz * nz, 0.0);
    const auto stamp = [S, nz] (int a, int b, double g)
    {
        if (a >= 0) S[a * nz + a] += g;
        if (b >= 0) S[b * nz + b] += g;
        if (a >= 0 && b >= 0) { S[a * nz + b] -= g; S[b * nz + a] -= g; }
    };
    for (const auto& r : resistors) stamp (r.a, r.b, r.g);
    // Trapezoidal companion models, both of the form i = g v - x (x injected into node a),
    // x' = a g v + d x:
    //   capacitor: g = 2C/T,  x' = 2 g v - x
    //   inductor:  g = T/2L,  x' = x - 2 g v   (x = minus the usual history term)
    double* g = w.scratch.data();
    double* va = g + nx;     // a: coefficient of g v
    double* vd = va + nx;    // d: coefficient of x
    for (int j = 0; j < nx; ++j)
    {
        const auto& e = reactives[(size_t) j];
        if (e.capacitor) { g[j] = 2.0 * e.value * fs;         va[j] = 2.0;  vd[j] = -1.0; }
        else             { g[j] = 1.0 / (2.0 * e.value * fs); va[j] = -2.0; vd[j] = 1.0; }
        stamp (e.a, e.b, g[j]);
    }
    int* voltageRow = w.rows.data();   // input q -> its MNA row (or -1)
    {
        int k = 0;
        for (int q = 0; q < nu; ++q)
        {
            const auto& in = inputs[(size_t) q];
            voltageRow[q] = -1;
            if (! in.voltage) continue;
            const int row = nodes + k++;
            voltageRow[q] = row;
            if (in.a >= 0) { S[in.a * nz + row] += 1.0; S[row * nz + in.a] += 1.0; }
            if (in.b >= 0) { S[in.b * nz + row] -= 1.0; S[row * nz + in.b] -= 1.0; }
        }
    }
    double* Si = w.inv.data();
    if (! invertInPlace (S, Si, nz))
        return false;

    //-- Selector rows and right-hand-side columns, then M(row, col) = row^T S^-1 col.
    const auto M = [Si, nz] (const Sel& r, const Sel& c)
    {
        double acc = 0.0;
        const int ri[2] { r.i0, r.i1 }; const double rw[2] { r.w0, r.w1 };
        const int ci[2] { c.i0, c.i1 }; const double cw[2] { c.w0, c.w1 };
        for (int a = 0; a < 2; ++a)
            if (ri[a] >= 0)
                for (int b = 0; b < 2; ++b)
                    if (ci[b] >= 0) acc += rw[a] * cw[b] * Si[ri[a] * nz + ci[b]];
        return acc;
    };
    const auto colX = [this] (int j) { const auto& e = reactives[(size_t) j]; return pair (e.a, e.b); };     // +x into a
    const auto colU = [this, voltageRow] (int q)
    {
        const auto& in = inputs[(size_t) q];
        return in.voltage ? Sel { voltageRow[q], 1.0, -1, 0.0 } : pair (in.a, in.b);
    };
    const auto colN = [this] (int m) { const auto& p = ports[(size_t) m]; return pair (p.a, p.b, -1.0); }; // current leaves a
    const auto rowX = colX;
    const auto rowN = [this] (int n) { const auto& p = ports[(size_t) n]; return pair (p.a, p.b); };
    const auto rowY = [this, voltageRow] (int r)
    {
        const auto& o = outputs[(size_t) r];
        // MNA's source current flows from node a into the source; the current it delivers is minus that
        return o.sourceInput >= 0 ? Sel { voltageRow[o.sourceInput], -1.0, -1, 0.0 } : pair (o.a, o.b);
    };

    for (int n = 0; n < nn; ++n)
    {
        const Sel r = rowN (n);
        for (int j = 0; j < nx; ++j) t.G[j * nn + n] = M (r, colX (j));
        for (int q = 0; q < nu; ++q) t.H[q * nn + n] = M (r, colU (q));
        for (int m = 0; m < nn; ++m)
        {
            const double k = M (r, colN (m));
            t.K[n * nn + m] = k;
            t.Kcm[m * nn + n] = k;
        }
    }
    for (int o = 0; o < ny; ++o)
    {
        const Sel r = rowY (o);
        for (int j = 0; j < nx; ++j) t.D[j * ny + o] = M (r, colX (j));
        for (int q = 0; q < nu; ++q) t.E[q * ny + o] = M (r, colU (q));
        for (int m = 0; m < nn; ++m) t.F[m * ny + o] = M (r, colN (m));
    }
    for (int r = 0; r < nx; ++r)
    {
        const Sel row = rowX (r);
        const double s = va[r] * g[r];
        for (int c = 0; c < nx; ++c) t.A[c * nx + r] = s * M (row, colX (c)) + (r == c ? vd[r] : 0.0);
        for (int c = 0; c < nu; ++c) t.B[c * nx + r] = s * M (row, colU (c));
        for (int c = 0; c < nn; ++c) t.C[c * nx + r] = s * M (row, colN (c));
    }
    return true;
}

bool Netlist::dcMatrices (const DkTarget& t, std::vector<double>& Xu, std::vector<double>& Xi,
                          std::vector<double>& Hdc, std::vector<double>& Kdc) const
{
    // The fixed point of x' = A x + B u + C i is x = (I - A)^-1 (B u + C i), and the controlling
    // voltages there are v = G x + H u + K i.
    const int nx = t.nx, nu = t.nu, nn = t.nn;
    std::vector<double> m ((size_t) (nx * nx)), inv ((size_t) (nx * nx));
    for (int r = 0; r < nx; ++r)
        for (int c = 0; c < nx; ++c) m[(size_t) (r * nx + c)] = (r == c ? 1.0 : 0.0) - t.A[c * nx + r];
    if (! invertInPlace (m.data(), inv.data(), nx))
        return false;
    Xu.assign ((size_t) (nx * nu), 0.0);
    Xi.assign ((size_t) (nx * nn), 0.0);
    for (int r = 0; r < nx; ++r)
        for (int k = 0; k < nx; ++k)
        {
            const double a = inv[(size_t) (r * nx + k)];
            for (int c = 0; c < nu; ++c) Xu[(size_t) (r * nu + c)] += a * t.B[c * nx + k];
            for (int c = 0; c < nn; ++c) Xi[(size_t) (r * nn + c)] += a * t.C[c * nx + k];
        }
    Hdc.assign ((size_t) (nn * nu), 0.0);
    Kdc.assign ((size_t) (nn * nn), 0.0);
    for (int r = 0; r < nn; ++r)
    {
        for (int c = 0; c < nu; ++c)
        {
            double acc = t.H[c * nn + r];
            for (int k = 0; k < nx; ++k) acc += t.G[k * nn + r] * Xu[(size_t) (k * nu + c)];
            Hdc[(size_t) (r * nu + c)] = acc;
        }
        for (int c = 0; c < nn; ++c)
        {
            double acc = t.K[r * nn + c];
            for (int k = 0; k < nx; ++k) acc += t.G[k * nn + r] * Xi[(size_t) (k * nn + c)];
            Kdc[(size_t) (r * nn + c)] = acc;
        }
    }
    return true;
}

std::string Netlist::spice (const std::vector<std::string>& inputSources, const TriodeModel& tri,
                            const GridDiode& diode, double step, double duration, const std::string& dataFile) const
{
    const auto node = [this] (int n) -> std::string
    {
        if (n < 0) return "0";
        return names[(size_t) n].empty() ? "n" + std::to_string (n) : names[(size_t) n];
    };
    std::ostringstream s;
    s.precision (12);
    s << "* Axiom Physical engine sub-circuit, exported for an ngspice cross-check\n";
    for (size_t r = 0; r < resistors.size(); ++r)
        s << "R" << r << " " << node (resistors[r].a) << " " << node (resistors[r].b) << " " << 1.0 / resistors[r].g << "\n";
    for (size_t j = 0; j < reactives.size(); ++j)
        s << (reactives[j].capacitor ? "C" : "L") << j << " " << node (reactives[j].a) << " " << node (reactives[j].b) << " " << reactives[j].value << "\n";
    for (size_t q = 0; q < inputs.size(); ++q)
    {
        const auto& in = inputs[q];
        if (in.voltage)
            s << "V" << q << " " << node (in.a) << " " << node (in.b) << " " << inputSources[q] << "\n";
        else   // SPICE: positive current flows from n+ through the source to n-
            s << "I" << q << " " << node (in.b) << " " << node (in.a) << " " << inputSources[q] << "\n";
    }
    const auto childLaw = [] (const std::string& v, const GridDiode& d)
    {
        std::ostringstream e;
        e.precision (12);
        e << d.g << "*pwr(0.5*(" << v << "+sqrt(" << v << "*" << v << "+" << d.knee * d.knee << ")),1.5)";
        return e.str();
    };
    for (size_t k = 0; k < devices.size(); ++k)
    {
        const auto& d = devices[k];
        const std::string vg = "V(" + node (d.g) + "," + node (d.k) + ")";
        if (d.triode)
        {
            const std::string vp = "V(" + node (d.p) + "," + node (d.k) + ")";
            std::ostringstream u;
            u.precision (12);
            u << tri.C << "*(" << vp << "/" << tri.mu << "+" << vg << ")";
            const std::string sp = "(max(" + u.str() + ",0)+ln(1+exp(-abs(" + u.str() + "))))";
            std::ostringstream ik;
            ik.precision (12);
            ik << tri.G << "*pwr(" << sp << "/" << tri.C << "," << tri.gamma << ")";
            const std::string ig = childLaw (vg, tri.grid);
            s << "BG" << k << " " << node (d.g) << " " << node (d.k) << " I=" << ig << "\n";
            s << "BP" << k << " " << node (d.p) << " " << node (d.k) << " I=" << ik.str() << "-(" << ig << ")\n";
        }
        else
            s << "BD" << k << " " << node (d.g) << " " << node (d.k) << " I=" << childLaw (vg, diode) << "\n";
    }
    s << ".options method=trap reltol=1e-7 abstol=1e-15 vntol=1e-10 interp\n";
    s << ".tran " << step << " " << duration << " 0 " << step << "\n";
    s << ".control\nset wr_singlescale\nset wr_vecnames\nrun\nwrdata " << dataFile;
    for (const auto& o : outputs)
        if (o.sourceInput < 0)
            s << " v(" << node (o.a) << ")" << (o.b >= 0 ? "-v(" + node (o.b) + ")" : "");
    s << "\nquit\n.endc\n.end\n";
    return s.str();
}
} // namespace axiom::physical
