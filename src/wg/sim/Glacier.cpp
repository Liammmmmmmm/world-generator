#include "wg/sim/Glacier.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "wg/core/Parallel.hpp"

namespace wg {

namespace {

constexpr double kSecondsPerYear = 3.15576e7;
constexpr double kGravity = 9.81;
constexpr double kMaxIceStepYears = 10.0;   // the mass balance changes slowly: at most this per sub-step
constexpr int kConvergenceCheck = 20;       // sub-steps between two convergence checks
// Ice hardly transmits more basal stress than its yield stress (~1e5 Pa).
// Uncapped, thin ice on a steep slope (gradient ~1 over a 50 m cell) slid at
// km/yr, and the erosion dug an overdeepening that steepened itself without end.
constexpr double kYieldStressPa = 1.5e5;
constexpr double kMoraineMaxM = 2.0;        // moraine built per step on a cell; the rest goes on as outwash
constexpr std::size_t kSeaFillCells = 4096; // sea cells the debris reaching the sea may fill

double minmod(double a, double b) {
    if (a * b <= 0.0)
        return 0.0;
    return std::abs(a) < std::abs(b) ? a : b;
}

} // namespace

GlacierModel::GlacierModel(const IslandParams& params, int nx, int ny, double cellM)
    : g_(params.erosion.glacial), nx_(nx), ny_(ny), cellM_(cellM) {
    factor_ = std::max(1, static_cast<int>(std::lround(g_.iceGridCellM / cellM)));
    cnx_ = (nx + factor_ - 1) / factor_;
    cny_ = (ny + factor_ - 1) / factor_;
    coarseM_ = cellM * factor_;
    rhoG_ = g_.iceDensity * kGravity;
    const double aPerYear = g_.glenA * kSecondsPerYear; // Pa^-3 s^-1 -> Pa^-3 yr^-1
    gamma_ = 2.0 * aPerYear / 5.0 * rhoG_ * rhoG_ * rhoG_;
    const std::size_t n = static_cast<std::size_t>(cnx_) * cny_;
    ice_.assign(n, 0.0);
    bed_.assign(n, 0.0);
    sliding_.assign(n, 0.0);
    melt_.assign(n, 0.0);
    balance_.assign(n, 0.0);
}

int GlacierModel::fine(int ci, int cj, int& i0, int& j0, int& i1, int& j1) const {
    i0 = ci * factor_;
    j0 = cj * factor_;
    i1 = std::min(nx_, i0 + factor_);
    j1 = std::min(ny_, j0 + factor_);
    return (i1 - i0) * (j1 - j0);
}

bool GlacierModel::hasIce() const {
    return std::any_of(ice_.begin(), ice_.end(), [](double h) { return h > 0.0; });
}

double GlacierModel::volume() const {
    return std::accumulate(ice_.begin(), ice_.end(), 0.0) * coarseM_ * coarseM_;
}

void GlacierModel::meltAll(double years) {
    for (std::size_t c = 0; c < ice_.size(); ++c) {
        budget_.melted += ice_[c] * coarseM_ * coarseM_;
        melt_[c] = years > 0.0 ? ice_[c] / years : 0.0;
        ice_[c] = 0.0;
        sliding_[c] = 0.0;
        balance_[c] = 0.0;
    }
}

int GlacierModel::relax(const std::vector<double>& h, const std::vector<double>& precipitation, const Climate& climate) {
    const int n = cnx_ * cny_;
    const double dx = coarseM_, area = dx * dx;
    // Coarse bed and precipitation: means over the fine cells of each block.
    std::vector<double> rain(n, 0.0);
    for (int cj = 0; cj < cny_; ++cj)
        for (int ci = 0; ci < cnx_; ++ci) {
            int i0, j0, i1, j1;
            const int count = fine(ci, cj, i0, j0, i1, j1);
            double sb = 0.0, sp = 0.0;
            for (int j = j0; j < j1; ++j)
                for (int i = i0; i < i1; ++i) {
                    sb += h[j * nx_ + i];
                    sp += precipitation[j * nx_ + i];
                }
            bed_[cj * cnx_ + ci] = sb / count;
            rain[cj * cnx_ + ci] = sp / count;
        }

    std::vector<double> s(n), next(n), qx(n, 0.0), qy(n, 0.0), mass(n), outScale(n, 1.0);
    const auto surface = [&](int i, int j) {
        i = std::clamp(i, 0, cnx_ - 1);
        j = std::clamp(j, 0, cny_ - 1);
        return s[j * cnx_ + i];
    };
    // Flux through the face between a and b (b = a + 1 along x or y), MUSCL on s.
    const auto faceFlux = [&](int a, int b, double sA, double sB, double sBeforeA, double sAfterB, double cross,
                              double& dMax) {
        const double slope = (sB - sA) / dx;
        const double gradient = std::hypot(slope, cross);
        if (gradient <= 0.0)
            return 0.0;
        const double bedFace = std::max(bed_[a], bed_[b]); // the ridge between them
        const double left = sA + 0.5 * minmod(sA - sBeforeA, sB - sA);
        const double right = sB - 0.5 * minmod(sB - sA, sAfterB - sB);
        // Upstream, and never more than the upstream cell holds (the
        // reconstruction can overshoot it next to a cliff).
        const double thickness =
            std::min(std::max(0.0, (slope < 0.0 ? left : right) - bedFace), slope < 0.0 ? ice_[a] : ice_[b]);
        if (thickness <= 0.0)
            return 0.0;
        const double h3 = thickness * thickness * thickness;
        // Sliding u_s = C tau_b^2, tau_b capped at the yield stress of the ice.
        const double tau = std::min(rhoG_ * thickness * gradient, kYieldStressPa);
        const double d = gamma_ * h3 * thickness * thickness * gradient * gradient +
                         g_.slidingCoeff * tau * tau * thickness / gradient;
        dMax = std::max(dMax, d);
        return -d * slope;
    };

    int substeps = 0;
    for (; substeps < g_.maxSubsteps; ++substeps) {
        for (int c = 0; c < n; ++c) {
            s[c] = bed_[c] + ice_[c];
            const double t = climate.baseTemperatureC + climate.offsetC -
                             climate.lapseRateCPerKm * std::max(s[c] - climate.referenceLevel, 0.0) / 1000.0;
            mass[c] = (t < g_.snowTemperatureC ? rain[c] * g_.accumulationRate : 0.0) -
                      g_.ablationGradient * std::max(0.0, t);
        }
        // Fluxes (m2/yr) through the faces: qx[c] between c and c + 1, qy[c] between c and c + cnx.
        double dMax = 0.0;
        for (int j = 0; j < cny_; ++j)
            for (int i = 0; i < cnx_; ++i) {
                const int c = j * cnx_ + i;
                qx[c] = qy[c] = 0.0;
                if (i + 1 < cnx_ && (ice_[c] > 0.0 || ice_[c + 1] > 0.0)) {
                    const double cross = (surface(i, j + 1) - surface(i, j - 1) + surface(i + 1, j + 1) -
                                          surface(i + 1, j - 1)) / (4.0 * dx);
                    qx[c] = faceFlux(c, c + 1, s[c], s[c + 1], surface(i - 1, j), surface(i + 2, j), cross, dMax);
                }
                if (j + 1 < cny_ && (ice_[c] > 0.0 || ice_[c + cnx_] > 0.0)) {
                    const double cross = (surface(i + 1, j) - surface(i - 1, j) + surface(i + 1, j + 1) -
                                          surface(i - 1, j + 1)) / (4.0 * dx);
                    qy[c] = faceFlux(c, c + cnx_, s[c], s[c + cnx_], surface(i, j - 1), surface(i, j + 2), cross, dMax);
                }
            }
        const double dt = dMax > 0.0 ? std::min(kMaxIceStepYears, 0.2 * dx * dx / dMax) : kMaxIceStepYears;
        // A cell never sends more ice than it holds: next to a cliff of the bed,
        // the surface step comes from the rock, not the ice, and the stable
        // step of the diffusion does not bound the outflow by the ice there.
        // Each face has one upstream cell, so scaling its outflows keeps the
        // mass exact.
        for (int j = 0; j < cny_; ++j)
            for (int i = 0; i < cnx_; ++i) {
                const int c = j * cnx_ + i;
                double out = std::max(0.0, qx[c]) + std::max(0.0, qy[c]);
                if (i > 0)
                    out += std::max(0.0, -qx[c - 1]);
                if (j > 0)
                    out += std::max(0.0, -qy[c - cnx_]);
                out *= dt / dx; // m of ice
                outScale[c] = out > ice_[c] && out > 0.0 ? ice_[c] / out : 1.0;
            }
        for (int j = 0; j < cny_; ++j)
            for (int i = 0; i < cnx_; ++i) {
                const int c = j * cnx_ + i;
                if (i + 1 < cnx_)
                    qx[c] *= qx[c] > 0.0 ? outScale[c] : outScale[c + 1];
                if (j + 1 < cny_)
                    qy[c] *= qy[c] > 0.0 ? outScale[c] : outScale[c + cnx_];
            }

        double change = 0.0;
        int iced = 0;
        for (int j = 0; j < cny_; ++j)
            for (int i = 0; i < cnx_; ++i) {
                const int c = j * cnx_ + i;
                const double divergence = (qx[c] - (i > 0 ? qx[c - 1] : 0.0) + qy[c] - (j > 0 ? qy[c - cnx_] : 0.0)) / dx;
                double value = ice_[c] - dt * divergence;
                if (value < 0.0) { // the limited fluxes keep this rare (ice created: counted)
                    budget_.clamped += -value * area;
                    value = 0.0;
                }
                melt_[c] = 0.0;
                if (mass[c] >= 0.0) {
                    value += mass[c] * dt;
                    budget_.accumulated += mass[c] * dt * area;
                } else {
                    const double ablation = std::min(-mass[c] * dt, value);
                    value -= ablation;
                    budget_.ablated += ablation * area;
                    melt_[c] = ablation / dt;
                }
                // Calving: no ice on deep water, nor on the border of the map.
                const bool border = i == 0 || j == 0 || i == cnx_ - 1 || j == cny_ - 1;
                if (value > 0.0 && (border || bed_[c] < climate.seaLevel - g_.calvingDepthM)) {
                    budget_.calved += value * area;
                    value = 0.0;
                }
                if (value > 0.0 || ice_[c] > 0.0) {
                    change += std::abs(value - ice_[c]) / dt;
                    ++iced;
                }
                next[c] = value;
                balance_[c] = mass[c];
            }
        ice_.swap(next);
        // Equilibrium: the ice no longer changes (mean |dH/dt| over the ice).
        if (substeps % kConvergenceCheck == kConvergenceCheck - 1 && (iced == 0 || change / iced < g_.convergenceTol))
            break;
    }

    // Sliding velocity at the cell centers.
    for (int j = 0; j < cny_; ++j)
        for (int i = 0; i < cnx_; ++i) {
            const int c = j * cnx_ + i;
            s[c] = bed_[c] + ice_[c];
        }
    for (int j = 0; j < cny_; ++j)
        for (int i = 0; i < cnx_; ++i) {
            const int c = j * cnx_ + i;
            const double gx = (surface(i + 1, j) - surface(i - 1, j)) / (2.0 * dx);
            const double gy = (surface(i, j + 1) - surface(i, j - 1)) / (2.0 * dx);
            const double tau = std::min(rhoG_ * ice_[c] * std::hypot(gx, gy), kYieldStressPa);
            sliding_[c] = g_.slidingCoeff * tau * tau;
        }
    return substeps;
}

double GlacierModel::bilinear(const std::vector<double>& field, int i, int j) const {
    // Coarse cell centers at fine coordinates (ci + 0.5) * factor - 0.5.
    const double x = std::clamp((i + 0.5) / factor_ - 0.5, 0.0, cnx_ - 1.0);
    const double y = std::clamp((j + 0.5) / factor_ - 0.5, 0.0, cny_ - 1.0);
    const int x0 = std::min(static_cast<int>(x), cnx_ - 2 < 0 ? 0 : cnx_ - 2);
    const int y0 = std::min(static_cast<int>(y), cny_ - 2 < 0 ? 0 : cny_ - 2);
    const int x1 = std::min(x0 + 1, cnx_ - 1), y1 = std::min(y0 + 1, cny_ - 1);
    const double tx = x - x0, ty = y - y0;
    return (1 - ty) * ((1 - tx) * field[y0 * cnx_ + x0] + tx * field[y0 * cnx_ + x1]) +
           ty * ((1 - tx) * field[y1 * cnx_ + x0] + tx * field[y1 * cnx_ + x1]);
}

void GlacierModel::fineFields(std::vector<double>& ice, std::vector<double>& sliding, std::vector<double>& melt,
                              std::vector<double>& balance) const {
    const std::size_t n = static_cast<std::size_t>(nx_) * ny_;
    ice.resize(n);
    sliding.resize(n);
    melt.resize(n);
    balance.resize(n);
    parallelFor(ny_, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < nx_; ++i) {
                const std::size_t c = static_cast<std::size_t>(j) * nx_ + i;
                ice[c] = std::max(0.0, bilinear(ice_, i, j));
                sliding[c] = ice[c] > 0.0 ? std::max(0.0, bilinear(sliding_, i, j)) : 0.0;
                melt[c] = std::max(0.0, bilinear(melt_, i, j));
                balance[c] = bilinear(balance_, i, j);
            }
    });
}

double GlacierModel::fillSea(const Regolith& regolith, int start, const Composition& mix, double seaLevel,
                             std::vector<double>& h) const {
    const double fineArea = cellM_ * cellM_;
    double left = 0.0;
    for (double v : mix)
        left += v / fineArea; // m of column
    if (left <= 0.0)
        return 0.0;
    const double total = left;
    const double top = seaLevel - 1.0;
    std::vector<int> queue{start};
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(nx_) * ny_, 0);
    seen[start] = 1;
    for (std::size_t q = 0; q < queue.size() && q < kSeaFillCells && left > 0.0; ++q) {
        const int c = queue[q];
        const bool edge = c % nx_ == 0 || c / nx_ == 0 || c % nx_ == nx_ - 1 || c / nx_ == ny_ - 1;
        if (!edge && h[c] < top) { // (the map edge is FIXED)
            const double thickness = std::min(left, top - h[c]);
            Composition volumes;
            for (int k = 0; k < kSedimentKinds; ++k)
                volumes[k] = mix[k] / fineArea * thickness / total;
            regolith.deposit(c, volumes);
            h[c] = regolith.b[c] + regolith.sed[c];
            left -= thickness;
        }
        const int i = c % nx_, j = c / nx_;
        const int next[4][2] = {{i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1}};
        for (const auto& [ni, nj] : next) {
            if (ni < 1 || nj < 1 || ni >= nx_ - 1 || nj >= ny_ - 1)
                continue;
            const int nb = nj * nx_ + ni;
            if (!seen[nb] && h[nb] < seaLevel) {
                seen[nb] = 1;
                queue.push_back(nb);
            }
        }
    }
    return left * fineArea; // what does not fit: off the map
}

double GlacierModel::depositDebris(const Regolith& regolith, const std::vector<double>& eroded,
                                   const std::vector<Composition>& debris, const std::vector<double>& fineIce,
                                   double threshold, double seaLevel, std::vector<double>& h) const {
    const int n = cnx_ * cny_;
    const double fineArea = cellM_ * cellM_;
    // Debris of each coarse cell (m3 per kind).
    std::vector<Composition> load(n, Composition{});
    bool any = false;
    for (int j = 0; j < ny_; ++j)
        for (int i = 0; i < nx_; ++i) {
            const int c = j * nx_ + i;
            if (eroded[c] <= 0.0)
                continue;
            any = true;
            auto& l = load[(j / factor_) * cnx_ + i / factor_];
            for (int k = 0; k < kSedimentKinds; ++k)
                l[k] += debris[c][k] * fineArea;
        }
    if (!any)
        return 0.0;
    const auto top = [&](int c) { return bed_[c] + ice_[c]; };
    const auto lowest = [&](int c) { // steepest descent on the ice (or bed) surface
        const int ci = c % cnx_, cj = c / cnx_;
        int next = -1;
        double steepest = 0.0;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const int ni = ci + di, nj = cj + dj;
                if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= cnx_ || nj >= cny_)
                    continue;
                const int nb = nj * cnx_ + ni;
                const double drop = (top(c) - top(nb)) / std::hypot(di, dj);
                if (drop > steepest) {
                    steepest = drop;
                    next = nb;
                }
            }
        return next;
    };
    // Spreads `volume` (m3, of composition `mix` in m3 per kind summing to
    // `total`) over the fine cells of block c given, `cap` m at most each.
    const auto spread = [&](int c, Composition& mix, double cap, bool thinIceOnly) {
        double total = 0.0;
        for (double v : mix)
            total += v;
        int i0, j0, i1, j1;
        fine(c % cnx_, c / cnx_, i0, j0, i1, j1);
        std::vector<int> spots;
        for (int j = std::max(j0, 1); j < std::min(j1, ny_ - 1); ++j) // (the map edge is FIXED)
            for (int i = std::max(i0, 1); i < std::min(i1, nx_ - 1); ++i)
                if (!thinIceOnly || fineIce[j * nx_ + i] < threshold)
                    spots.push_back(j * nx_ + i);
        if (spots.empty() || total <= 0.0)
            return;
        const double share = std::min(1.0, cap * spots.size() * fineArea / total); // of the load dropped here
        for (const int f : spots) {
            Composition volumes;
            for (int k = 0; k < kSedimentKinds; ++k)
                volumes[k] = mix[k] * share / spots.size() / fineArea;
            regolith.deposit(f, volumes);
            h[f] = regolith.b[f] + regolith.sed[f];
        }
        for (double& v : mix)
            v *= 1.0 - share;
    };

    // Down the ice surface, highest first: carried by the ice while it is
    // thick; where it thins (front, margins), moraines of at most
    // kMoraineMaxM per step on each cell; the rest goes on downhill as
    // outwash, to the sea (spread there, for the marine diffusion) or off the map.
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return top(a) > top(b); });
    double exported = 0.0;
    for (const int c : order) {
        double total = 0.0;
        for (double v : load[c])
            total += v;
        if (total <= 0.0)
            continue;
        const int ci = c % cnx_, cj = c / cnx_;
        const bool border = ci == 0 || cj == 0 || ci == cnx_ - 1 || cj == cny_ - 1;
        const int next = lowest(c);
        if (ice_[c] < threshold || next < 0) {
            if (bed_[c] < seaLevel) { // the sea: it fills from here, like a delta
                int i0, j0, i1, j1;
                fine(ci, cj, i0, j0, i1, j1);
                exported += fillSea(regolith, (i0 + i1) / 2 + (j0 + j1) / 2 * nx_, load[c], seaLevel, h);
                load[c] = {};
                continue;
            }
            spread(c, load[c], kMoraineMaxM, true);
        }
        double left = 0.0;
        for (double v : load[c])
            left += v;
        if (left <= 0.0)
            continue;
        if (next >= 0 && !border) {
            for (int k = 0; k < kSedimentKinds; ++k)
                load[next][k] += load[c][k];
        } else if (border) {
            exported += left;
        } else {
            spread(c, load[c], 1e300, false); // a closed hollow: it fills
        }
        load[c] = {};
    }
    return exported;
}

} // namespace wg
