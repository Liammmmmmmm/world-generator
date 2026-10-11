#include "wg/sim/Hillslope.hpp"

#include <algorithm>
#include <cmath>

#include "wg/core/Parallel.hpp"

namespace wg {

Composition Regolith::remove(int c, double volume) const {
    Composition removed{};
    const double fromSed = std::min(volume, sed[c]);
    if (fromSed > 0.0) {
        for (int k = 0; k < kSedimentKinds; ++k)
            removed[k] = fromSed * frac[c][k];
        sed[c] = std::max(0.0, sed[c] - fromSed);
    }
    const double fromRock = volume - fromSed;
    if (fromRock > 0.0) {
        b[c] -= fromRock;
        removed[rockKind[c]] += fromRock;
    }
    return removed;
}

void Regolith::deposit(int c, const Composition& volumes) const {
    double total = 0.0;
    for (double v : volumes)
        total += v;
    if (total <= 0.0)
        return;
    const double after = sed[c] + total;
    for (int k = 0; k < kSedimentKinds; ++k)
        frac[c][k] = (sed[c] * frac[c][k] + volumes[k]) / after;
    sed[c] = after;
}

namespace {

// Implicit half step along the rows of a width x height grid (the y pass runs
// on the transposed grid, so both passes read memory contiguously). Each run of
// consecutive active cells (kind 1) is an independent tridiagonal system,
// (1 - a d2_row) dst = src + a d2_column src, solved by the Thomas algorithm.
// Its neighbours are walls (kind 0: no flux) or sinks (kind 2: fixed height,
// absorbing the flux, added to `absorbed`). Non-active cells keep their height.
void implicitRows(const std::vector<double>& src, std::vector<double>& dst, const std::vector<std::uint8_t>& kind,
                  int width, int height, double a, std::vector<double>& absorbed) {
    constexpr std::uint8_t kActive = 1, kSink = 2;
    parallelFor(height, [&](int j0, int j1) {
        std::vector<double> cPrime(width), xPrime(width);
        for (int j = j0; j < j1; ++j) {
            const int row = j * width;
            int t = 0;
            while (t < width) {
                if (kind[row + t] != kActive) {
                    dst[row + t] = src[row + t];
                    ++t;
                    continue;
                }
                int end = t;
                while (end < width && kind[row + end] == kActive)
                    ++end;
                const int n = end - t;
                const bool sinkLeft = t > 0 && kind[row + t - 1] == kSink;
                const bool sinkRight = end < width && kind[row + end] == kSink;
                // Forward sweep (off-diagonals -a, diagonal 1 + a * links).
                for (int s = 0; s < n; ++s) {
                    const int c = row + t + s;
                    double across = 0.0; // explicit term of the other direction
                    if (j > 0 && kind[c - width] != 0)
                        across += src[c - width] - src[c];
                    if (j < height - 1 && kind[c + width] != 0)
                        across += src[c + width] - src[c];
                    double rhs = src[c] + a * across;
                    double links = (s > 0) + (s < n - 1);
                    if (s == 0 && sinkLeft) {
                        links += 1.0;
                        rhs += a * src[c - 1];
                    }
                    if (s == n - 1 && sinkRight) {
                        links += 1.0;
                        rhs += a * src[c + 1];
                    }
                    const double m = 1.0 + a * links + (s > 0 ? a * cPrime[s - 1] : 0.0);
                    cPrime[s] = -a / m;
                    xPrime[s] = (rhs + (s > 0 ? a * xPrime[s - 1] : 0.0)) / m;
                }
                // Back substitution.
                double next = 0.0;
                for (int s = n - 1; s >= 0; --s) {
                    const double x = xPrime[s] - (s < n - 1 ? cPrime[s] * next : 0.0);
                    dst[row + t + s] = x;
                    next = x;
                }
                // Implicit fluxes into the sinks at both ends (same row: same thread).
                if (sinkLeft)
                    absorbed[row + t - 1] += a * (dst[row + t] - src[row + t - 1]);
                if (sinkRight)
                    absorbed[row + end] += a * (dst[row + end - 1] - src[row + end]);
                t = end;
            }
        }
    });
    // Explicit fluxes into the sinks from the rows above and below, gathered by
    // the sink's row.
    parallelFor(height, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < width; ++i) {
                const int c = j * width + i;
                if (kind[c] != kSink)
                    continue;
                if (j > 0 && kind[c - width] == kActive)
                    absorbed[c] += a * (src[c - width] - src[c]);
                if (j < height - 1 && kind[c + width] == kActive)
                    absorbed[c] += a * (src[c + width] - src[c]);
            }
    });
}

// dst (height x width) = transpose of src (width x height), by cache blocks.
template <typename T>
void transpose(const std::vector<T>& src, std::vector<T>& dst, int width, int height) {
    constexpr int kBlock = 32;
    parallelFor((height + kBlock - 1) / kBlock, [&](int b0, int b1) {
        for (int jb = b0 * kBlock; jb < std::min(height, b1 * kBlock); jb += kBlock)
            for (int ib = 0; ib < width; ib += kBlock)
                for (int j = jb; j < std::min(height, jb + kBlock); ++j)
                    for (int i = ib; i < std::min(width, ib + kBlock); ++i)
                        dst[static_cast<std::size_t>(i) * height + j] = src[static_cast<std::size_t>(j) * width + i];
    }, 1);
}

} // namespace

int diffuseADI(std::vector<double>& h, const std::vector<std::uint8_t>& kind, int nx, int ny, double r,
               std::vector<double>* absorbed) {
    if (r <= 0.0)
        return 0;
    const int substeps = std::max(1, static_cast<int>(std::ceil(r)));
    const double a = r / substeps / 2.0; // each half step carries r_sub / 2
    const std::size_t n = h.size();
    std::vector<double> half(n), halfT(n), nextT(n), sinkX(n, 0.0), sinkYT(n, 0.0);
    std::vector<std::uint8_t> kindT(n);
    transpose(kind, kindT, nx, ny);
    for (int sub = 0; sub < substeps; ++sub) {
        implicitRows(h, half, kind, nx, ny, a, sinkX); // x implicit, y explicit
        transpose(half, halfT, nx, ny);
        implicitRows(halfT, nextT, kindT, ny, nx, a, sinkYT); // y implicit, x explicit
        transpose(nextT, h, ny, nx);
    }
    if (absorbed) {
        transpose(sinkYT, *absorbed, ny, nx);
        for (std::size_t c = 0; c < n; ++c)
            (*absorbed)[c] += sinkX[c];
    }
    return substeps;
}

double thermalCollapse(const Regolith& regolith, std::vector<double>& h, const std::vector<std::uint8_t>& source,
                       const std::vector<std::uint8_t>& fixed, const std::vector<double>& rockTan, double sedimentTan,
                       double fraction, int nx, int ny, double dx) {
    // Neighbour k at (di, dj); the opposite direction is 7 - k.
    static constexpr int kDi[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr int kDj[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    const int n = nx * ny;
    const double diagonal = dx * std::sqrt(2.0);
    const auto inside = [&](int i, int j) { return i >= 0 && j >= 0 && i < nx && j < ny; };
    // Per source cell: the amount it sends, the repose slope that governs the
    // shares and the sum of the excesses at that slope; the share of each
    // neighbour is recomputed from them (heights do not change until the end).
    std::vector<double> plan(n, 0.0), out(n, 0.0), limit(n, 0.0), totalExcess(n, 0.0), scale(n, 1.0);
    std::vector<Composition> removed(n);
    // Excess of `from` over its neighbour `to` at its governing angle (planned
    // share: plan[from] * excess / totalExcess[from]).
    const auto excessTowards = [&](int from, int to, int k) {
        const double length = (kDi[k] != 0 && kDj[k] != 0) ? diagonal : dx;
        return std::max(0.0, h[from] - h[to] - length * limit[from]);
    };

    // 1. Transfers, from the current heights.
    parallelFor(ny, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
            for (int i = 0; i < nx; ++i) {
                const int c = j * nx + i;
                if (!source[c] || fixed[c])
                    continue;
                double sedExcess = 0.0, rockExcess = 0.0;
                for (int k = 0; k < 8; ++k) {
                    const int ni = i + kDi[k], nj = j + kDj[k];
                    if (!inside(ni, nj) || fixed[nj * nx + ni])
                        continue;
                    const double length = (kDi[k] != 0 && kDj[k] != 0) ? diagonal : dx;
                    const double drop = h[c] - h[nj * nx + ni];
                    sedExcess = std::max(sedExcess, drop - length * sedimentTan);
                    rockExcess = std::max(rockExcess, drop - length * rockTan[c]);
                }
                const double sedMove = std::min(regolith.sed[c], fraction * sedExcess);
                const double rockMove = fraction * rockExcess;
                const double amount = std::max(sedMove, rockMove);
                if (amount <= 0.0)
                    continue;
                // Shares in proportion to the excess at the governing angle.
                limit[c] = sedMove >= rockMove ? sedimentTan : rockTan[c];
                for (int k = 0; k < 8; ++k) {
                    const int ni = i + kDi[k], nj = j + kDj[k];
                    if (!inside(ni, nj) || fixed[nj * nx + ni])
                        continue;
                    const double length = (kDi[k] != 0 && kDj[k] != 0) ? diagonal : dx;
                    totalExcess[c] += std::max(0.0, h[c] - h[nj * nx + ni] - length * limit[c]);
                }
                plan[c] = amount;
            }
        }
    });

    // 2. Stability: with simultaneous transfers, a cell below several high
    //    neighbours would receive half the excess of each and overshoot them,
    //    an oscillation that grows. A receiver takes at most half the largest
    //    excess of its donors over it; the transfers into it are scaled down.
    parallelFor(ny, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
            for (int i = 0; i < nx; ++i) {
                const int c = j * nx + i;
                if (fixed[c])
                    continue;
                double incoming = 0.0, largest = 0.0;
                for (int k = 0; k < 8; ++k) {
                    const int ni = i + kDi[k], nj = j + kDj[k];
                    if (!inside(ni, nj))
                        continue;
                    const int from = nj * nx + ni;
                    if (plan[from] <= 0.0)
                        continue;
                    const double excess = excessTowards(from, c, 7 - k);
                    incoming += plan[from] * excess / totalExcess[from];
                    largest = std::max(largest, excess);
                }
                if (incoming > 0.5 * largest)
                    scale[c] = 0.5 * largest / incoming;
            }
        }
    });
    parallelFor(ny, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
            for (int i = 0; i < nx; ++i) {
                const int c = j * nx + i;
                if (plan[c] <= 0.0)
                    continue;
                for (int k = 0; k < 8; ++k) {
                    const int ni = i + kDi[k], nj = j + kDj[k];
                    if (!inside(ni, nj) || fixed[nj * nx + ni])
                        continue;
                    const int to = nj * nx + ni;
                    out[c] += plan[c] * excessTowards(c, to, k) / totalExcess[c] * scale[to];
                }
            }
        }
    });

    // 3. Removal (each cell only touches itself).
    parallelFor(n, [&](int c0, int c1) {
        for (int c = c0; c < c1; ++c)
            if (out[c] > 0.0)
                removed[c] = regolith.remove(c, out[c]);
    });

    // 4. Deposition: each cell gathers what its neighbours send it.
    parallelFor(ny, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
            for (int i = 0; i < nx; ++i) {
                const int c = j * nx + i;
                if (fixed[c])
                    continue;
                Composition incoming{};
                bool any = false;
                for (int k = 0; k < 8; ++k) {
                    const int ni = i + kDi[k], nj = j + kDj[k];
                    if (!inside(ni, nj))
                        continue;
                    const int from = nj * nx + ni;
                    if (out[from] <= 0.0)
                        continue;
                    const double excess = excessTowards(from, c, 7 - k);
                    if (excess <= 0.0)
                        continue;
                    // share of what `from` actually sends
                    const double share = plan[from] * excess / totalExcess[from] * scale[c] / out[from];
                    for (int s = 0; s < kSedimentKinds; ++s)
                        incoming[s] += removed[from][s] * share;
                    any = true;
                }
                if (any)
                    regolith.deposit(c, incoming);
            }
        }
    });

    double moved = 0.0;
    for (int c = 0; c < n; ++c) {
        moved += out[c];
        h[c] = regolith.b[c] + regolith.sed[c];
    }
    return moved;
}

} // namespace wg
