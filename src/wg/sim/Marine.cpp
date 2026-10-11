#include "wg/sim/Marine.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "wg/core/Parallel.hpp"

namespace wg {

namespace {

// Implicit diffusion along one line of cells (index(t) for t in [0, length)):
// (1 + sum of the link coefficients) x_t - sum(link * x_neighbour) = src_t.
// `link(c, d)` is the coefficient of the face between cell c and the next
// one along the line (d = +1) or the previous one (d = -1), 0 if no flux
// crosses it. Runs of active cells are independent systems; an inactive
// neighbour behind a non-zero link is a fixed (Dirichlet) height.
template <typename Index, typename Link>
void implicitLine(int length, Index index, Link link, const std::vector<double>& src, std::vector<double>& dst,
                  const std::vector<std::uint8_t>& active, std::vector<double>& cPrime, std::vector<double>& dPrime) {
    int t = 0;
    while (t < length) {
        if (!active[index(t)]) {
            dst[index(t)] = src[index(t)];
            ++t;
            continue;
        }
        int end = t;
        while (end < length && active[index(end)])
            ++end;
        const int n = end - t;
        for (int s = 0; s < n; ++s) {
            const int c = index(t + s);
            const double left = link(c, -1), right = link(c, +1);
            double diag = 1.0 + left + right, rhs = src[c];
            // Dirichlet neighbours at both ends of the run (FIXED cells).
            if (s == 0 && left > 0.0)
                rhs += left * src[index(t - 1)];
            if (s == n - 1 && right > 0.0)
                rhs += right * src[index(end)];
            const double lower = s > 0 ? -left : 0.0, upper = s < n - 1 ? -right : 0.0;
            const double m = diag - (s > 0 ? lower * cPrime[s - 1] : 0.0);
            cPrime[s] = upper / m;
            dPrime[s] = (rhs - (s > 0 ? lower * dPrime[s - 1] : 0.0)) / m;
        }
        double next = 0.0;
        for (int s = n - 1; s >= 0; --s) {
            const double x = dPrime[s] - (s < n - 1 ? cPrime[s] * next : 0.0);
            dst[index(t + s)] = x;
            next = x;
        }
        t = end;
    }
}

// Active cells from the highest to the lowest (bucket sort on the height: the
// order inside a bucket does not matter).
std::vector<int> highestFirst(const std::vector<double>& h, const std::vector<std::uint8_t>& active) {
    constexpr int kBuckets = 1 << 16;
    double lo = 1e300, hi = -1e300;
    for (std::size_t c = 0; c < h.size(); ++c) {
        if (active[c]) {
            lo = std::min(lo, h[c]);
            hi = std::max(hi, h[c]);
        }
    }
    std::vector<int> order;
    if (lo > hi)
        return order;
    const double scale = hi > lo ? (kBuckets - 1) / (hi - lo) : 0.0;
    std::vector<int> start(kBuckets + 1, 0);
    const auto bucket = [&](std::size_t c) { return kBuckets - 1 - static_cast<int>((h[c] - lo) * scale); };
    for (std::size_t c = 0; c < h.size(); ++c)
        if (active[c])
            ++start[bucket(c) + 1];
    for (int b = 0; b < kBuckets; ++b)
        start[b + 1] += start[b];
    order.resize(start[kBuckets]);
    for (std::size_t c = 0; c < h.size(); ++c)
        if (active[c])
            order[start[bucket(c)]++] = static_cast<int>(c);
    return order;
}

} // namespace

double marineDiffusion(const Regolith& regolith, std::vector<double>& h, const std::vector<std::uint8_t>& active,
                       const std::vector<std::uint8_t>& fixed, int nx, int ny, const MarineSettings& s) {
    const int n = nx * ny;
    const double cellArea = s.cellM * s.cellM;
    // Sediment of each kind on the active cells (m of column).
    std::array<std::vector<double>, kSedimentKinds> amount;
    for (int k = 0; k < kSedimentKinds; ++k) {
        amount[k].assign(n, 0.0);
        for (int c = 0; c < n; ++c)
            if (active[c])
                amount[k][c] = regolith.sed[c] * regolith.frac[c][k];
    }
    if (std::none_of(regolith.sed.begin(), regolith.sed.end(), [](double v) { return v > 0.0; }))
        return 0.0;

    struct GrainClass {
        double d0;
        std::vector<int> kinds;
    };
    const GrainClass classes[] = {{s.sandD0, {kGravel, kSand}}, {s.finesD0, {kClay}}};
    std::vector<double> half(n), trial(n), linkX(n, 0.0), linkY(n, 0.0);
    const auto takesPart = [&](int c) { return active[c] || fixed[c]; };
    double exported = 0.0;

    for (const GrainClass& grain : classes) {
        if (grain.d0 <= 0.0)
            continue;
        // Face coefficients r = D dt / dx2, D at the depth of the shallower side
        // (linkX[c]: face between c and c + 1, linkY[c]: between c and c + nx).
        const auto coefficient = [&](int a, int b) {
            if (!takesPart(a) || !takesPart(b) || (!active[a] && !active[b]))
                return 0.0;
            const double depth = std::max(0.0, s.seaLevel - std::max(h[a], h[b]));
            return grain.d0 * std::exp(-depth / s.depthScaleM) * s.dt / cellArea;
        };
        parallelFor(ny, [&](int j0, int j1) {
            for (int j = j0; j < j1; ++j)
                for (int i = 0; i < nx; ++i) {
                    const int c = j * nx + i;
                    linkX[c] = i + 1 < nx ? coefficient(c, c + 1) : 0.0;
                    linkY[c] = j + 1 < ny ? coefficient(c, c + nx) : 0.0;
                }
        });

        // Trial: implicit in x, then in y.
        parallelFor(ny, [&](int j0, int j1) {
            std::vector<double> cPrime(nx), dPrime(nx);
            for (int j = j0; j < j1; ++j) {
                const auto index = [&](int t) { return j * nx + t; };
                const auto link = [&](int c, int d) {
                    const int i = c % nx;
                    return d > 0 ? (i + 1 < nx ? linkX[c] : 0.0) : (i > 0 ? linkX[c - 1] : 0.0);
                };
                implicitLine(nx, index, link, h, half, active, cPrime, dPrime);
            }
        });
        parallelFor(nx, [&](int i0, int i1) {
            std::vector<double> cPrime(ny), dPrime(ny);
            for (int i = i0; i < i1; ++i) {
                const auto index = [&](int t) { return t * nx + i; };
                const auto link = [&](int c, int d) {
                    const int j = c / nx;
                    return d > 0 ? (j + 1 < ny ? linkY[c] : 0.0) : (j > 0 ? linkY[c - nx] : 0.0);
                };
                implicitLine(ny, index, link, half, trial, active, cPrime, dPrime);
            }
        });
        // Face fluxes of the trial (m of column over the step, > 0 towards +x / +y):
        // x from the intermediate heights, y from the final ones, so that they
        // add up exactly to trial - h.
        const auto fluxX = [&](int c) { return linkX[c] * (half[c] - half[c + 1]); };
        const auto fluxY = [&](int c) { return linkY[c] * (trial[c] - trial[c + nx]); };

        // Limited transfer. Each face can carry at most its trial flux; a cell
        // sends at most the sediment of this class it holds, including what it
        // received. Cells are visited from the highest of the trial surface down
        // (the direction of the fluxes), and a few sweeps pass on what arrived
        // after a cell was visited. (Everything a cell needs in one cache line:
        // the visiting order jumps across the grid.)
        struct Cell {
            double load[2];     // sediment of the class's kinds (m)
            double inbox[2];    // received, not yet counted
            double capacity[4]; // +x, -x, +y, -y
        };
        const int kinds = static_cast<int>(grain.kinds.size());
        std::vector<Cell> cells(n);
        parallelFor(n, [&](int c0, int c1) {
            for (int c = c0; c < c1; ++c) {
                Cell& cell = cells[c];
                cell = {};
                if (!active[c])
                    continue;
                for (int k = 0; k < kinds; ++k)
                    cell.load[k] = amount[grain.kinds[k]][c];
                const int i = c % nx, j = c / nx;
                cell.capacity[0] = i + 1 < nx ? std::max(0.0, fluxX(c)) : 0.0;
                cell.capacity[1] = i > 0 ? std::max(0.0, -fluxX(c - 1)) : 0.0;
                cell.capacity[2] = j + 1 < ny ? std::max(0.0, fluxY(c)) : 0.0;
                cell.capacity[3] = j > 0 ? std::max(0.0, -fluxY(c - nx)) : 0.0;
            }
        });
        const std::vector<int> order = highestFirst(trial, active);
        double initial = 0.0;
        for (const Cell& cell : cells)
            initial += cell.load[0] + cell.load[1];
        constexpr int kMaxSweeps = 3;
        const int offsets[4] = {1, -1, nx, -nx};
        for (int sweep = 0; sweep < kMaxSweeps && initial > 0.0; ++sweep) {
            double moved = 0.0;
            for (const int c : order) {
                Cell& cell = cells[c];
                double available = 0.0;
                for (int k = 0; k < kinds; ++k) {
                    cell.load[k] = std::max(0.0, cell.load[k] + cell.inbox[k]);
                    cell.inbox[k] = 0.0;
                    available += cell.load[k];
                }
                if (available <= 0.0)
                    continue;
                const double total = cell.capacity[0] + cell.capacity[1] + cell.capacity[2] + cell.capacity[3];
                if (total <= 0.0)
                    continue;
                // Only a mobile layer moves at the full rate: a thin film would
                // otherwise carry as much as a thick cover, at unbounded speed.
                const double sent = std::min(total * std::min(1.0, available / s.mobileLayerM), available);
                for (int f = 0; f < 4; ++f) {
                    if (cell.capacity[f] <= 0.0)
                        continue;
                    const int to = c + offsets[f];
                    const double volume = cell.capacity[f] / total * sent;
                    if (fixed[to]) {
                        exported += volume * cellArea;
                        continue;
                    }
                    for (int k = 0; k < kinds; ++k)
                        cells[to].inbox[k] += volume * cell.load[k] / available;
                }
                for (int k = 0; k < kinds; ++k)
                    cell.load[k] *= 1.0 - sent / available;
                for (double& cap : cell.capacity)
                    cap *= 1.0 - sent / total;
                moved += sent;
            }
            if (moved <= 1e-9 * initial)
                break;
        }
        // Late arrivals (cells already visited), then the new surface.
        for (int c = 0; c < n; ++c) {
            if (!active[c])
                continue;
            for (int k = 0; k < kinds; ++k)
                amount[grain.kinds[k]][c] = cells[c].load[k] + cells[c].inbox[k];
            double sed = 0.0;
            for (int k = 0; k < kSedimentKinds; ++k)
                sed += amount[k][c];
            h[c] = regolith.b[c] + sed;
        }
    }

    for (int c = 0; c < n; ++c) {
        if (!active[c])
            continue;
        double sed = 0.0;
        for (int k = 0; k < kSedimentKinds; ++k)
            sed += amount[k][c];
        regolith.sed[c] = sed;
        if (sed > 0.0)
            for (int k = 0; k < kSedimentKinds; ++k)
                regolith.frac[c][k] = amount[k][c] / sed;
        h[c] = regolith.b[c] + sed;
    }
    return exported;
}

} // namespace wg
