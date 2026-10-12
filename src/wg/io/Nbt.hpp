#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wg::nbt {

// Tag types of Minecraft's NBT format.
enum class Tag : std::uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

// Streaming writer of uncompressed, big-endian NBT. Named tags go inside a
// compound; list elements are written with the unnamed `*Payload` calls (or
// beginCompoundPayload / endCompound for a list of compounds).
//
//   Writer w;
//   w.beginCompound("");            // root
//   w.intTag("DataVersion", 3955);
//   w.beginList("palette", Tag::Compound, 1);
//   w.beginCompoundPayload();
//   w.stringTag("Name", "minecraft:stone");
//   w.endCompound();
//   w.endCompound();                // root
class Writer {
public:
    void beginCompound(std::string_view name);
    void beginCompoundPayload() {} // a compound inside a list: no header
    void endCompound();
    void beginList(std::string_view name, Tag elements, std::int32_t count);

    void byteTag(std::string_view name, std::int8_t value);
    void shortTag(std::string_view name, std::int16_t value);
    void intTag(std::string_view name, std::int32_t value);
    void longTag(std::string_view name, std::int64_t value);
    void floatTag(std::string_view name, float value);
    void stringTag(std::string_view name, std::string_view value);
    void longArrayTag(std::string_view name, std::span<const std::int64_t> values);

    void stringPayload(std::string_view value);

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    void header(Tag tag, std::string_view name);
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);

    std::vector<std::uint8_t> bytes_;
};

} // namespace wg::nbt
