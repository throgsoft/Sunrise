#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::middleware::queuez {
struct Family;
}

namespace sunrise::server::bap::capture {

inline constexpr std::size_t kFrameLimit = 2048;
inline constexpr std::size_t kMarkerLimit = 128;
inline constexpr std::size_t kObjectLimit = 128;
inline constexpr std::size_t kBodyLimit = 320;

/** Outbound captures describe encoded candidates, not delivery or State commit receipts. */
enum class Kind : std::uint8_t { request, response, notification };

struct Status {
    bool enabled{};
    std::uint64_t run{};
    std::size_t frames{};
    std::size_t markers{};
    std::size_t objects{};
};

/** Off by default. Turning on explicitly starts a fresh bounded capture. */
void set_enabled(bool enabled) noexcept;
[[nodiscard]] Status status() noexcept;

/** Borrowed decoded body only; correlation is a task id or notification sequence. */
void record(Kind kind,
            std::uint16_t service,
            std::uint32_t correlation,
            std::span<const std::byte> body,
            const middleware::queuez::Family* family = nullptr) noexcept;

/** Returns false while disabled or after the marker budget; labels are sanitized and capped. */
[[nodiscard]] bool mark(std::string_view label) noexcept;

/** Records selected sync fields of a staged account image, never a delivery receipt. */
void record_account(std::uint32_t definitionId,
                    std::span<const std::byte> image) noexcept;

} // namespace sunrise::server::bap::capture
