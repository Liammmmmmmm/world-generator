#include "wg/erosion/Avalanche.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wg::erosion {

namespace {

// Levels of cascade below the modified cell. The references do not cascade;
// a few levels let debris slide down to rest instead of perching on a slope.
constexpr int kMaxDepth = 6;

struct Neighbor {
    int di, dj;
    float distance;
};
constexpr Neighbor kNeighbors[] = {
    {-1, -1, std::numbers::sqrt2_v<float>}, {0, -1, 1.0f}, {1, -1, std::numbers::sqrt2_v<float>},
    {-1, 0, 1.0f},                                         {1, 0, 1.0f},
    {-1, 1, std::numbers::sqrt2_v<float>},  {0, 1, 1.0f},  {1, 1, std::numbers::sqrt2_v<float>},
};

void cascade(ErosionMap& map, const Rock& rock, float rate, int i, int j, int depth) {
    for (const Neighbor& n : kNeighbors) {
        const int ni = i + n.di, nj = j + n.dj;
        if (!map.inside(ni, nj) || map.fixed(ni, nj))
            continue;
        ErosionCell& cell = map.at(i, j);
        ErosionCell& other = map.at(ni, nj);
        const float diff = cell.height - other.height;
        if (std::abs(diff) <= rock.minTalus() * n.distance)
            continue; // stable whatever the materials (cheap early out)

        const bool down = diff > 0.0f; // from (i, j) to the neighbor
        const int hi = down ? i : ni, hj = down ? j : nj;
        ErosionCell& high = down ? cell : other;
        ErosionCell& low = down ? other : cell;
        float transfer = 0.0f;
        if (high.sediment > 0.0f) // loose material, at most what there is
            transfer = std::min(high.sediment,
                                rate * std::max(0.0f, std::abs(diff) - rock.sedimentTalus(high) * n.distance) / 2.0f);
        // The rock itself gives way, above the sea only (no erosion under water).
        const float rockExcess =
            high.height - high.sediment >= 0.0f ? std::abs(diff) - rock.rockTalus(hi, hj, high) * n.distance : 0.0f;
        if (rockExcess > 0.0f)
            transfer = std::max(transfer, rate * rockExcess / 2.0f);
        if (transfer <= 0.0f)
            continue;

        high.height -= transfer; // loose sediment goes first, then rock
        high.sediment = std::max(0.0f, high.sediment - transfer);
        low.height += transfer;
        low.sediment += transfer;
        if (down && depth < kMaxDepth)
            cascade(map, rock, rate, ni, nj, depth + 1);
    }
}

} // namespace

void avalanche(ErosionMap& map, const Rock& rock, float rate, int i, int j) {
    if (!map.fixed(i, j))
        cascade(map, rock, rate, i, j, 0);
}

} // namespace wg::erosion
