#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap {
struct Session;
struct Scratch;
namespace encrypted {

/** One pass over actual Synthesizer residents after their Family-5 predicates publish. */
struct SynthesizerFamily4Refresh {
    std::uint64_t dueTick{};
    std::uint64_t lastInstanceSoid{};
    bool armed{};
};

/** Call only after a complete Family-5 frame reaches the caller. Coalesce newer flags. */
inline void arm_synthesizer_family4_refresh(SynthesizerFamily4Refresh& refresh,
                                           std::uint64_t now) noexcept {
    // Same settling interval as the existing artifact Family-5/Family-4 companion.
    refresh = {now + 100, 0, true};
}

[[nodiscard]] bool consume_synthesizer_family4_refresh(
    Session& session, SynthesizerFamily4Refresh& refresh, Scratch& scratch,
    std::span<std::byte> response, std::size_t& written, bool& touchesScratch) noexcept;

} // namespace encrypted
} // namespace sunrise::server::bap
