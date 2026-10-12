#include "wg/erosion/Avalanche.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wg::erosion {

namespace {

// Cells checked per avalanche, at most: the modified cell, then the cells it
// sends matter to, in order (breadth first). Enough for debris to slide down
// to rest, and bounded: a cascade on a slope steeper than every talus would
// otherwise spread over the whole slope at each change.
constexpr int kMaxCells = 24;

struct Neighbor {
    int di, dj;
    float distance;
};
constexpr Neighbor kNeighbors[] = {
    {-1, -1, std::numbers::sqrt2_v<float>}, {0, -1, 1.0f}, {1, -1, std::numbers::sqrt2_v<float>},
    {-1, 0, 1.0f},                                         {1, 0, 1.0f},
    {-1, 1, std::numbers::sqrt2_v<float>},  {0, 1, 1.0f},  {1, 1, std::numbers::sqrt2_v<float>},
};

// Checks cell (i, j) against its 8 neighbors, moving the unstable part of
// each height difference down. Calls `received(ni, nj)` for each neighbor that
// received matter.
template <typename Received>
void relax(ErosionMap& map, const Rock& rock, float rate, int i, int j, Received&& received) {
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
        if (down)
            received(ni, nj);
    }
}

} // namespace

void avalanche(ErosionMap& map, const Rock& rock, float rate, int i, int j) {
    if (map.fixed(i, j))
        return;
    struct Cell {
        int i, j;
    };
    Cell queue[kMaxCells];
    int size = 0;
    queue[size++] = {i, j};
    for (int next = 0; next < size; ++next) {
        relax(map, rock, rate, queue[next].i, queue[next].j, [&](int ni, int nj) {
            if (size == kMaxCells)
                return;
            for (int k = next + 1; k < size; ++k)
                if (queue[k].i == ni && queue[k].j == nj)
                    return; // already waiting
            queue[size++] = {ni, nj};
        });
    }
}

} // namespace wg::erosion
