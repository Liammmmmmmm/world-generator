#include "wg/sim/Coastal.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>

#include "wg/core/Parallel.hpp"

namespace wg {

namespace {

constexpr int kDi[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDj[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
constexpr double kBeachAboveSeaM = 1.0;  // beaches build up to this above sea level
constexpr int kBeachRadius = 3;          // cells searched around the cliff for sheltered spots
constexpr std::size_t kSpreadCells = 4096; // cells the debris may fill, outwards from the cliff
constexpr double kBelowPlatformM = 1.0;    // the debris stays this far below the abrasion platform
constexpr double kDiffractionCells = 2.0;  // land this close does not block a fetch ray
constexpr int kSweepRadius = 3;            // cells in front of an exposed coast swept down to the platform
constexpr double kWaveBaseFactor = 2.0;    // water shallower than this times the platform depth damps the waves

} // namespace

bool CoastalErosion::isCoast(const std::vector<double>& h, const std::vector<std::uint8_t>& sea, int c,
                             double seaLevel) const {
    if (h[c] < seaLevel || sea[c])
        return false;
    const int i = c % nx_, j = c / nx_;
    for (int k = 0; k < 8; ++k) {
        const int ni = i + kDi[k], nj = j + kDj[k];
        if (ni >= 0 && nj >= 0 && ni < nx_ && nj < ny_ && sea[nj * nx_ + ni])
            return true;
    }
    return false;
}

double CoastalErosion::cellVolume(double h, const CoastalSettings& s) const {
    return std::max(0.0, h - (s.seaLevel - s.platformDepthM)) * s.cellM * s.cellM;
}

double CoastalErosion::rayExposure(const std::vector<double>& h, int c, const CoastalSettings& s) const {
    const int rays = std::max(1, s.fetchRayCount);
    const double swell = s.swellDirectionDeg * std::numbers::pi / 180.0;
    const double maxCells = s.fetchMaxM / s.cellM;
    const int i = c % nx_, j = c / nx_;
    double sum = 0.0, totalWeight = 0.0;
    for (int k = 0; k < rays; ++k) {
        const double theta = 2.0 * std::numbers::pi * k / rays;
        const double w = std::max(0.0, std::cos(theta - swell));
        totalWeight += w * w;
        if (w <= 0.0)
            continue;
        // March over the water until land, the maximum fetch or the map edge
        // (beyond it: open sea). Land within the first kDiffractionCells does
        // not count: waves bend around small irregularities of the coast, and a
        // notch one or two cells deep would otherwise shelter itself, build a
        // beach in front and stop the whole cliff.
        // The waves lose their energy over the shallows in front of the coast
        // (the abrasion platform, water shallower than twice its depth), until
        // the first deep water: exp(-width / platform_attenuation_m). As the
        // cliff retreats its platform widens, and the retreat slows down
        // (Trenhaile 2000); without it, a constant retreat wore whole islands
        // away over a few Ma.
        double distance = maxCells, shallows = 0.0;
        bool deep = false;
        for (double t = 1.0; t < maxCells; t += 1.0) {
            const int x = static_cast<int>(std::lround(i + t * std::cos(theta)));
            const int y = static_cast<int>(std::lround(j + t * std::sin(theta)));
            if (x < 0 || y < 0 || x >= nx_ || y >= ny_)
                break;
            const int at = y * nx_ + x;
            if (t > kDiffractionCells && at != c && h[at] >= s.seaLevel) {
                distance = t;
                break;
            }
            if (!deep && h[at] < s.seaLevel - kWaveBaseFactor * s.platformDepthM)
                deep = true;
            else if (!deep)
                shallows += 1.0;
        }
        const double damping = s.platformAttenuationM > 0.0 ? std::exp(-shallows * s.cellM / s.platformAttenuationM) : 1.0;
        sum += w * w * std::min(distance, maxCells) / maxCells * damping;
    }
    return totalWeight > 0.0 ? sum / totalWeight : 0.0;
}

void CoastalErosion::updateExposure(const std::vector<double>& h, const std::vector<std::uint8_t>& sea,
                                    const CoastalSettings& s) {
    parallelFor(ny_, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < nx_; ++i) {
                const int c = j * nx_ + i;
                exposure_[c] = isCoast(h, sea, c, s.seaLevel) ? rayExposure(h, c, s) : 0.0;
            }
    });
}

double CoastalErosion::step(const Regolith& regolith, std::vector<double>& h, std::vector<std::uint8_t>& sea,
                            const std::vector<std::uint8_t>& fixed, const std::vector<double>& hardness,
                            const CoastalSettings& s) {
    const double cellArea = s.cellM * s.cellM;
    const double platform = s.seaLevel - s.platformDepthM;
    std::deque<int> ready;
    // 1. Accumulation on the coast cells. The hardness is that of the column:
    //    sediment and rock weighted by their thickness above the platform.
    // Cells that joined the coast since the last update (scree or a beach on
    // the platform, land uncovered by the sea) get their exposure now: with
    // none, they would neither erode nor let the swell reach the cliff behind.
    std::vector<std::uint8_t> coast(static_cast<std::size_t>(nx_) * ny_);
    parallelFor(ny_, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < nx_; ++i) {
                const int c = j * nx_ + i;
                coast[c] = !fixed[c] && isCoast(h, sea, c, s.seaLevel);
                if (!coast[c])
                    exposure_[c] = 0.0;
                else if (exposure_[c] == 0.0)
                    exposure_[c] = rayExposure(h, c, s);
            }
    });
    for (int c = 0; c < nx_ * ny_; ++c) {
        if (!coast[c]) {
            accumulator_[c] = 0.0; // only the coast keeps its wear
            continue;
        }
        const double rock = std::max(0.0, regolith.b[c] - platform), cover = regolith.sed[c];
        const double hard = rock + cover > 0.0 ? (rock * hardness[c] + cover * s.sedimentHardness) / (rock + cover)
                                               : s.sedimentHardness;
        accumulator_[c] += s.waveErosionCoeff * s.swellEnergy * exposure_[c] / std::max(hard, 1e-3) * s.cellM * s.dt;
        if (accumulator_[c] >= cellVolume(h[c], s))
            ready.push_back(c);
    }

    // 2. Destruction, cell by cell; the excess wears the cells behind.
    double destroyed = 0.0, exported = 0.0;
    std::vector<int> newCoast;
    std::vector<int> stamp(static_cast<std::size_t>(nx_) * ny_, -1), queue;
    int fill = 0;
    // Sediment of composition `fractions` sent offshore from `start`: it fills
    // the sea up to 1 m below the platform, outwards; the rest, ground to fines,
    // is carried off the map (exported). Spread over the platform, it would
    // build land in front of the cliff and shield it: a 30 m cliff yields far
    // more debris than the platform can hold below it.
    const double floor = platform - kBelowPlatformM;
    const auto sendOffshore = [&](int start, const Composition& fractions, double left) {
        queue.assign(1, start);
        stamp[start] = fill;
        for (std::size_t q = 0; q < queue.size() && q < kSpreadCells && left > 0.0; ++q) {
            const int at = queue[q];
            if (h[at] < floor) {
                const double thickness = std::min(left, floor - h[at]);
                Composition volumes;
                for (int k = 0; k < kSedimentKinds; ++k)
                    volumes[k] = fractions[k] * thickness;
                regolith.deposit(at, volumes);
                h[at] = regolith.b[at] + regolith.sed[at];
                left -= thickness;
            }
            const int ai = at % nx_, aj = at / nx_;
            for (int k = 0; k < 8; ++k) {
                const int ni = ai + kDi[k], nj = aj + kDj[k];
                if (ni < 0 || nj < 0 || ni >= nx_ || nj >= ny_)
                    continue;
                const int nb = nj * nx_ + ni;
                if (stamp[nb] != fill && sea[nb] && !fixed[nb]) {
                    stamp[nb] = fill;
                    queue.push_back(nb);
                }
            }
        }
        if (left > 1e-12)
            exported += left * cellArea;
        ++fill;
    };
    while (!ready.empty()) {
        const int c = ready.front();
        ready.pop_front();
        if (sea[c] || accumulator_[c] < cellVolume(h[c], s))
            continue;
        const double excess = accumulator_[c] - cellVolume(h[c], s);
        accumulator_[c] = 0.0;
        // The land behind that becomes the coast now (the cells already on the
        // coast, beside it, wear by their own exposure and hardness: passing them
        // the excess would even out the retreat of hard and soft rocks).
        const int ci = c % nx_, cj = c / nx_;
        std::vector<int> behind;
        for (int k = 0; k < 8; ++k) {
            const int ni = ci + kDi[k], nj = cj + kDj[k];
            if (ni < 0 || nj < 0 || ni >= nx_ || nj >= ny_)
                continue;
            const int nb = nj * nx_ + ni;
            if (!sea[nb] && !fixed[nb] && h[nb] >= s.seaLevel && !isCoast(h, sea, nb, s.seaLevel))
                behind.push_back(nb);
        }
        // Down to the platform: the whole cover, and the rock above the platform.
        const double removedVolume = std::max(0.0, h[c] - platform);
        const Composition debris = regolith.remove(c, removedVolume);
        destroyed += removedVolume * cellArea;
        h[c] = regolith.b[c] + regolith.sed[c];
        sea[c] = 1;
        const double cellExposure = exposure_[c];
        exposure_[c] = 0.0; // no longer a coast

        // Debris: beaches in sheltered spots, otherwise offshore.
        double left = removedVolume;
        const auto put = [&](int at, double thickness) {
            Composition volumes;
            for (int k = 0; k < kSedimentKinds; ++k)
                volumes[k] = debris[k] / removedVolume * thickness;
            regolith.deposit(at, volumes);
            h[at] = regolith.b[at] + regolith.sed[at];
            left -= thickness;
        };
        const bool exposed = s.swellEnergy * cellExposure > s.exportThreshold;
        if (!exposed && removedVolume > 0.0) {
            // Sea cells nearby, the most sheltered first (most land around).
            std::vector<std::pair<int, int>> spots;
            for (int nj = std::max(0, cj - kBeachRadius); nj <= std::min(ny_ - 1, cj + kBeachRadius); ++nj)
                for (int ni = std::max(0, ci - kBeachRadius); ni <= std::min(nx_ - 1, ci + kBeachRadius); ++ni) {
                    const int at = nj * nx_ + ni;
                    if (!sea[at] || fixed[at] || h[at] >= s.seaLevel + kBeachAboveSeaM)
                        continue;
                    int land = 0;
                    for (int y = std::max(0, nj - 2); y <= std::min(ny_ - 1, nj + 2); ++y)
                        for (int x = std::max(0, ni - 2); x <= std::min(nx_ - 1, ni + 2); ++x)
                            land += !sea[y * nx_ + x];
                    spots.push_back({-land, at});
                }
            std::sort(spots.begin(), spots.end());
            for (const auto& [shelter, at] : spots) {
                if (left <= 0.0)
                    break;
                put(at, std::min(left, s.seaLevel + kBeachAboveSeaM - h[at]));
            }
        }
        if (left > 1e-12) {
            Composition fractions;
            for (int k = 0; k < kSedimentKinds; ++k)
                fractions[k] = debris[k] / removedVolume;
            sendOffshore(c, fractions, left);
        }

        // The land behind becomes the coast and takes the excess wear.
        for (const int nb : behind) {
            newCoast.push_back(nb);
            accumulator_[nb] += excess / behind.size();
            if (accumulator_[nb] >= cellVolume(h[nb], s))
                ready.push_back(nb);
        }
    }
    // 3. Wave sweeping: in front of an exposed coast, the waves carry offshore
    //    the sediment above the abrasion platform (the scree of the cliff,
    //    debris), which would otherwise bury the platform and shield the
    //    cliff. Sheltered coasts keep their beaches.
    for (int c = 0; c < nx_ * ny_; ++c) {
        if (exposure_[c] <= 0.0 || s.swellEnergy * exposure_[c] <= s.exportThreshold || !isCoast(h, sea, c, s.seaLevel))
            continue;
        const int ci = c % nx_, cj = c / nx_;
        for (int nj = std::max(0, cj - kSweepRadius); nj <= std::min(ny_ - 1, cj + kSweepRadius); ++nj)
            for (int ni = std::max(0, ci - kSweepRadius); ni <= std::min(nx_ - 1, ci + kSweepRadius); ++ni) {
                const int at = nj * nx_ + ni;
                if (!sea[at] || fixed[at] || regolith.sed[at] <= 0.0 || h[at] <= platform)
                    continue;
                const double amount = std::min(regolith.sed[at], h[at] - platform);
                const Composition swept = regolith.remove(at, amount);
                h[at] = regolith.b[at] + regolith.sed[at];
                Composition fractions;
                for (int k = 0; k < kSedimentKinds; ++k)
                    fractions[k] = swept[k] / amount;
                sendOffshore(at, fractions, amount);
            }
    }
    exported_ = exported;
    // The new coast cells get their own exposure, once the whole step is done
    // (computed at once, a cell would still see its neighbours of the old coast
    // destroyed later in the step, and stay shielded until the next update).
    for (const int c : newCoast)
        if (isCoast(h, sea, c, s.seaLevel))
            exposure_[c] = rayExposure(h, c, s);
    return destroyed;
}

void CoastalErosion::normalizedAccumulator(const std::vector<double>& h, const CoastalSettings& s,
                                           std::vector<double>& out) const {
    out.assign(h.size(), 0.0);
    for (std::size_t c = 0; c < h.size(); ++c) {
        const double volume = cellVolume(h[c], s);
        if (accumulator_[c] > 0.0 && volume > 0.0)
            out[c] = std::min(1.0, accumulator_[c] / volume);
    }
}

} // namespace wg
