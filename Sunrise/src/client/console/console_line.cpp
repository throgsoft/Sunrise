#include "console_line.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "console_registry.h"

namespace sunrise::client::console {
namespace {

constexpr std::string_view kSeparators = " \t";

/** @return True when every character is a separator, or the token is empty. */
[[nodiscard]] bool blank(std::string_view text) noexcept {
    return text.find_first_not_of(kSeparators) == std::string_view::npos;
}

/** @return Parameters the entry declares, stopping at the first unnamed one. */
[[nodiscard]] std::size_t parameter_count(const Entry& entry) noexcept {
    std::size_t declared = 0;
    while (declared < entry.parameters.size() && entry.parameters[declared].name != nullptr) {
        ++declared;
    }
    return declared;
}

/** @return True when the parameter's bounds are meaningful rather than an unbounded pair. */
[[nodiscard]] bool bounded(const Parameter& parameter) noexcept {
    return parameter.minimum != parameter.maximum;
}

/** Parses one boolean word. @return True when the word is one of the accepted spellings. */
[[nodiscard]] bool parse_boolean(std::string_view text, bool& value) noexcept {
    if (text == "on" || text == "true" || text == "1") {
        value = true;
        return true;
    }
    if (text == "off" || text == "false" || text == "0") {
        value = false;
        return true;
    }
    return false;
}

/** @return The token as a printf argument pair, which every refusal line needs. */
[[nodiscard]] int token_length(std::string_view token) noexcept {
    return static_cast<int>(token.size());
}

/** Parses one token as the parameter's type and checks its bounds. */
[[nodiscard]] bool parse_value(const Parameter& parameter,
                               std::string_view token,
                               Value& value,
                               Output& output) noexcept {
    value = {};
    value.type = parameter.type;
    value.text = token;
    switch (parameter.type) {
    case ValueType::boolean:
        if (!parse_boolean(token, value.boolean)) {
            output.format("%s: expected on or off, got %.*s",
                          parameter.name,
                          token_length(token),
                          token.data());
            return false;
        }
        return true;
    case ValueType::integer: {
        const char* first = token.data();
        const char* const last = first + token.size();
        // Both printed hexadecimal identities and decimal definition indices are accepted.
        // Refuse magnitudes outside signed storage before converting them.
        int base = 10;
        if (token.size() > 2 && first[0] == '0' && (first[1] == 'x' || first[1] == 'X')) {
            first += 2;
            base = 16;
        }
        std::uint64_t magnitude = 0;
        const std::from_chars_result parsed = std::from_chars(first, last, magnitude, base);
        const bool negative = base == 10 && !token.empty() && token.front() == '-';
        if (negative) {
            std::int64_t signedValue = 0;
            const std::from_chars_result signedParse =
                std::from_chars(token.data(), last, signedValue);
            if (signedParse.ec != std::errc{} || signedParse.ptr != last) {
                output.format("%s: expected a whole number, got %.*s",
                              parameter.name,
                              token_length(token),
                              token.data());
                return false;
            }
            value.integer = signedValue;
        } else {
            if (parsed.ec != std::errc{} || parsed.ptr != last
                || magnitude
                       > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
                output.format("%s: expected a whole number, got %.*s",
                              parameter.name,
                              token_length(token),
                              token.data());
                return false;
            }
            value.integer = static_cast<std::int64_t>(magnitude);
        }
        if (bounded(parameter)
            && (static_cast<double>(value.integer) < parameter.minimum
                || static_cast<double>(value.integer) > parameter.maximum)) {
            output.format("%s: %lld is outside %.0f..%.0f",
                          parameter.name,
                          static_cast<long long>(value.integer),
                          parameter.minimum,
                          parameter.maximum);
            return false;
        }
        value.real = static_cast<double>(value.integer);
        return true;
    }
    case ValueType::real: {
        const char* const first = token.data();
        const char* const last = first + token.size();
        const std::from_chars_result parsed = std::from_chars(first, last, value.real);
        if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value.real)) {
            output.format("%s: expected a number, got %.*s",
                          parameter.name,
                          token_length(token),
                          token.data());
            return false;
        }
        if (bounded(parameter)
            && (value.real < parameter.minimum || value.real > parameter.maximum)) {
            output.format("%s: %g is outside %g..%g",
                          parameter.name,
                          value.real,
                          parameter.minimum,
                          parameter.maximum);
            return false;
        }
        return true;
    }
    case ValueType::text:
        break;
    }
    // A text parameter declaring choices accepts only those, so a typo is refused here rather than
    // handed to the module as though it meant something.
    if (parameter.choices != nullptr) {
        for (std::size_t index = 0;; ++index) {
            const char* const choice = parameter.choices(index);
            if (choice == nullptr) {
                output.format("%s: %.*s is not one of its values",
                              parameter.name,
                              token_length(token),
                              token.data());
                return false;
            }
            if (std::string_view(choice) == token) {
                return true;
            }
        }
    }
    return true;
}

/** Appends the boolean spellings, or the parameter's own choices, to the candidate list. */
void gather(const Parameter& parameter,
            std::string_view prefix,
            std::span<std::string_view> candidates,
            std::size_t& count) noexcept {
    const auto offer = [&](std::string_view candidate) noexcept {
        if (count < candidates.size() && candidate.starts_with(prefix)) {
            candidates[count++] = candidate;
        }
    };
    if (parameter.type == ValueType::boolean) {
        offer("on");
        offer("off");
        return;
    }
    if (parameter.choices == nullptr) {
        return;
    }
    for (std::size_t index = 0; count < candidates.size(); ++index) {
        const char* const choice = parameter.choices(index);
        if (choice == nullptr) {
            return;
        }
        offer(choice);
    }
}

} // namespace

void Output::format(const char* format, ...) noexcept {
    std::array<char, 512> storage{};
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(storage.data(), storage.size(), format, arguments);
    va_end(arguments);
    if (length > 0) {
        line({storage.data(), (std::min)(static_cast<std::size_t>(length), storage.size() - 1)});
    }
}

Tokens split(std::string_view line) noexcept {
    Tokens tokens{};
    std::size_t at = 0;
    while (at < line.size() && tokens.count < tokens.values.size()) {
        const std::size_t start = line.find_first_not_of(kSeparators, at);
        if (start == std::string_view::npos) {
            break;
        }
        std::size_t end = line.find_first_of(kSeparators, start);
        if (end == std::string_view::npos) {
            end = line.size();
        }
        tokens.values[tokens.count++] = line.substr(start, end - start);
        at = end;
    }
    tokens.trailingSeparator =
        !line.empty() && kSeparators.find(line.back()) != std::string_view::npos;
    return tokens;
}

bool invoke(std::string_view line, Output& output) noexcept {
    if (line.size() >= kLineCapacity || line.find('\0') != std::string_view::npos) {
        output.line("command line is too long or contains a NUL character");
        return false;
    }
    if (blank(line)) {
        return true;
    }
    const Tokens tokens = split(line);
    Entry entry{};
    if (!find(tokens.values[0], entry)) {
        output.format("unknown command %.*s - try help",
                      token_length(tokens.values[0]),
                      tokens.values[0].data());
        return false;
    }
    const std::size_t declared = parameter_count(entry);
    const std::size_t supplied = tokens.count - 1;
    if (supplied > declared) {
        // The declared parameters are named, not just counted. A bare count invites an argument
        // about what the command takes that neither side can settle from the message.
        output.format("%s takes %zu argument(s), got %zu", entry.name, declared, supplied);
        for (std::size_t index = 0; index < declared; ++index) {
            output.format("  %zu: %s", index + 1, entry.parameters[index].name);
        }
        return false;
    }
    std::array<Value, kParameterCapacity> values{};
    for (std::size_t index = 0; index < declared; ++index) {
        const Parameter& parameter = entry.parameters[index];
        if (index >= supplied) {
            if (!parameter.optional) {
                output.format("%s: missing required argument %s", entry.name, parameter.name);
                return false;
            }
            break;
        }
        if (!parse_value(parameter, tokens.values[index + 1], values[index], output)) {
            return false;
        }
    }
    return entry.handler({values.data(), supplied}, output);
}

bool complete(std::string_view line,
              std::span<std::string_view> candidates,
              std::size_t& count,
              std::string_view& prefix) noexcept {
    count = 0;
    prefix = {};
    if (candidates.empty()) {
        return false;
    }
    const Tokens tokens = split(line);
    // A trailing separator puts the caret on a fresh token, so completion moves one position on.
    const std::size_t position =
        tokens.count == 0 ? 0 : (tokens.trailingSeparator ? tokens.count : tokens.count - 1);
    prefix = (position < tokens.count) ? tokens.values[position] : std::string_view{};

    if (position == 0) {
        std::array<Entry, kEntryCapacity> entries{};
        std::size_t entryCount = 0;
        if (!snapshot(entries, entryCount)) {
            return false;
        }
        for (std::size_t index = 0; index < entryCount && count < candidates.size(); ++index) {
            const std::string_view name(entries[index].name);
            if (name.starts_with(prefix)) {
                candidates[count++] = name;
            }
        }
        return count != 0;
    }

    Entry entry{};
    if (!find(tokens.values[0], entry)) {
        return false;
    }
    const std::size_t parameterIndex = position - 1;
    if (parameterIndex >= parameter_count(entry)) {
        return false;
    }
    gather(entry.parameters[parameterIndex], prefix, candidates, count);
    return count != 0;
}

std::string_view shared_prefix(std::span<const std::string_view> candidates) noexcept {
    if (candidates.empty()) {
        return {};
    }
    std::size_t length = candidates[0].size();
    for (const std::string_view candidate : candidates.subspan(1)) {
        length = (std::min)(length, candidate.size());
        std::size_t at = 0;
        while (at < length && candidate[at] == candidates[0][at]) {
            ++at;
        }
        length = at;
    }
    return candidates[0].substr(0, length);
}

} // namespace sunrise::client::console
