#include "wg/erosion/Lakes.hpp"

#include <algorithm>
#include <deque>
#include <queue>
#include <utility>
#include <vector>

namespace wg::erosion {

namespace {

constexpr int kDi[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDj[8] = {0, 1, 1, 1, 0, -1, -1, -1};

} // namespace

int updateLakes(ErosionMap& map, float minDepth) {
    const int nx = map.nx(), ny = map.ny();
    const std::size_t n = map.size();
    std::vector<float>& water = map.water();
    std::fill(water.begin(), water.end(), 0.0f);

    // Spill level of every land cell: Priority-Flood (Barnes 2014, with a
    // plain queue for the cells inside a depression) from the outlets: the
    // sea (height <= 0) and the map border.
    std::vector<float> level(n);
    std::vector<char> reached(n, 0);
    using Entry = std::pair<float, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    std::deque<std::uint32_t> pit;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t k = map.index(i, j);
            level[k] = map[k].height;
            if (map[k].height <= 0.0f || map.fixed(i, j))
                reached[k] = 1;
        }
    }
    for (int j = 0; j < ny; ++j) { // seeds: the outlets next to the land
        for (int i = 0; i < nx; ++i) {
            const std::size_t k = map.index(i, j);
            if (!reached[k])
                continue;
            for (int d = 0; d < 8; ++d) {
                const int a = i + kDi[d], b = j + kDj[d];
                if (map.inside(a, b) && !reached[map.index(a, b)]) {
                    queue.push({std::max(level[k], 0.0f), static_cast<std::uint32_t>(k)});
                    break;
                }
            }
        }
    }
    while (!queue.empty() || !pit.empty()) {
        std::size_t c;
        if (!pit.empty()) {
            c = pit.front();
            pit.pop_front();
        } else {
            c = queue.top().second;
            queue.pop();
        }
        const float top = std::max(level[c], 0.0f);
        const int i = static_cast<int>(c % nx), j = static_cast<int>(c / nx);
        for (int d = 0; d < 8; ++d) {
            const int a = i + kDi[d], b = j + kDj[d];
            if (!map.inside(a, b))
                continue;
            const std::size_t m = map.index(a, b);
            if (reached[m])
                continue;
            reached[m] = 1;
            if (level[m] <= top) {
                level[m] = top;
                pit.push_back(static_cast<std::uint32_t>(m));
            } else {
                queue.push({level[m], static_cast<std::uint32_t>(m)});
            }
        }
    }

    // Lakes: the connected depressions deep enough.
    int lakes = 0;
    std::vector<char> seen(n, 0);
    std::vector<std::uint32_t> component, stack;
    for (std::size_t start = 0; start < n; ++start) {
        if (seen[start] || level[start] <= map[start].height)
            continue;
        component.assign(1, static_cast<std::uint32_t>(start));
        stack.assign(1, static_cast<std::uint32_t>(start));
        seen[start] = 1;
        float deepest = 0.0f;
        while (!stack.empty()) {
            const std::size_t k = stack.back();
            stack.pop_back();
            deepest = std::max(deepest, level[k] - map[k].height);
            const int i = static_cast<int>(k % nx), j = static_cast<int>(k / nx);
            for (int d = 0; d < 8; ++d) {
                const int a = i + kDi[d], b = j + kDj[d];
                if (!map.inside(a, b))
                    continue;
                const std::size_t m = map.index(a, b);
                if (seen[m] || level[m] <= map[m].height)
                    continue;
                seen[m] = 1;
                component.push_back(static_cast<std::uint32_t>(m));
                stack.push_back(static_cast<std::uint32_t>(m));
            }
        }
        if (deepest < minDepth)
            continue;
        ++lakes;
        for (std::size_t k : component)
            water[k] = level[k];
    }
    return lakes;
}

} // namespace wg::erosion
