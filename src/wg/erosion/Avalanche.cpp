#include "wg/erosion/Avalanche.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wg::erosion {

namespace {

// Levels of cascade below the modified cell. The references do not cascade;
// a couple of levels keep a collapse from leaving a step at its foot.
constexpr int kMaxDepth = 2;

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
        const float excess = std::abs(diff) - rock.talus(hi, hj, high) * n.distance;
        if (excess <= 0.0f)
            continue;

        const float transfer = rate * excess / 2.0f;
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
