#pragma once

#include <cstdint>
#include <vector>

namespace wg::erosion {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

// One cell of the erosion grid. Kept as an array of structures: a drop reads
// and writes most fields of the cell it is on at once (cache locality).
// Heights are in the normalized frame: 1 unit = erosion.particles.height_scale_m,
// measured from sea level.
struct ErosionCell {
    float height = 0.0f;          // ground: rock + sediment
    float sediment = 0.0f;        // thickness of loose sediment (part of height)
    float discharge = 0.0f;       // time-averaged water flow
    float dischargeTrack = 0.0f;  // accumulated during the current step
    float momentumX = 0.0f;       // time-averaged water momentum
    float momentumY = 0.0f;
    float momentumXTrack = 0.0f;  // accumulated during the current step
    float momentumYTrack = 0.0f;
};

// The erosion grid: one cell per cell of the island grid, 1 unit wide. FIXED
// cells (the map border) are never modified: what reaches them leaves the domain.
class ErosionMap {
public:
    ErosionMap(int nx, int ny)
        : nx_(nx), ny_(ny), cells_(static_cast<std::size_t>(nx) * ny), fixed_(cells_.size(), 0) {}

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    std::size_t size() const { return cells_.size(); }

    bool inside(int i, int j) const { return i >= 0 && j >= 0 && i < nx_ && j < ny_; }
    std::size_t index(int i, int j) const { return static_cast<std::size_t>(j) * nx_ + i; }

    ErosionCell& at(int i, int j) { return cells_[index(i, j)]; }
    const ErosionCell& at(int i, int j) const { return cells_[index(i, j)]; }
    ErosionCell& operator[](std::size_t k) { return cells_[k]; }
    const ErosionCell& operator[](std::size_t k) const { return cells_[k]; }

    bool fixed(int i, int j) const { return fixed_[index(i, j)] != 0; }
    void setFixed(int i, int j) { fixed_[index(i, j)] = 1; }

    // Height gradient (dh/dx, dh/dy) by centered differences, one-sided on the
    // grid border.
    Vec2 gradient(int i, int j) const {
        const int i0 = i > 0 ? i - 1 : i, i1 = i < nx_ - 1 ? i + 1 : i;
        const int j0 = j > 0 ? j - 1 : j, j1 = j < ny_ - 1 ? j + 1 : j;
        return {(at(i1, j).height - at(i0, j).height) / static_cast<float>(i1 - i0),
                (at(i, j1).height - at(i, j0).height) / static_cast<float>(j1 - j0)};
    }

private:
    int nx_, ny_;
    std::vector<ErosionCell> cells_;
    std::vector<std::uint8_t> fixed_;
};

} // namespace wg::erosion
