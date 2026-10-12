#include "wg/io/MinecraftExport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <zlib.h>

#include "wg/io/Nbt.hpp"

namespace wg {

namespace {

constexpr int kSection = VoxelVolume::kSectionSize;
constexpr int kRegionChunks = 32; // chunks per region side
constexpr int kSectorBytes = 4096;
// The overworld of Minecraft 1.18+: sections -4..19 (y -64..319).
constexpr int kMinSectionY = -4;
constexpr int kMaxSectionY = 19;

std::string_view minecraftName(Block block) {
    switch (block) {
    case Block::Air: return "minecraft:air";
    case Block::Bedrock: return "minecraft:bedrock";
    case Block::Stone: return "minecraft:stone";
    case Block::Dirt: return "minecraft:dirt";
    case Block::Grass: return "minecraft:grass_block";
    case Block::Sand: return "minecraft:sand";
    case Block::Gravel: return "minecraft:gravel";
    case Block::Snow: return "minecraft:snow_block";
    case Block::Water: return "minecraft:water";
    case Block::River: return "minecraft:blue_wool";
    default: return "minecraft:air";
    }
}

int floorDiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

std::vector<std::uint8_t> zlibCompress(const std::vector<std::uint8_t>& data, bool gzip) {
    z_stream stream{};
    // windowBits 15 (+16: gzip header, for level.dat), fast compression.
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, gzip ? 15 + 16 : 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw MinecraftExportError("zlib initialization failed");
    std::vector<std::uint8_t> out(deflateBound(&stream, static_cast<uLong>(data.size())));
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        throw MinecraftExportError("zlib compression failed");
    out.resize(stream.total_out);
    return out;
}

// Geometry shared by all the chunks.
struct Layout {
    int sectionsX, sectionsY;
    int ourMinSectionY;     // Minecraft section Y of our first section
    int originChunkX, originChunkZ; // Minecraft chunk of our section (0, 0)
};

// One chunk (16 x world height x 16) as uncompressed NBT.
std::vector<std::uint8_t> chunkNbt(const VoxelVolume& voxels, const Layout& layout, int sx, int sz,
                                   const MinecraftExportOptions& options, int& dropped) {
    nbt::Writer w;
    w.beginCompound("");
    w.intTag("DataVersion", options.dataVersion);
    w.intTag("xPos", layout.originChunkX + sx);
    w.intTag("zPos", layout.originChunkZ + sz);
    w.intTag("yPos", kMinSectionY);
    w.stringTag("Status", "minecraft:full");
    w.longTag("LastUpdate", 0);
    w.longTag("InhabitedTime", 0);
    w.byteTag("isLightOn", 0); // Minecraft computes the light when loading the chunk

    // Our sections that fall in the overworld.
    std::vector<int> sections;
    for (int sy = 0; sy < layout.sectionsY; ++sy) {
        const int y = layout.ourMinSectionY + sy;
        if (y < kMinSectionY || y > kMaxSectionY)
            ++dropped;
        else
            sections.push_back(sy);
    }
    w.beginList("sections", nbt::Tag::Compound, static_cast<std::int32_t>(sections.size()));
    std::vector<std::int64_t> packed;
    for (int sy : sections) {
        const int index = (sz * layout.sectionsX + sx) * layout.sectionsY + sy; // see VoxelVolume::slotIndex
        w.beginCompoundPayload();
        w.byteTag("Y", static_cast<std::int8_t>(layout.ourMinSectionY + sy));
        w.beginCompound("block_states");
        if (voxels.isSectionUniform(index)) {
            w.beginList("palette", nbt::Tag::Compound, 1);
            w.beginCompoundPayload();
            w.stringTag("Name", minecraftName(voxels.sectionUniformBlock(index)));
            w.endCompound();
        } else {
            // Palette in order of first appearance; indices packed in longs,
            // max(4, bits) per entry, entries not spanning two longs. Both
            // index orders are (y, z, x), x fastest.
            const auto blocks = voxels.sectionBlocks(index);
            std::array<int, static_cast<std::size_t>(Block::Count)> paletteIndex;
            paletteIndex.fill(-1);
            std::vector<Block> palette;
            for (Block b : blocks) {
                if (paletteIndex[static_cast<std::size_t>(b)] < 0) {
                    paletteIndex[static_cast<std::size_t>(b)] = static_cast<int>(palette.size());
                    palette.push_back(b);
                }
            }
            w.beginList("palette", nbt::Tag::Compound, static_cast<std::int32_t>(palette.size()));
            for (Block b : palette) {
                w.beginCompoundPayload();
                w.stringTag("Name", minecraftName(b));
                w.endCompound();
            }
            if (palette.size() > 1) {
                const int bits = std::max(4, static_cast<int>(std::bit_width(palette.size() - 1)));
                const int perLong = 64 / bits;
                packed.assign((blocks.size() + perLong - 1) / perLong, 0);
                for (std::size_t k = 0; k < blocks.size(); ++k) {
                    const auto value = static_cast<std::uint64_t>(paletteIndex[static_cast<std::size_t>(blocks[k])]);
                    const auto word = static_cast<std::uint64_t>(packed[k / perLong]) | value << ((k % perLong) * bits);
                    packed[k / perLong] = static_cast<std::int64_t>(word);
                }
                w.longArrayTag("data", packed);
            }
        }
        w.endCompound(); // block_states
        w.beginCompound("biomes");
        w.beginList("palette", nbt::Tag::String, 1);
        w.stringPayload("minecraft:plains");
        w.endCompound();
        w.endCompound(); // section
    }
    w.beginList("block_entities", nbt::Tag::Compound, 0);
    w.endCompound(); // root
    return w.bytes();
}

void writeRegion(const std::filesystem::path& path, const std::vector<std::vector<std::uint8_t>>& chunks) {
    // Header: 1024 locations (sector offset << 8 | sector count) and 1024
    // timestamps; each chunk: length (+1), compression type 2 (zlib), data,
    // padded to whole sectors.
    std::vector<std::uint8_t> file(2 * kSectorBytes, 0);
    const auto timestamp = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (int b = 0; b < 4; ++b)
            file[at + b] = static_cast<std::uint8_t>(value >> (24 - 8 * b));
    };
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        if (chunks[k].empty())
            continue;
        const std::size_t offset = file.size() / kSectorBytes;
        const std::size_t length = chunks[k].size() + 1;
        const std::size_t sectors = (length + 4 + kSectorBytes - 1) / kSectorBytes;
        if (sectors > 255)
            throw MinecraftExportError("a chunk is larger than 1 MB once compressed");
        put32(4 * k, static_cast<std::uint32_t>(offset << 8 | sectors));
        put32(kSectorBytes + 4 * k, timestamp);
        file.resize(file.size() + sectors * kSectorBytes, 0);
        const std::size_t at = offset * kSectorBytes;
        put32(at, static_cast<std::uint32_t>(length));
        file[at + 4] = 2;
        std::copy(chunks[k].begin(), chunks[k].end(), file.begin() + static_cast<std::ptrdiff_t>(at + 5));
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out)
        throw MinecraftExportError("cannot write " + path.string());
}

void writeLevelDat(const std::filesystem::path& path, const MinecraftExportOptions& options, std::uint64_t seed,
                   int spawnX, int spawnY, int spawnZ) {
    nbt::Writer w;
    w.beginCompound("");
    w.beginCompound("Data");
    w.intTag("DataVersion", options.dataVersion);
    w.beginCompound("Version");
    w.intTag("Id", options.dataVersion);
    w.stringTag("Name", options.versionName);
    w.stringTag("Series", "main");
    w.byteTag("Snapshot", 0);
    w.endCompound();
    w.intTag("version", 19133); // NBT level format (Anvil)
    w.stringTag("LevelName", options.levelName);
    w.intTag("GameType", 1); // creative
    w.byteTag("allowCommands", 1);
    w.byteTag("initialized", 1);
    w.byteTag("hardcore", 0);
    w.byteTag("Difficulty", 1);
    w.longTag("LastPlayed",
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count());
    w.longTag("Time", 0);
    w.longTag("DayTime", 6000); // noon
    w.intTag("SpawnX", spawnX);
    w.intTag("SpawnY", spawnY);
    w.intTag("SpawnZ", spawnZ);
    w.floatTag("SpawnAngle", 0.0f);
    w.byteTag("raining", 0);
    w.byteTag("thundering", 0);

    // Outside the exported chunks: an empty world (the void preset).
    w.beginCompound("WorldGenSettings");
    w.longTag("seed", static_cast<std::int64_t>(seed));
    w.byteTag("generate_features", 0);
    w.byteTag("bonus_chest", 0);
    w.beginCompound("dimensions");
    w.beginCompound("minecraft:overworld");
    w.stringTag("type", "minecraft:overworld");
    w.beginCompound("generator");
    w.stringTag("type", "minecraft:flat");
    w.beginCompound("settings");
    w.beginList("layers", nbt::Tag::Compound, 1);
    w.beginCompoundPayload();
    w.stringTag("block", "minecraft:air");
    w.intTag("height", 1);
    w.endCompound();
    w.stringTag("biome", "minecraft:the_void");
    w.byteTag("features", 0);
    w.byteTag("lakes", 0);
    w.endCompound(); // settings
    w.endCompound(); // generator
    w.endCompound(); // overworld
    w.beginCompound("minecraft:the_nether");
    w.stringTag("type", "minecraft:the_nether");
    w.beginCompound("generator");
    w.stringTag("type", "minecraft:noise");
    w.stringTag("settings", "minecraft:nether");
    w.beginCompound("biome_source");
    w.stringTag("type", "minecraft:multi_noise");
    w.stringTag("preset", "minecraft:nether");
    w.endCompound();
    w.endCompound();
    w.endCompound(); // nether
    w.beginCompound("minecraft:the_end");
    w.stringTag("type", "minecraft:the_end");
    w.beginCompound("generator");
    w.stringTag("type", "minecraft:noise");
    w.stringTag("settings", "minecraft:end");
    w.beginCompound("biome_source");
    w.stringTag("type", "minecraft:the_end");
    w.endCompound();
    w.endCompound();
    w.endCompound(); // end
    w.endCompound(); // dimensions
    w.endCompound(); // WorldGenSettings

    w.beginCompound("DataPacks");
    w.beginList("Enabled", nbt::Tag::String, 1);
    w.stringPayload("vanilla");
    w.beginList("Disabled", nbt::Tag::String, 0);
    w.endCompound();
    w.endCompound(); // Data
    w.endCompound(); // root

    const auto data = zlibCompress(w.bytes(), true);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out)
        throw MinecraftExportError("cannot write " + path.string());
}

} // namespace

MinecraftExportReport exportMinecraft(const WorldState& world, const std::filesystem::path& directory,
                                      const MinecraftExportOptions& options, const std::function<void(float)>& progress) {
    if (!world.hasVoxels())
        throw MinecraftExportError("the world has no voxels (run the voxelize stage)");
    const VoxelVolume& voxels = world.voxels();
    if (voxels.minY() % kSection != 0)
        throw MinecraftExportError("the bottom of the world (world.min_y) must be a multiple of 16");

    // Map centered on the origin, on chunk boundaries.
    Layout layout{};
    layout.sectionsX = (voxels.width() + kSection - 1) / kSection;
    layout.sectionsY = (voxels.maxY() - voxels.minY() + kSection - 1) / kSection;
    const int sectionsZ = (voxels.length() + kSection - 1) / kSection;
    layout.ourMinSectionY = voxels.minY() / kSection;
    layout.originChunkX = -layout.sectionsX / 2;
    layout.originChunkZ = -sectionsZ / 2;

    // Regions covering the map, each with its 32 x 32 chunks.
    const int rx0 = floorDiv(layout.originChunkX, kRegionChunks);
    const int rx1 = floorDiv(layout.originChunkX + layout.sectionsX - 1, kRegionChunks);
    const int rz0 = floorDiv(layout.originChunkZ, kRegionChunks);
    const int rz1 = floorDiv(layout.originChunkZ + sectionsZ - 1, kRegionChunks);
    std::vector<std::pair<int, int>> regions;
    for (int rz = rz0; rz <= rz1; ++rz)
        for (int rx = rx0; rx <= rx1; ++rx)
            regions.emplace_back(rx, rz);

    std::filesystem::create_directories(directory / "region");
    MinecraftExportReport report;
    std::atomic<std::size_t> next{0};
    std::atomic<int> chunksDone{0}, dropped{0};
    std::mutex errorMutex;
    std::string error;
    std::atomic<bool> failed{false};
    const auto worker = [&] {
        for (std::size_t r; (r = next++) < regions.size();) {
            try {
                const auto [rx, rz] = regions[r];
                std::vector<std::vector<std::uint8_t>> chunks(kRegionChunks * kRegionChunks);
                for (int lz = 0; lz < kRegionChunks; ++lz) {
                    for (int lx = 0; lx < kRegionChunks; ++lx) {
                        const int sx = rx * kRegionChunks + lx - layout.originChunkX;
                        const int sz = rz * kRegionChunks + lz - layout.originChunkZ;
                        if (sx < 0 || sz < 0 || sx >= layout.sectionsX || sz >= sectionsZ)
                            continue;
                        int droppedHere = 0;
                        chunks[lz * kRegionChunks + lx] =
                            zlibCompress(chunkNbt(voxels, layout, sx, sz, options, droppedHere), false);
                        dropped += droppedHere;
                        ++chunksDone;
                    }
                }
                writeRegion(directory / "region" / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca"),
                            chunks);
            } catch (const std::exception& e) {
                const std::lock_guard lock(errorMutex);
                error = e.what();
                failed = true;
            }
        }
    };
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threadCount =
        std::min(static_cast<unsigned>(regions.size()), options.threads > 0 ? static_cast<unsigned>(options.threads) : hardware);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < threadCount; ++t)
        threads.emplace_back(worker);
    const int totalChunks = layout.sectionsX * sectionsZ;
    while (progress && chunksDone < totalChunks && !failed) {
        progress(static_cast<float>(chunksDone) / totalChunks);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    for (auto& thread : threads)
        thread.join();
    if (!error.empty())
        throw MinecraftExportError(error);

    // Spawn: the land column closest to the center of the map.
    const int cx = voxels.width() / 2, cz = voxels.length() / 2;
    int bestX = cx, bestZ = cz, bestY = voxels.topNonAir(cx, cz);
    long best = -1;
    for (int z = 0; z < voxels.length(); z += 4) {
        for (int x = 0; x < voxels.width(); x += 4) {
            const int y = voxels.topNonAir(x, z);
            const Block top = voxels.get(x, y, z);
            if (top == Block::Water || top == Block::River || y < world.info().seaLevel)
                continue;
            const long d = static_cast<long>(x - cx) * (x - cx) + static_cast<long>(z - cz) * (z - cz);
            if (best < 0 || d < best)
                best = d, bestX = x, bestZ = z, bestY = y;
        }
    }
    report.spawnX = bestX + layout.originChunkX * kSection;
    report.spawnY = std::clamp(bestY + 1, kMinSectionY * kSection, kMaxSectionY * kSection + 15);
    report.spawnZ = bestZ + layout.originChunkZ * kSection;
    writeLevelDat(directory / "level.dat", options, world.info().seed, report.spawnX, report.spawnY, report.spawnZ);

    report.chunks = chunksDone;
    report.regions = static_cast<int>(regions.size());
    report.sectionsDropped = dropped;
    if (progress)
        progress(1.0f);
    return report;
}

} // namespace wg
