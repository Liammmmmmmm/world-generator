#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace wg {

// Water routing on a regular grid (8 neighbours), cell index = j * nx + i.
//
//  1. Outlets: the given fixed cells, plus the ocean (cells below sea level
//     connected to the map border).
//  2. Priority-Flood + epsilon (Barnes, Lehman & Mulla 2014) from the outlets
//     fills the depressions: filled >= h, and every cell that is not an
//     outlet gets a strictly lower neighbour. The same flood also gives the
//     water surface without epsilon (flat lakes): lake depth = water - h.
//     (filled - h would add epsilon per cell across flats: centimetres over a
//     few hundred cells, enough to call a whole plateau a lake.)
//  3. Receivers: steepest descent on `filled` (L = dx or dx * sqrt 2). Away
//     from lakes filled == h: the true steepest slope. In a lake the epsilon
//     slope leads the water across it to its spill point.
//  4. Stack (Braun & Willett 2013): built from the receiver graph (donor lists
//     and a depth-first walk from each outlet), so every cell comes after its
//     receiver. (The Priority-Flood pop order does not guarantee that for
//     steepest-descent receivers.)
// Heights are doubles: epsilon (1e-4 m) is below float precision beyond 1 km.
class FlowRouter {
public:
    static constexpr double kEpsilon = 1e-4; // m: invisible, yet well above double precision

    FlowRouter(int nx, int ny, double cellM);

    void route(const std::vector<double>& h, const std::vector<std::uint8_t>& fixed, double seaLevel);

    // Q = source, plus everything that flows in from upstream (reverse stack).
    void accumulate(const std::vector<double>& source, std::vector<double>& out) const;

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    const std::vector<int>& receivers() const { return receiver_; }
    const std::vector<double>& receiverDistance() const { return distance_; }
    const std::vector<int>& stack() const { return stack_; }
    const std::vector<double>& filled() const { return filled_; }
    // Water surface: h, or the spill level of the depression (no epsilon).
    const std::vector<double>& water() const { return water_; }
    // Outlets: fixed cells and ocean (their receiver is themselves).
    const std::vector<std::uint8_t>& outlets() const { return outlet_; }
    const std::vector<std::uint8_t>& ocean() const { return ocean_; }

private:
    int nx_, ny_;
    double cellM_;
    int offset_[8];
    int di_[8], dj_[8];
    double length_[8];

    std::vector<double> filled_, water_;
    std::vector<std::uint8_t> ocean_, outlet_, closed_;
    std::vector<int> receiver_, stack_, donorStart_, donors_, work_;
    std::vector<double> distance_;
    std::vector<std::pair<double, int>> heap_; // min-heap of the Priority-Flood, reused
};

} // namespace wg
