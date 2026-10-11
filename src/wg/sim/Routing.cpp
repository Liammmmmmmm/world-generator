#include "wg/sim/Routing.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace wg {

FlowRouter::FlowRouter(int nx, int ny, double cellM) : nx_(nx), ny_(ny), cellM_(cellM) {
    int k = 0;
    for (int dj = -1; dj <= 1; ++dj) {
        for (int di = -1; di <= 1; ++di) {
            if (di == 0 && dj == 0)
                continue;
            di_[k] = di, dj_[k] = dj;
            offset_[k] = dj * nx + di;
            length_[k] = (di != 0 && dj != 0 ? std::sqrt(2.0) : 1.0) * cellM;
            ++k;
        }
    }
    const std::size_t n = static_cast<std::size_t>(nx) * ny;
    filled_.resize(n);
    water_.resize(n);
    ocean_.resize(n);
    outlet_.resize(n);
    closed_.resize(n);
    receiver_.resize(n);
    distance_.resize(n);
    stack_.reserve(n);
    donorStart_.resize(n + 1);
    donors_.resize(n);
    work_.reserve(n);
}

void FlowRouter::route(const std::vector<double>& h, const std::vector<std::uint8_t>& fixed, double seaLevel) {
    const int n = nx_ * ny_;
    const auto inside = [&](int i, int j) { return i >= 0 && j >= 0 && i < nx_ && j < ny_; };

    // 1. Ocean: below sea level and connected to the border (8-connected).
    std::fill(ocean_.begin(), ocean_.end(), 0);
    work_.clear();
    for (int j = 0; j < ny_; ++j)
        for (int i = 0; i < nx_; ++i)
            if ((i == 0 || j == 0 || i == nx_ - 1 || j == ny_ - 1) && h[j * nx_ + i] < seaLevel) {
                ocean_[j * nx_ + i] = 1;
                work_.push_back(j * nx_ + i);
            }
    while (!work_.empty()) {
        const int c = work_.back();
        work_.pop_back();
        const int ci = c % nx_, cj = c / nx_;
        for (int k = 0; k < 8; ++k) {
            const int vi = ci + di_[k], vj = cj + dj_[k];
            if (!inside(vi, vj))
                continue;
            const int v = c + offset_[k];
            if (!ocean_[v] && h[v] < seaLevel) {
                ocean_[v] = 1;
                work_.push_back(v);
            }
        }
    }

    // 2. Priority-Flood + epsilon, with Barnes' pit queue for depressions.
    // Only the outlets bordering other cells seed the flood: the open sea is
    // most of the map and its inner cells have nothing left to flood.
    std::vector<std::pair<double, int>>& open = heap_;
    open.clear();
    const auto push = [&](double height, int cell) {
        open.emplace_back(height, cell);
        std::push_heap(open.begin(), open.end(), std::greater<>());
    };
    for (int c = 0; c < n; ++c) {
        outlet_[c] = fixed[c] || ocean_[c];
        closed_[c] = outlet_[c];
        if (outlet_[c])
            filled_[c] = water_[c] = h[c];
    }
    for (int c = 0; c < n; ++c) {
        if (!outlet_[c])
            continue;
        const int ci = c % nx_, cj = c / nx_;
        for (int k = 0; k < 8; ++k) {
            const int vi = ci + di_[k], vj = cj + dj_[k];
            if (inside(vi, vj) && !outlet_[c + offset_[k]]) {
                push(h[c], c);
                break;
            }
        }
    }
    std::vector<int>& pit = work_; // FIFO over a vector
    pit.clear();
    std::size_t pitHead = 0;
    while (!open.empty() || pitHead < pit.size()) {
        int c;
        if (pitHead < pit.size()) {
            c = pit[pitHead++];
        } else {
            std::pop_heap(open.begin(), open.end(), std::greater<>());
            c = open.back().second;
            open.pop_back();
        }
        const int ci = c % nx_, cj = c / nx_;
        for (int k = 0; k < 8; ++k) {
            const int vi = ci + di_[k], vj = cj + dj_[k];
            if (!inside(vi, vj))
                continue;
            const int v = c + offset_[k];
            if (closed_[v])
                continue;
            closed_[v] = 1;
            const double rise = filled_[c] + kEpsilon;
            if (h[v] <= rise) { // in a depression: filled with the epsilon slope
                filled_[v] = rise;
                water_[v] = std::max(h[v], water_[c]); // the lake surface itself is flat
                pit.push_back(v);
            } else {
                filled_[v] = water_[v] = h[v];
                push(h[v], v);
            }
        }
        if (pitHead == pit.size())
            pit.clear(), pitHead = 0;
    }

    // 3. Receivers: steepest descent on the filled surface.
    for (int c = 0; c < n; ++c) {
        receiver_[c] = c;
        distance_[c] = cellM_;
        if (outlet_[c])
            continue;
        const int ci = c % nx_, cj = c / nx_;
        double best = 0.0;
        for (int k = 0; k < 8; ++k) {
            const int vi = ci + di_[k], vj = cj + dj_[k];
            if (!inside(vi, vj))
                continue;
            const int v = c + offset_[k];
            const double slope = (filled_[c] - filled_[v]) / length_[k];
            if (slope > best) {
                best = slope;
                receiver_[c] = v;
                distance_[c] = length_[k];
            }
        }
    }

    // 4. Stack: donors (counting sort), then a depth-first walk from each base level.
    std::fill(donorStart_.begin(), donorStart_.end(), 0);
    for (int c = 0; c < n; ++c)
        if (receiver_[c] != c)
            ++donorStart_[receiver_[c] + 1];
    for (int c = 0; c < n; ++c)
        donorStart_[c + 1] += donorStart_[c];
    std::vector<int> fill(donorStart_.begin(), donorStart_.end() - 1);
    for (int c = 0; c < n; ++c)
        if (receiver_[c] != c)
            donors_[fill[receiver_[c]]++] = c;

    stack_.clear();
    work_.clear();
    for (int base = 0; base < n; ++base) {
        if (receiver_[base] != base)
            continue;
        work_.push_back(base);
        while (!work_.empty()) {
            const int c = work_.back();
            work_.pop_back();
            stack_.push_back(c);
            for (int d = donorStart_[c]; d < donorStart_[c + 1]; ++d)
                work_.push_back(donors_[d]);
        }
    }
}

void FlowRouter::accumulate(const std::vector<double>& source, std::vector<double>& out) const {
    out = source;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        const int c = *it;
        if (receiver_[c] != c)
            out[receiver_[c]] += out[c];
    }
}

} // namespace wg
