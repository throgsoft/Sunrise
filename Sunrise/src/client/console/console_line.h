#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "definition.h"

namespace sunrise::client::console {

/** One line split into its command and parameter tokens. */
struct Tokens {
    std::array<std::string_view, kTokenCapacity> values{};
    std::size_t count{};
    /** True when the line ends in whitespace, so the caret sits on a new empty token. */
    bool trailingSeparator{};
};

/**
 * Splits one line on runs of spaces and tabs.
 * @param line Whole input line.
 * @return The tokens, borrowed from line. Tokens past kTokenCapacity are dropped.
 */
[[nodiscard]] Tokens split(std::string_view line) noexcept;

/**
 * Runs one line against the registry.
 *
 * Every refusal explains itself through the output: an unknown command, a missing required
 * parameter, one that does not parse as its type, and one outside its bounds all say so.
 *
 * @param line Whole input line. An empty or blank line does nothing and succeeds.
 * @param output Sink for the command's own output and for any refusal.
 * @return True when a command ran and reported success.
 */
[[nodiscard]] bool invoke(std::string_view line, Output& output) noexcept;

/**
 * Completes the token the caret sits in.
 *
 * Token zero completes registered command names. A later token completes from that parameter's
 * choices, and a boolean parameter completes `on` and `off` whether or not it declares any.
 *
 * @param line Whole input line up to the caret.
 * @param candidates Receives the matches, borrowed from the registry or from a Choices provider.
 * @param count Receives the match count, capped at the span size.
 * @param prefix Receives the token being completed, which every match starts with.
 * @return True when at least one match was found.
 */
[[nodiscard]] bool complete(std::string_view line,
                            std::span<std::string_view> candidates,
                            std::size_t& count,
                            std::string_view& prefix) noexcept;

/**
 * Finds the longest string every candidate starts with.
 * @param candidates Matches from complete().
 * @return The shared prefix, borrowed from the first candidate.
 */
[[nodiscard]] std::string_view shared_prefix(std::span<const std::string_view> candidates) noexcept;

} // namespace sunrise::client::console
