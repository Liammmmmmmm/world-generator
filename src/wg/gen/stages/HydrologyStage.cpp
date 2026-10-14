#include "wg/gen/stages/HydrologyStage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>
#include <string>
#include <vector>

#include "wg/core/Random.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

namespace {

// Rise of a flooded flat cell, and fall of a cell cut by a channel, from the
// cell that reached it (m): the conditioned surface always slopes to the sea.
constexpr float kEpsilonM = 1e-3f;
// Relative jitter of the slopes when choosing where a cell flows.
constexpr float kFlowJitter = 0.1f;

// D8 neighbors; the direction opposite to d is (d + 4) % 8.
constexpr int kDi[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDj[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr std::uint8_t kNone = 255;

// Cell states during the flood.
constexpr std::uint8_t kReached = 1;
constexpr std::uint8_t kFilled = 2; // raised in a depression too deep to cut: a lake candidate

constexpr float kInfinity = std::numeric_limits<float>::infinity();

} // namespace

void HydrologyStage::run(WorldState& world, const StageContext& context) {
    const auto& p = world.params().values.hydrology;
    Grid2D<float>& h = world.editLayer(layers::kHeight);
    const int nx = h.width(), ny = h.length();
    const std::size_t n = static_cast<std::size_t>(nx) * ny;
    const double cellM = context.info.widthM() / nx;
    const float sea = static_cast<float>(context.info.seaLevelM());

    Grid2D<float>& water = world.createLayer(layers::kWaterSurface, nx, ny, 0.0f, {LayerStyle::Height, {}});
    std::vector<std::string> kindNames(std::begin(layers::kWaterKindNames), std::end(layers::kWaterKindNames));
    Grid2D<float>& kind = world.createLayer(layers::kWater, nx, ny, layers::kDry, {LayerStyle::Categorical, kindNames});
    // The sea: the cells below sea level connected to the map border (the
    // ocean), not an inland hollow that happens to be below sea level.
    const auto markSea = [&] {
        std::vector<std::size_t> stack;
        const auto visit = [&](int i, int j) {
            const std::size_t k = static_cast<std::size_t>(j) * nx + i;
            if (h.data()[k] < sea && static_cast<int>(kind.data()[k]) != layers::kSea) {
                kind.data()[k] = layers::kSea;
                stack.push_back(k);
            }
        };
        for (int i = 0; i < nx; ++i)
            visit(i, 0), visit(i, ny - 1);
        for (int j = 0; j < ny; ++j)
            visit(0, j), visit(nx - 1, j);
        while (!stack.empty()) {
            const std::size_t k = stack.back();
            stack.pop_back();
            const int i = static_cast<int>(k % nx), j = static_cast<int>(k / nx);
            for (int d = 0; d < 8; ++d) // 8-connected, like the flow (cut channels go diagonally too)
                if (i + kDi[d] >= 0 && j + kDj[d] >= 0 && i + kDi[d] < nx && j + kDj[d] < ny)
                    visit(i + kDi[d], j + kDj[d]);
        }
    };
    markSea();
    const auto finish = [&] { // water surface: the sea level, a lake level (set), the ground
        for (std::size_t k = 0; k < n; ++k) {
            const auto type = static_cast<int>(kind.data()[k]);
            if (type == layers::kSea)
                water.data()[k] = sea;
            else if (type == layers::kDry)
                water.data()[k] = h.data()[k];
        }
    };
    if (!p.enabled) {
        finish();
        return;
    }
    const auto index = [&](int i, int j) { return static_cast<std::size_t>(j) * nx + i; };
    const auto neighbor = [&](std::size_t k, int d, std::size_t& m) {
        const int i = static_cast<int>(k % nx) + kDi[d], j = static_cast<int>(k / nx) + kDj[d];
        if (i < 0 || j < 0 || i >= nx || j >= ny)
            return false;
        m = index(i, j);
        return true;
    };

    // --- 1. Conditioning: Priority-Flood with hybrid breaching --------------
    std::vector<float> elev(h.data().begin(), h.data().end());
    std::vector<std::uint8_t> parent(n, kNone); // direction to the cell the flood came from
    std::vector<std::uint8_t> state(n, 0);
    using Entry = std::pair<float, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t k = index(i, j);
            if (static_cast<int>(kind.data()[k]) == layers::kSea || i == 0 || j == 0 || i == nx - 1 || j == ny - 1) {
                state[k] = kReached;
                queue.push({elev[k], static_cast<std::uint32_t>(k)});
            }
        }
    }
    const int maxCutCells = static_cast<int>(p.breachMaxLengthM / cellM);
    // Cuts a channel from `from` along the flood path down to a cell lower
    // than `level`, each cell kEpsilonM below the previous one, if it stays
    // within the limits and does not go through a lake. Returns whether it did.
    const auto breach = [&](std::size_t from, float level) {
        int cells = 0;
        float target = level;
        for (std::size_t k = from; elev[k] >= target;) {
            if ((state[k] & kFilled) || parent[k] == kNone)
                return false;
            target -= kEpsilonM;
            // Depth below the original ground (h, not yet cut), or successive
            // cuts would deepen a channel without limit.
            if (h.data()[k] - target > p.breachMaxDepthM || ++cells > maxCutCells)
                return false;
            neighbor(k, parent[k], k);
        }
        target = level;
        for (std::size_t k = from; elev[k] >= target;) {
            target -= kEpsilonM;
            elev[k] = target;
            neighbor(k, parent[k], k);
        }
        return true;
    };
    int breached = 0;
    while (!queue.empty()) {
        const std::size_t c = queue.top().second;
        queue.pop();
        for (int d = 0; d < 8; ++d) {
            std::size_t m;
            if (!neighbor(c, d, m) || state[m])
                continue;
            state[m] = kReached;
            parent[m] = static_cast<std::uint8_t>((d + 4) % 8);
            if (elev[m] < elev[c] - kEpsilonM) { // in a depression: cut through, or fill
                if (!(state[c] & kFilled) && breach(c, elev[m])) {
                    ++breached;
                } else {
                    elev[m] = elev[c] + kEpsilonM;
                    state[m] |= kFilled;
                }
            } else if (elev[m] <= elev[c]) { // flat: a tiny slope to the outlet
                elev[m] = elev[c] + kEpsilonM;
            }
            queue.push({elev[m], static_cast<std::uint32_t>(m)});
        }
    }
    context.progress(0.4f);

    // --- 2. Flow and drained area ---------------------------------------------
    // Receivers: the steepest lower neighbor, slopes jittered. Every cell
    // but the outlets has one (its parent is lower).
    std::vector<std::uint8_t>& receiver = parent; // reused
    const std::uint64_t seed = context.seed;
    for (std::size_t k = 0; k < n; ++k) {
        if (parent[k] == kNone)
            continue; // outlet
        float steepest = 0.0f;
        std::uint8_t best = parent[k];
        for (int d = 0; d < 8; ++d) {
            std::size_t m;
            if (!neighbor(k, d, m) || elev[m] >= elev[k])
                continue;
            std::uint64_t s = seed ^ (static_cast<std::uint64_t>(k) * 8 + static_cast<std::uint64_t>(d));
            const float noise = static_cast<float>(splitmix64(s) >> 40) / static_cast<float>(1 << 24); // [0, 1)
            const float slope = (elev[k] - elev[m]) / ((d % 2) ? std::numbers::sqrt2_v<float> : 1.0f) *
                                (1.0f + kFlowJitter * (2.0f * noise - 1.0f));
            if (slope > steepest)
                steepest = slope, best = static_cast<std::uint8_t>(d);
        }
        receiver[k] = best;
    }
    // Drained area, upstream first (topological order of the flow tree).
    const float cellArea = static_cast<float>(cellM * cellM);
    std::vector<float> area(n, cellArea);
    std::vector<std::uint8_t> donors(n, 0);
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t m;
        if (receiver[k] != kNone && neighbor(k, receiver[k], m))
            ++donors[m];
    }
    std::vector<std::uint32_t> ready;
    for (std::size_t k = 0; k < n; ++k)
        if (donors[k] == 0)
            ready.push_back(static_cast<std::uint32_t>(k));
    while (!ready.empty()) {
        const std::size_t k = ready.back();
        ready.pop_back();
        std::size_t m;
        if (receiver[k] == kNone || !neighbor(k, receiver[k], m))
            continue;
        area[m] += area[k];
        if (--donors[m] == 0)
            ready.push_back(static_cast<std::uint32_t>(m));
    }
    // Cut channels lower the ground; a hollow below sea level drained by a cut
    // to the ocean joins the sea (a small estuary).
    for (std::size_t k = 0; k < n; ++k)
        h.data()[k] = std::min(h.data()[k], elev[k]);
    markSea();
    context.progress(0.6f);

    // --- 3. Lakes: filled depressions, at the level of their lowest rim -------
    std::vector<char> seen(n, 0);
    int lakes = 0;
    for (std::size_t start = 0; start < n; ++start) {
        if (seen[start] || !(state[start] & kFilled))
            continue;
        std::vector<std::size_t> component{start}, stack{start};
        seen[start] = 1;
        float rim = kInfinity, deepest = 0.0f;
        while (!stack.empty()) {
            const std::size_t k = stack.back();
            stack.pop_back();
            for (int d = 0; d < 8; ++d) {
                std::size_t m;
                if (!neighbor(k, d, m))
                    continue;
                if (!(state[m] & kFilled)) {
                    rim = std::min(rim, h.data()[m]);
                } else if (!seen[m]) {
                    seen[m] = 1;
                    component.push_back(m);
                    stack.push_back(m);
                }
            }
        }
        std::size_t wet = 0;
        for (std::size_t k : component) {
            deepest = std::max(deepest, rim - h.data()[k]);
            wet += h.data()[k] < rim;
        }
        if (deepest < p.lakeMinDepthM || static_cast<double>(wet) * cellArea < p.lakeMinAreaM2) {
            // Too small for a lake: filled up to its spill level, as sediment
            // would, rather than a dry hollow (or a puddle below sea level).
            for (std::size_t k : component)
                h.data()[k] = std::max(h.data()[k], elev[k]);
            continue;
        }
        ++lakes;
        for (std::size_t k : component) {
            if (h.data()[k] < rim && static_cast<int>(kind.data()[k]) == layers::kDry) {
                kind.data()[k] = layers::kLake;
                water.data()[k] = rim;
            }
        }
    }
    context.progress(0.7f);

    // Dry ground left below sea level (a few cells along the shore, not
    // connected to the ocean) is raised to it: flush with the sea, not a hole.
    for (std::size_t k = 0; k < n; ++k)
        if (static_cast<int>(kind.data()[k]) == layers::kDry && h.data()[k] < sea)
            h.data()[k] = sea;

    // --- 4. Rivers: bands along the flow paths -------------------------------
    std::vector<float> level(n, kInfinity), depth(n, 0.0f);
    int sources = 0, riverCells = 0, bankCells = 0;
    float bankCut = 0.0f;
    for (std::size_t k = 0; k < n; ++k) {
        if (area[k] < p.riverMinAreaM2 || static_cast<int>(kind.data()[k]) != layers::kDry)
            continue;
        ++riverCells;
        const double width = std::max(p.riverMinWidthM, p.riverWidthCoeff * std::sqrt(area[k]));
        const double bed = std::max(1.0, p.riverDepthRatio * width); // in the middle
        const double radius = width / cellM / 2.0; // in cells
        const int reach = static_cast<int>(std::floor(radius));
        const int i = static_cast<int>(k % nx), j = static_cast<int>(k / nx);
        for (int dj = -reach; dj <= reach; ++dj) {
            for (int di = -reach; di <= reach; ++di) {
                const double r = std::hypot(di, dj);
                if (r > std::max(radius, 0.0))
                    continue;
                const int bi = i + di, bj = j + dj;
                if (bi < 0 || bj < 0 || bi >= nx || bj >= ny)
                    continue;
                const std::size_t b = index(bi, bj);
                if (static_cast<int>(kind.data()[b]) != layers::kDry)
                    continue; // the band stops at the sea and the lakes (their own water)
                level[b] = std::min(level[b], elev[k]);
                // A hollow cross-section: deepest in the middle, shallower
                // towards the banks, at least 1 m everywhere.
                const double t = r / (radius + 1.0);
                depth[b] = std::max(depth[b], static_cast<float>(std::max(1.0, bed * (1.0 - t * t))));
            }
        }
        // A source: no upstream river cell flows into this one.
        bool fed = false;
        for (int d = 0; d < 8 && !fed; ++d) {
            std::size_t m;
            fed = neighbor(k, d, m) && area[m] >= p.riverMinAreaM2 && receiver[m] == (d + 4) % 8;
        }
        sources += !fed;
    }
    const auto isRiver = [&](std::size_t b) {
        return level[b] != kInfinity && static_cast<int>(kind.data()[b]) == layers::kDry;
    };
    // The surface never rises above a neighboring dry ground, nor above the
    // sea it flows into (whose top block is just below sea level).
    const auto capSurfaces = [&] {
        for (std::size_t b = 0; b < n; ++b) {
            if (!isRiver(b))
                continue;
            for (int d = 0; d < 8; ++d) {
                std::size_t m;
                if (!neighbor(b, d, m) || level[m] != kInfinity)
                    continue;
                const auto type = static_cast<int>(kind.data()[m]);
                if (type == layers::kDry)
                    level[b] = std::min(level[b], h.data()[m]);
                else if (type == layers::kSea)
                    level[b] = std::min(level[b], sea);
            }
        }
    };
    capSurfaces();
    // Banks: the dry ground beside a river rises at most river_bank_slope_deg
    // from its water, so a river crossing a slope lies in a small valley, not
    // at the bottom of a trench. A bank is lowered from the river outwards
    // (Dijkstra on the allowed height), never below the sea or a lake beside it.
    // Only the foot of the banks is softened: up to river_bank_height_m above
    // the water, and by that much at most. A higher wall is a real cliff (a
    // gorge), kept as it is: the softening stops there, and a steep hillside
    // above a river keeps its relief.
    {
        const float rise = static_cast<float>(std::tan(p.riverBankSlopeDeg * std::numbers::pi / 180.0) * cellM);
        std::vector<float> allowed(n, kInfinity), source(n, kInfinity); // source: the water level it is graded from
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> banks;
        for (std::size_t b = 0; b < n; ++b)
            if (isRiver(b)) {
                source[b] = level[b];
                banks.push({level[b], static_cast<std::uint32_t>(b)});
            }
        while (!banks.empty()) {
            const auto [top, c] = banks.top();
            banks.pop();
            if (level[c] == kInfinity && top > allowed[c])
                continue; // superseded
            for (int d = 0; d < 8; ++d) {
                std::size_t m;
                if (!neighbor(c, d, m) || level[m] != kInfinity || static_cast<int>(kind.data()[m]) != layers::kDry)
                    continue;
                const float next = top + rise * ((d % 2) ? std::numbers::sqrt2_v<float> : 1.0f);
                if (next >= h.data()[m] || next >= allowed[m])
                    continue; // already low enough: the bank ends here
                if (next > source[c] + p.riverBankHeightM || h.data()[m] - next > p.riverBankHeightM)
                    continue; // above the foot of the bank, or a cliff
                allowed[m] = next;
                source[m] = source[c];
                banks.push({next, static_cast<std::uint32_t>(m)});
            }
        }
        for (std::size_t m = 0; m < n; ++m) {
            if (allowed[m] == kInfinity)
                continue;
            float floor = sea;
            for (int d = 0; d < 8; ++d) {
                std::size_t l;
                if (neighbor(m, d, l) && static_cast<int>(kind.data()[l]) == layers::kLake)
                    floor = std::max(floor, water.data()[l]);
            }
            const float lowered = std::min(h.data()[m], std::max(allowed[m], floor));
            bankCut = std::max(bankCut, h.data()[m] - lowered);
            bankCells += lowered < h.data()[m];
            h.data()[m] = lowered;
        }
    }
    capSurfaces(); // against the lowered banks (a step down where a river falls)
    for (std::size_t b = 0; b < n; ++b) {
        if (!isRiver(b))
            continue;
        h.data()[b] = std::min(h.data()[b], level[b] - depth[b]); // the bed
        water.data()[b] = level[b];
        kind.data()[b] = layers::kRiver;
    }
    finish();
    context.log(std::to_string(sources) + " river sources, " + std::to_string(riverCells) + " river path cells, " +
                std::to_string(bankCells) + " bank cells lowered (at most " + std::to_string(bankCut) + " m), " +
                std::to_string(lakes) + " lakes, " + std::to_string(breached) + " depressions drained by a cut");
    context.progress(1.0f);
}

} // namespace wg
