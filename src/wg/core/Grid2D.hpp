#pragma once

#include <algorithm>
#include <cassert>
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

} // namespace wg
