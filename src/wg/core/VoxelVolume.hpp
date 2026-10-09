#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "wg/core/Block.hpp"

namespace wg {

// Finite 3D block storage, split into 16x16x16 sections (same as Minecraft).
//
// - A section filled with a single block takes no memory (air above the
//   terrain, stone deep below).
// - Sections are copy-on-write: copying a volume only copies pointers, and a
//   section is duplicated the first time it is modified. Pipeline snapshots of
//   the 3D world therefore only cost what each stage actually changed.
class VoxelVolume {
public:
    static constexpr int kSectionSize = 16;
    static constexpr int kSectionVolume = kSectionSize * kSectionSize * kSectionSize;

    VoxelVolume() = default;
    VoxelVolume(int width, int length, int minY, int maxY, Block fill = Block::Air);

    int width() const { return width_; }
    int length() const { return length_; }
    int minY() const { return minY_; }
    int maxY() const { return maxY_; } // exclusive

    bool contains(int x, int y, int z) const {
        return x >= 0 && z >= 0 && x < width_ && z < length_ && y >= minY_ && y < maxY_;
    }

    // Out-of-bounds reads return Air.
    Block get(int x, int y, int z) const;
    void set(int x, int y, int z, Block block);

    // Fills [yBegin, yEnd) of column (x, z). Clamped to the volume.
    void fillColumn(int x, int z, int yBegin, int yEnd, Block block);
    // Fills the box [x0, x1) x [y0, y1) x [z0, z1). Fully covered sections
    // become uniform, so filling large areas is cheap. Clamped to the volume.
    void fillBox(int x0, int y0, int z0, int x1, int y1, int z1, Block block);

    // Y of the highest non-air block of the column, or minY() - 1 if none.
    int topNonAir(int x, int z) const;

    // Turns sections that ended up holding a single block type back into
    // uniform sections. Call after large edits.
    void compact();

    // Raw section access (serialization). Sections are ordered by (sz, sx, sy),
    // sy fastest; blocks inside a section by (ly, lz, lx), lx fastest.
    int sectionCount() const { return static_cast<int>(slots_.size()); }
    bool isSectionUniform(int index) const { return !slots_[index].data; }
    Block sectionUniformBlock(int index) const { return slots_[index].uniform; }
    std::span<const Block> sectionBlocks(int index) const;
    void setSectionUniform(int index, Block block);
    std::span<Block> editSectionBlocks(int index);

private:
    struct Section {
        std::array<Block, kSectionVolume> blocks;
    };
    struct Slot {
        std::shared_ptr<Section> data; // null: every block is `uniform`
        Block uniform = Block::Air;
    };

    std::size_t slotIndex(int sx, int sy, int sz) const {
        return (static_cast<std::size_t>(sz) * sectionsX_ + sx) * sectionsY_ + sy;
    }
    static int localIndex(int lx, int ly, int lz) { return (ly * kSectionSize + lz) * kSectionSize + lx; }
    Section& editSection(Slot& slot);

    int width_ = 0;
    int length_ = 0;
    int minY_ = 0;
    int maxY_ = 0;
    int sectionsX_ = 0;
    int sectionsY_ = 0;
    int sectionsZ_ = 0;
    std::vector<Slot> slots_;
};

} // namespace wg
