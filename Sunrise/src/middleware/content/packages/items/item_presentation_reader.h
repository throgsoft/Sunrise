#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "../reader/reader.h"
#include "../tables/activity_display_name_reader.h"

namespace sunrise::middleware::content::packages::items {

namespace text = tables::activity_display_names;
inline constexpr std::uint16_t kNoIcon = 0xFFFF;
inline constexpr std::size_t kIconSideLimit = 256;
inline constexpr std::size_t kIconByteLimit = kIconSideLimit * kIconSideLimit * 4;

/** Presentation references only; inventory authority remains in State. */
struct Display {
    std::uint16_t iconIndex{kNoIcon};
    text::Reference name{};
    text::Reference description{};
    text::Reference itemType{};
};

/** A single installed, uncompressed RGBA8 mip. No game bytes are written to disk. */
struct Icon {
    std::uint16_t width{};
    std::uint16_t height{};
    std::vector<std::byte> rgba{};
};

/** Worker-owned reader, with a bounded entry-size check before every allocation. */
class PresentationReader final {
public:
    explicit PresentationReader(const reader::Source& source) noexcept : source_(source) {}
    ~PresentationReader() noexcept;
    PresentationReader(const PresentationReader&) = delete;
    PresentationReader& operator=(const PresentationReader&) = delete;

    [[nodiscard]] bool initialize() noexcept;
    [[nodiscard]] bool item(std::uint16_t index, std::uint32_t hash, Display& output) noexcept;
    [[nodiscard]] bool objective(std::uint16_t index,
                                  std::uint32_t hash,
                                  text::Reference& output) const noexcept;
    [[nodiscard]] bool resolve(std::span<const text::Reference> refs,
                                text::Snapshot& output) noexcept;
    [[nodiscard]] bool icon(std::uint16_t index, Icon& output) noexcept;

private:
    [[nodiscard]] bool read(std::uint32_t tag, std::size_t limit,
                            std::vector<std::byte>& output, std::uint32_t& reference) noexcept;
    [[nodiscard]] text::Reference reference(std::span<const std::byte> bytes,
                                            std::size_t offset) const noexcept;
    static bool read_text(void* context, std::uint32_t tag, std::uint32_t expectedClass,
                           std::vector<std::byte>& output) noexcept;

    reader::Source source_;
    reader::Scratch scratch_{};
    std::vector<std::byte> strings_{}, banks_{}, icons_{}, objectives_{};
    std::vector<std::byte> blob_{};
};

} // namespace sunrise::middleware::content::packages::items
