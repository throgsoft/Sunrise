#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::enemy_classes {

inline constexpr std::size_t kCapacity = 1024;
inline constexpr std::uint32_t kClassRootClass = 0x80805B1BU;
inline constexpr std::uint32_t kTaxonomyRootClass = 0x80807BBBU;
inline constexpr std::uint32_t kClassRowClass = 0x80805B1FU;
inline constexpr std::uint32_t kTaxonomyRowClass = 0x80807BC0U;

struct Row {
    std::uint32_t classHash{};
    std::uint32_t lookupHash{};
    /** Zero for valid non-enemy taxonomy indexes 4 and 5. */
    std::uint32_t raceHash{};
};

struct Catalog {
    /** Preserves investment class order, which native class indexes address. */
    std::array<Row, kCapacity> rows{};
    std::size_t count{};
};

namespace detail {

/** Bounded little-endian reads do not require aligned input or host endianness. */
[[nodiscard]] inline bool read(std::span<const std::byte> blob,
                               std::size_t offset,
                               std::size_t width,
                               std::uint64_t& value) noexcept {
    value = 0;
    if (offset > blob.size() || width > blob.size() - offset) {
        return false;
    }
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(blob[offset + i]))
                 << (8U * i);
    }
    return true;
}

struct Array {
    std::size_t count{};
    std::size_t data{};
};

/** Both roots have {u64 count, i64 relative} at +8, relative based at +16. */
[[nodiscard]] inline bool array(std::span<const std::byte> blob,
                                std::uint32_t elementClass,
                                std::size_t stride,
                                Array& output) noexcept {
    output = {};
    std::uint64_t length = 0, count = 0, relative = 0;
    if (!read(blob, 0, 8, length) || length != blob.size() || !read(blob, 8, 8, count) || count == 0
        || count > kCapacity || !read(blob, 16, 8, relative)) {
        return false;
    }
    // A negative relative cannot reach a header after this root's descriptor.
    // This bound also rejects signed overflow and prevents unsigned addition overflow.
    if (relative > blob.size() - 16U) {
        return false;
    }
    const auto header = 16U + static_cast<std::size_t>(relative);
    if (header < 32U || header > blob.size() || blob.size() - header < 16U) {
        return false;
    }
    std::uint64_t marker = 0, headerCount = 0, rowClass = 0;
    if (!read(blob, header - 4U, 4, marker) || marker != 0x80809FBDU
        || !read(blob, header, 8, headerCount) || headerCount != count
        || !read(blob, header + 8U, 4, rowClass) || rowClass != elementClass) {
        return false;
    }
    const auto data = header + 16U;
    if (blob.size() - data != static_cast<std::size_t>(count) * stride) {
        return false;
    }
    output = {static_cast<std::size_t>(count), data};
    return true;
}

inline constexpr std::array<std::uint32_t, 7> kRaceHashes{
    0x208C606EU, 0x6187D46FU, 0xC2A4EF43U, 0x2A682C12U, 0U, 0U, 0x4F33FBC2U};

[[nodiscard]] inline bool valid_class_key(std::uint64_t key) noexcept {
    return key != 0 && key != 0x811C9DC5U && key != 0xFFFFFFFFU;
}

} // namespace detail

/**
 * Parses class root 81326ED2 and taxonomy root 8132067D without allocation.
 * The caller must verify their package classes against kClassRootClass and
 * kTaxonomyRootClass: package class identity is not stored inside these payloads.
 * Joins by classHash, never taxonomy row order. lookupHash aliases may repeat.
 * Any malformed row or missing/ambiguous join leaves the entire output empty.
 */
[[nodiscard]] inline bool parse(std::span<const std::byte> classBlob,
                                std::span<const std::byte> raceBlob,
                                Catalog& output) noexcept {
    output = {};
    detail::Array classes{}, taxonomy{};
    if (!detail::array(classBlob, kClassRowClass, 28U, classes)
        || !detail::array(raceBlob, kTaxonomyRowClass, 8U, taxonomy)
        || classes.count != taxonomy.count) {
        return false;
    }

    Catalog staged{};
    std::array<std::uint32_t, kCapacity> taxonomyKeys{};
    std::array<std::uint32_t, kCapacity> taxonomyRaces{};
    for (std::size_t i = 0; i < taxonomy.count; ++i) {
        std::uint64_t key = 0, race = 0;
        if (!detail::read(raceBlob, taxonomy.data + i * 8U, 4, key) || !detail::valid_class_key(key)
            || !detail::read(raceBlob, taxonomy.data + i * 8U + 4U, 4, race)
            || race >= detail::kRaceHashes.size()) {
            return false;
        }
        taxonomyKeys[i] = static_cast<std::uint32_t>(key);
        taxonomyRaces[i] = detail::kRaceHashes[static_cast<std::size_t>(race)];
        for (std::size_t j = 0; j < i; ++j) {
            if (taxonomyKeys[j] == taxonomyKeys[i]) {
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < classes.count; ++i) {
        std::uint64_t key = 0, lookup = 0;
        if (!detail::read(classBlob, classes.data + i * 28U, 4, key)
            || !detail::valid_class_key(key)
            || !detail::read(classBlob, classes.data + i * 28U + 4U, 4, lookup)) {
            return false;
        }
        Row& row = staged.rows[i];
        row.classHash = static_cast<std::uint32_t>(key);
        row.lookupHash = static_cast<std::uint32_t>(lookup);
        for (std::size_t j = 0; j < i; ++j) {
            if (staged.rows[j].classHash == row.classHash) {
                return false;
            }
        }
        bool found = false;
        for (std::size_t j = 0; j < taxonomy.count; ++j) {
            if (taxonomyKeys[j] == row.classHash) {
                row.raceHash = taxonomyRaces[j];
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    staged.count = classes.count;
    output = staged;
    return true;
}

} // namespace sunrise::state::build_data::enemy_classes
