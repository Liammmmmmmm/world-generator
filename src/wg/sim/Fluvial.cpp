#include "wg/sim/Fluvial.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace wg {

namespace {

// Erosion of a cell `drop` above its base level, covered by `sed` of
// sediment, over one step (factors f = K Q^m dt / L for sediment and rock).
// Implicit: erosion = drop f / (1 + f). When that would cut through the
// cover, the step is split in time: the part that strips the cover at the
// sediment rate (solving drop f_tau / (1 + f_tau) = sed), then the rest at the
// rock rate from the rock surface. Never more than `drop`.
double implicitErosion(double drop, double sed, double fSed, double fRock) {
    if (sed > 0.0) {
        const double eroded = drop * fSed / (1.0 + fSed);
        if (eroded <= sed)
            return eroded;
        const double stripped = sed / (drop - sed);                 // f over the stripping time
        const double rest = std::max(0.0, 1.0 - stripped / fSed);   // share of the step left
        const double f = fRock * rest;
        return sed + (drop - sed) * f / (1.0 + f);
    }
    return drop * fRock / (1.0 + fRock);
}

constexpr std::size_t kMaxDeltaCells = 4096; // cells a delta fill may visit per river mouth

struct Mouth {
    int cell;
    double volume;   // m3 over the step
    Composition mix; // m3/yr per kind
};

} // namespace

FluvialResult fluvialStep(const FlowRouter& router, const FluvialSettings& s, const Regolith& regolith,
                          const std::vector<double>& discharge, const std::vector<double>& area,
                          const std::vector<double>& rockK, const std::vector<std::uint8_t>& fixed,
                          std::vector<double>& h, std::vector<double>& sedimentFlux, std::vector<double>& erosionRate,
                          std::vector<double>& depositionRate, const std::vector<std::uint8_t>* frozen) {
    const int n = static_cast<int>(h.size());
    // The cells that drain somewhere, in stack order (outlets do nothing here,
    // and the sea is most of the map).
    std::vector<int> stack;
    stack.reserve(router.stack().size());
    for (const int c : router.stack())
        if (router.receivers()[c] != c)
            stack.push_back(c);
    const auto& receivers = router.receivers();
    const auto& distance = router.receiverDistance();
    const auto& ocean = router.ocean();
    const auto& water = router.water();
    const double cellArea = s.cellM * s.cellM;
    const double perVolume = s.dt / cellArea; // flux (m3/yr) over the step -> thickness (m)

    const std::vector<double> h0 = h;
    std::vector<double> h1 = h0, qsIn(n, 0.0), previous(n, 0.0), transport(n, 0.0);
    std::vector<std::uint8_t> lake(n, 0);
    for (int c = 0; c < n; ++c) {
        lake[c] = !ocean[c] && !fixed[c] && water[c] > h0[c];
        if (receivers[c] != c)
            transport[c] = std::pow(discharge[c], s.m) * s.dt / distance[c]; // f / K
    }

    FluvialResult result;
    for (int iteration = 0; iteration < std::max(1, s.maxIterations); ++iteration) {
        result.iterations = iteration + 1;
        // 1. h from the base level up, with the flux of the previous iteration.
        for (const int c : stack) {
            const int r = receivers[c];
            if (fixed[c] || ocean[c] || lake[c] || (frozen && (*frozen)[c]))
                continue; // outlets; lakes are filled in the flux pass; under the ice h stays
            // Base level: the sea, the surface of the lake the receiver lies in
            // (nothing erodes below it), else the receiver's new height.
            const double base = ocean[r] ? std::max(h1[r], s.seaLevel) : lake[r] ? water[r] : h1[r];
            // Deposit G Qs / A, never more than what comes in.
            const double deposit = s.g > 0.0 ? std::min(s.g * qsIn[c] * s.dt / area[c], qsIn[c] * perVolume) : 0.0;
            const double raised = h0[c] + deposit;
            if (raised <= base) {
                h1[c] = raised;
                continue;
            }
            const double eroded = implicitErosion(raised - base, regolith.sed[c] + deposit,
                                                  s.kSediment * transport[c], rockK[c] * transport[c]);
            h1[c] = raised - eroded;
#ifndef NDEBUG
            if (h1[c] < base - 1e-9 || (ocean[r] && h1[c] < s.seaLevel - 1e-9))
                throw std::runtime_error("fluvial erosion below the base level");
#endif
        }
        // 2. Flux down the reversed stack (upstream first): what enters a cell,
        //    plus its erosion, minus its deposit. Lakes keep what fits.
        std::swap(previous, qsIn);
        std::fill(qsIn.begin(), qsIn.end(), 0.0);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const int c = *it, r = receivers[c];
            if (lake[c])
                h1[c] = h0[c] + std::min(qsIn[c] * perVolume, water[c] - h0[c]);
            qsIn[r] += std::max(0.0, qsIn[c] + (h0[c] - h1[c]) / perVolume);
        }
        if (s.g <= 0.0)
            break; // no feedback of the flux on h: converged
        double change = 0.0, total = 0.0;
        for (const int c : stack) {
            change += std::abs(qsIn[c] - previous[c]);
            total += qsIn[c];
        }
        if (iteration > 0 && change <= s.tolerance * total)
            break;
    }

    // 3. Final pass, upstream first: apply each cell's change to the regolith
    //    and carry the flux with its composition. A deposit is capped by what
    //    actually comes in, so the budget is exact.
    std::fill(qsIn.begin(), qsIn.end(), 0.0);
    std::vector<Composition> qsMix(n, Composition{});
    std::vector<Mouth> mouths;
    std::fill(erosionRate.begin(), erosionRate.end(), 0.0);
    std::fill(depositionRate.begin(), depositionRate.end(), 0.0);
    const auto& fullStack = router.stack(); // with the outlets: they receive the flux
    for (auto it = fullStack.rbegin(); it != fullStack.rend(); ++it) {
        const int c = *it, r = receivers[c];
        const double in = qsIn[c];
        const Composition& inMix = qsMix[c];
        sedimentFlux[c] = in;
        if (fixed[c]) {
            result.exported += in * s.dt;
            continue;
        }
        if (r == c) { // ocean outlet: the load builds a delta (step 4)
            if (in > 0.0) {
                result.toSea += in * s.dt;
                mouths.push_back({c, in * s.dt, inMix});
            }
            continue;
        }
        double delta = h1[c] - h0[c];
        Composition outMix = inMix;
        if (delta > 0.0) {
            delta = std::min(delta, in * perVolume);
            const double share = in > 0.0 ? delta / (in * perVolume) : 0.0;
            Composition volumes;
            for (int k = 0; k < kSedimentKinds; ++k) {
                volumes[k] = inMix[k] * share * perVolume;
                outMix[k] = inMix[k] * (1.0 - share);
            }
            regolith.deposit(c, volumes);
            depositionRate[c] = delta / s.dt;
            if (lake[c])
                result.toLakes += delta * cellArea;
        } else if (delta < 0.0) {
            const Composition removed = regolith.remove(c, -delta);
            for (int k = 0; k < kSedimentKinds; ++k)
                outMix[k] += removed[k] / perVolume;
            erosionRate[c] = -delta / s.dt;
        }
        h[c] = regolith.b[c] + regolith.sed[c];
        const double out = std::max(0.0, in - delta / perVolume);
        sedimentFlux[c] = out;
        qsIn[r] += out;
        for (int k = 0; k < kSedimentKinds; ++k)
            qsMix[r][k] += outMix[k];
    }

    // 4. Deltas: each river mouth fills the sea up to sea level, from the mouth
    //    outwards (breadth first through the sea cells, at most kMaxDeltaCells
    //    of them), then whatever is left is spread evenly over the cells
    //    visited. A whole river's load
    //    (millions of m3 per step) dropped on one cell would stand hundreds of
    //    meters above the sea, out of reach of the marine diffusion, and dam
    //    the river.
    std::vector<int> stamp(n, -1), queue;
    for (std::size_t m = 0; m < mouths.size(); ++m) {
        const Mouth& mouth = mouths[m];
        double left = mouth.volume / cellArea; // m of column
        Composition fractions{};
        for (int k = 0; k < kSedimentKinds; ++k)
            fractions[k] = mouth.mix[k] * s.dt / mouth.volume;
        const auto put = [&](int c, double thickness) {
            Composition volumes;
            for (int k = 0; k < kSedimentKinds; ++k)
                volumes[k] = fractions[k] * thickness;
            regolith.deposit(c, volumes);
            depositionRate[c] += thickness / s.dt;
            h[c] = regolith.b[c] + regolith.sed[c];
            left -= thickness;
        };
        queue.assign(1, mouth.cell);
        stamp[mouth.cell] = static_cast<int>(m);
        for (std::size_t q = 0; q < queue.size() && q < kMaxDeltaCells && left > 0.0; ++q) {
            const int c = queue[q];
            const double room = s.seaLevel - h[c];
            if (room > 0.0)
                put(c, std::min(room, left));
            const int i = c % router.nx(), j = c / router.nx();
            const int next[4][2] = {{i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1}};
            for (const auto& [ni, nj] : next) {
                if (ni < 0 || nj < 0 || ni >= router.nx() || nj >= router.ny())
                    continue;
                const int nb = nj * router.nx() + ni;
                if (stamp[nb] != static_cast<int>(m) && ocean[nb] && !fixed[nb]) {
                    stamp[nb] = static_cast<int>(m);
                    queue.push_back(nb);
                }
            }
        }
        // Beyond the cells the fill may visit: a thin even layer over all of
        // them (the new delta top), rather than a mound on one cell.
        if (left > 0.0) {
            const std::size_t visited = std::min(queue.size(), kMaxDeltaCells);
            const double layer = left / visited;
            for (std::size_t q = 0; q < visited; ++q)
                put(queue[q], layer);
        }
    }
    return result;
}

} // namespace wg
