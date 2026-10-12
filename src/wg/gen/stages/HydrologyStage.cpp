#include "wg/gen/stages/HydrologyStage.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <queue>
#include <string>
#include <vector>

#include "wg/core/WorldState.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

namespace {

// Rise of the filled surface from a cell to the next one flooded from it, so
// that filled depressions still slope towards their outlet (m).
constexpr double kFillSlopeM = 1e-4;

} // namespace

void HydrologyStage::run(WorldState& world, const StageContext& context) {
    const auto& p = world.params().values.hydrology;
    const Grid2D<float>& h = world.layer(layers::kHeight);
    const Grid2D<std::uint8_t>& flags = world.byteLayer(layers::kFlags);
    const int nx = h.width(), ny = h.length();
    const std::size_t n = static_cast<std::size_t>(nx) * ny;
    const double cellM = context.info.widthM() / nx;
    const double cellArea = cellM * cellM;
    const double sea = context.info.seaLevelM();

    Grid2D<float>& surface = world.createLayer(layers::kWaterSurface, nx, ny, 0.0f, {LayerStyle::Height, {}});
    Grid2D<float>& lakeDepth = world.createLayer(layers::kLakeDepth, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& drainage = world.createLayer(layers::kDrainage, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& riverWidth = world.createLayer(layers::kRiverWidth, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& riverDepth = world.createLayer(layers::kRiverDepth, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<std::uint8_t>& direction = world.createByteLayer(layers::kFlowDirection, nx, ny, layers::kNoFlow);
    if (!p.enabled) {
        surface = h;
        return;
    }
    const auto index = [&](int i, int j) { return static_cast<std::size_t>(j) * nx + i; };

    // --- 1. Priority-Flood: from the outlets (the sea and the map border)
    // inwards, lowest first; a cell flooded from a lower one is raised to it
    // (plus a tiny slope). `order` lists the cells from the lowest surface up.
    std::vector<double> filled(n);
    std::vector<char> outlet(n, 0), done(n, 0);
    std::vector<std::size_t> order;
    order.reserve(n);
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t k = index(i, j);
            if (h.at(i, j) < sea || (flags.at(i, j) & layers::kFixed)) {
                outlet[k] = done[k] = 1;
                filled[k] = h.at(i, j);
                queue.push({filled[k], k});
            }
        }
    }
    while (!queue.empty()) {
        const std::size_t k = queue.top().second;
        queue.pop();
        order.push_back(k);
        const int i = static_cast<int>(k % nx), j = static_cast<int>(k / nx);
        for (int d = 0; d < 8; ++d) {
            const int ni = i + layers::kFlowDi[d], nj = j + layers::kFlowDj[d];
            if (ni < 0 || nj < 0 || ni >= nx || nj >= ny || done[index(ni, nj)])
                continue;
            const std::size_t m = index(ni, nj);
            done[m] = 1;
            filled[m] = std::max(static_cast<double>(h.at(ni, nj)), filled[k] + kFillSlopeM);
            queue.push({filled[m], m});
        }
    }
    context.progress(0.4f);

    // --- 2. Flow directions: steepest descent on the filled surface (D8,
    // slopes divided by the distance). The filling guarantees a lower
    // neighbor to every cell that is not an outlet.
    std::vector<std::int64_t> receiver(n, -1);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t k = index(i, j);
            if (outlet[k])
                continue;
            double steepest = 0.0;
            for (int d = 0; d < 8; ++d) {
                const int ni = i + layers::kFlowDi[d], nj = j + layers::kFlowDj[d];
                if (ni < 0 || nj < 0 || ni >= nx || nj >= ny)
                    continue;
                const double slope = (filled[k] - filled[index(ni, nj)]) / ((d % 2) ? std::numbers::sqrt2 : 1.0);
                if (slope > steepest) {
                    steepest = slope;
                    receiver[k] = static_cast<std::int64_t>(index(ni, nj));
                    direction.at(i, j) = static_cast<std::uint8_t>(d);
                }
            }
        }
    }

    // --- 3. Drained area, from the highest cells down (receivers are lower).
    std::vector<double> area(n, cellArea);
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (receiver[*it] >= 0)
            area[static_cast<std::size_t>(receiver[*it])] += area[*it];
    context.progress(0.6f);

    // --- 4. Lakes: connected cells under at least lake_min_depth_m of water,
    // covering at least lake_min_area_m2.
    std::vector<char> seen(n, 0);
    int lakes = 0;
    for (std::size_t start = 0; start < n; ++start) {
        if (seen[start] || outlet[start] || filled[start] - h.data()[start] < p.lakeMinDepthM)
            continue;
        std::vector<std::size_t> component{start}, stack{start};
        seen[start] = 1;
        while (!stack.empty()) {
            const std::size_t k = stack.back();
            stack.pop_back();
            const int i = static_cast<int>(k % nx), j = static_cast<int>(k / nx);
            for (int d = 0; d < 8; d += 2) { // 4-connected
                const int ni = i + layers::kFlowDi[d], nj = j + layers::kFlowDj[d];
                if (ni < 0 || nj < 0 || ni >= nx || nj >= ny)
                    continue;
                const std::size_t m = index(ni, nj);
                if (seen[m] || outlet[m] || filled[m] - h.data()[m] < p.lakeMinDepthM)
                    continue;
                seen[m] = 1;
                component.push_back(m);
                stack.push_back(m);
            }
        }
        if (static_cast<double>(component.size()) * cellArea < p.lakeMinAreaM2)
            continue;
        ++lakes;
        for (std::size_t k : component)
            lakeDepth.data()[k] = static_cast<float>(filled[k] - h.data()[k]);
    }

    // --- 5. Rivers, from their source down to the sea.
    int riverCells = 0;
    for (std::size_t k = 0; k < n; ++k) {
        surface.data()[k] = static_cast<float>(filled[k]);
        drainage.data()[k] = static_cast<float>(std::log10(area[k]));
        if (outlet[k] || area[k] < p.riverMinAreaM2)
            continue;
        const double width = std::max(p.riverMinWidthM, p.riverWidthCoeff * std::sqrt(area[k]));
        riverWidth.data()[k] = static_cast<float>(width);
        riverDepth.data()[k] = static_cast<float>(std::max(1.0, p.riverDepthRatio * width));
        ++riverCells;
    }
    context.log(std::to_string(lakes) + " lakes, " + std::to_string(riverCells) + " river cells");
    context.progress(1.0f);
}

} // namespace wg
