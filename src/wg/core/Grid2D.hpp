#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace wg {

// Dense 2D grid indexed by (x, z), stored row-major (z rows of x).
template <typename T>
class Grid2D {
public:
    Grid2D() = default;
    Grid2D(int width, int length, const T& fill = T{})
        : width_(width), length_(length), data_(static_cast<std::size_t>(width) * length, fill) {}

    int width() const { return width_; }
    int length() const { return length_; }
    std::size_t size() const { return data_.size(); }

    bool contains(int x, int z) const { return x >= 0 && z >= 0 && x < width_ && z < length_; }

    T& at(int x, int z) { return data_[index(x, z)]; }
    const T& at(int x, int z) const { return data_[index(x, z)]; }

    // Clamps coordinates to the grid borders; handy for neighbourhood lookups.
    const T& atClamped(int x, int z) const {
        x = x < 0 ? 0 : (x >= width_ ? width_ - 1 : x);
        z = z < 0 ? 0 : (z >= length_ ? length_ - 1 : z);
        return data_[index(x, z)];
    }

    void fill(const T& value) { std::fill(data_.begin(), data_.end(), value); }

    std::span<T> data() { return data_; }
    std::span<const T> data() const { return data_; }

private:
    std::size_t index(int x, int z) const {
        assert(contains(x, z));
        return static_cast<std::size_t>(z) * width_ + x;
    }

    int width_ = 0;
    int length_ = 0;
    std::vector<T> data_;
};

// Bilinear sample of a grid covering a widthM x lengthM rectangle centered on
// the origin (cell centers at the middle of each cell), at (x, y) meters.
inline float sampleCentered(const Grid2D<float>& grid, double x, double y, double widthM, double lengthM) {
    const double fx = (x / widthM + 0.5) * grid.width() - 0.5;
    const double fy = (y / lengthM + 0.5) * grid.length() - 0.5;
    const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
    const auto tx = static_cast<float>(fx - i), ty = static_cast<float>(fy - j);
    const float top = grid.atClamped(i, j) * (1 - tx) + grid.atClamped(i + 1, j) * tx;
    const float bottom = grid.atClamped(i, j + 1) * (1 - tx) + grid.atClamped(i + 1, j + 1) * tx;
    return top * (1 - ty) + bottom * ty;
}

// Bicubic (Catmull-Rom) sample, same conventions as sampleCentered: smooth
// (continuous slopes) and through the cell values, for upscaling heightmaps.
inline float sampleCubic(const Grid2D<float>& grid, double x, double y, double widthM, double lengthM) {
    const double fx = (x / widthM + 0.5) * grid.width() - 0.5;
    const double fy = (y / lengthM + 0.5) * grid.length() - 0.5;
    const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
    const double tx = fx - i, ty = fy - j;
    const auto weights = [](double t, double w[4]) {
        const double t2 = t * t, t3 = t2 * t;
        w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
        w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
        w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
        w[3] = 0.5 * (t3 - t2);
    };
    double wx[4], wy[4];
    weights(tx, wx);
    weights(ty, wy);
    double sum = 0.0;
    for (int b = 0; b < 4; ++b) {
        double row = 0.0;
        for (int a = 0; a < 4; ++a)
            row += wx[a] * grid.atClamped(i - 1 + a, j - 1 + b);
        sum += wy[b] * row;
    }
    return static_cast<float>(sum);
}

} // namespace wg
