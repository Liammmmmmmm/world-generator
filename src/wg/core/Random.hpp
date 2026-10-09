#pragma once

#include <cstdint>
#include <string_view>

namespace wg {

// Everything here is fully specified so a seed gives the same world on every
// platform/compiler (unlike std:: distributions).

constexpr std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// FNV-1a, 64 bits.
constexpr std::uint64_t hashString(std::string_view text) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

// Independent sub-seed for a named consumer (e.g. a generation stage), so adding
// or reordering stages does not change the randomness of the others.
constexpr std::uint64_t deriveSeed(std::uint64_t seed, std::string_view tag) {
    std::uint64_t state = seed ^ hashString(tag);
    return splitmix64(state);
}

class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) : state_(seed) {}

    constexpr std::uint64_t nextU64() { return splitmix64(state_); }

    // Uniform in [0, 1).
    constexpr double nextDouble() { return static_cast<double>(nextU64() >> 11) * 0x1.0p-53; }

    // Uniform in [min, max] (inclusive).
    constexpr int nextInt(int min, int max) {
        const auto range = static_cast<std::uint64_t>(static_cast<std::int64_t>(max) - min + 1);
        return static_cast<int>(min + static_cast<std::int64_t>(nextU64() % range));
    }

private:
    std::uint64_t state_;
};

} // namespace wg
