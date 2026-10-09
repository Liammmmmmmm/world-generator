#include "wg/io/WorldFile.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>

namespace wg {

static_assert(std::endian::native == std::endian::little, "WorldFile assumes a little-endian host");

namespace {

constexpr std::array<char, 4> kMagic{'W', 'G', 'W', '\0'};
constexpr std::uint32_t kVersion = 2;

template <typename T>
void writePod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
T readPod(std::ifstream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return value;
}

template <typename T>
void writeSpan(std::ofstream& out, std::span<const T> data) {
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size_bytes()));
}

template <typename T>
void readSpan(std::ifstream& in, std::span<T> data) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size_bytes()));
}

} // namespace

void writeWorld(const WorldState& world, const std::filesystem::path& path) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw WorldFileError("cannot write '" + path.string() + "'");

    const WorldInfo& info = world.info();
    out.write(kMagic.data(), kMagic.size());
    writePod(out, kVersion);
    writePod<std::int32_t>(out, info.width);
    writePod<std::int32_t>(out, info.length);
    writePod<std::int32_t>(out, info.minY);
    writePod<std::int32_t>(out, info.maxY);
    writePod<std::int32_t>(out, info.seaLevel);
    writePod<std::uint64_t>(out, info.seed);

    const auto names = world.layerNames();
    writePod(out, static_cast<std::uint32_t>(names.size()));
    for (const auto& name : names) {
        writePod(out, static_cast<std::uint32_t>(name.size()));
        out.write(name.data(), static_cast<std::streamsize>(name.size()));
        writeSpan(out, world.layer(name).data());
    }

    writePod<std::uint8_t>(out, world.hasVoxels());
    if (world.hasVoxels()) {
        const VoxelVolume& voxels = world.voxels();
        for (int i = 0; i < voxels.sectionCount(); ++i) {
            const bool uniform = voxels.isSectionUniform(i);
            writePod<std::uint8_t>(out, uniform ? 0 : 1);
            if (uniform)
                writePod(out, voxels.sectionUniformBlock(i));
            else
                writeSpan(out, voxels.sectionBlocks(i));
        }
    }

    if (!out)
        throw WorldFileError("failed while writing '" + path.string() + "'");
}

WorldState readWorld(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw WorldFileError("cannot open '" + path.string() + "'");

    std::array<char, 4> magic{};
    in.read(magic.data(), magic.size());
    if (magic != kMagic)
        throw WorldFileError("'" + path.string() + "' is not a world file");
    if (const auto version = readPod<std::uint32_t>(in); version != kVersion)
        throw WorldFileError("unsupported world file version " + std::to_string(version));

    WorldInfo info;
    info.width = readPod<std::int32_t>(in);
    info.length = readPod<std::int32_t>(in);
    info.minY = readPod<std::int32_t>(in);
    info.maxY = readPod<std::int32_t>(in);
    info.seaLevel = readPod<std::int32_t>(in);
    info.seed = readPod<std::uint64_t>(in);
    if (!in || info.width <= 0 || info.length <= 0 || info.minY >= info.maxY)
        throw WorldFileError("corrupted header in '" + path.string() + "'");

    WorldState world(info);
    const auto layerCount = readPod<std::uint32_t>(in);
    for (std::uint32_t i = 0; i < layerCount && in; ++i) {
        std::string name(readPod<std::uint32_t>(in), '\0');
        in.read(name.data(), static_cast<std::streamsize>(name.size()));
        readSpan(in, world.createLayer(name).data());
    }

    if (readPod<std::uint8_t>(in)) {
        VoxelVolume& voxels = world.createVoxels();
        for (int i = 0; i < voxels.sectionCount() && in; ++i) {
            if (readPod<std::uint8_t>(in) == 0)
                voxels.setSectionUniform(i, readPod<Block>(in));
            else
                readSpan(in, voxels.editSectionBlocks(i));
        }
    }

    if (!in)
        throw WorldFileError("truncated world file '" + path.string() + "'");
    return world;
}

} // namespace wg
