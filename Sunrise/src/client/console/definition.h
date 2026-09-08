#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::client::console {

/** Entries the registry holds. One per command; modules register at startup and never remove. */
inline constexpr std::size_t kEntryCapacity = 192;
/** Parameters one command declares. Nothing registered so far needs more. */
inline constexpr std::size_t kParameterCapacity = 4;
/** Characters one input line may hold, including the terminator. */
inline constexpr std::size_t kLineCapacity = 256;
/** Tokens one line may split into: the command, its parameters, and one spare. Sized exactly, a
 * line filling every parameter of a four-parameter command leaves no slack, and a token counted
 * differently anywhere would drop the last argument and report the command as taking fewer. */
inline constexpr std::size_t kTokenCapacity = kParameterCapacity + 2;
/** Every registered command fits in a completion pass. */
inline constexpr std::size_t kCandidateCapacity = kEntryCapacity;

/** What one parameter accepts, which decides both how it parses and how it completes. */
enum class ValueType : std::uint8_t {
    /** `on`/`off`, `true`/`false`, `1`/`0`. Completes to `on` and `off`. */
    boolean,
    /** A signed decimal or nonnegative hexadecimal integer, checked against the bounds. */
    integer,
    /** A decimal real, refused outside the parameter's bounds. */
    real,
    /** A bare word. Completes from the parameter's choices when it declares any. */
    text,
};

/** One parsed argument. Only the member matching its type is meaningful. */
struct Value {
    ValueType type{ValueType::text};
    bool boolean{};
    std::int64_t integer{};
    double real{};
    /** Borrowed from the line being invoked, so it outlives no handler call. */
    std::string_view text{};
};

/**
 * Supplies the legal values of one parameter, for completion and for help.
 *
 * Called only from the render thread while the prompt is open, so an implementation may return a
 * pointer into its own fixed storage.
 *
 * @param index Zero-based candidate position.
 * @return The candidate, or null once index passes the last one.
 */
using Choices = const char* (*)(std::size_t index) noexcept;

/** Where a handler writes what the operator should see. */
class Output {
public:
    virtual ~Output() = default;
    /** Appends one line. A line longer than the sink's width is truncated, never dropped. */
    virtual void line(std::string_view text) noexcept = 0;
    /** Appends one printf-style line. */
    void format(const char* format, ...) noexcept;
};

/** One parameter of one command. */
struct Parameter {
    /** Shown in help and in the prompt's hint. Null ends the parameter list. */
    const char* name{};
    /** One sentence, shown by `help <command>`. */
    const char* help{};
    ValueType type{ValueType::text};
    /** Legal values, or null when the parameter is unconstrained. */
    Choices choices{};
    /** Bounds for `integer` and `real`. Equal values mean unbounded. */
    double minimum{};
    double maximum{};
    /** A parameter the line may omit. Every optional parameter follows every required one. */
    bool optional{};
};

/**
 * Runs one command.
 * @param arguments Parsed values, in declaration order, holding only what the line supplied.
 * @param output Sink for whatever the operator should see.
 * @return True when the command did what it was asked. False prints nothing extra by itself, so
 *     a handler that refuses should say why through the output first.
 */
using Handler = bool (*)(std::span<const Value> arguments, Output& output) noexcept;

/** One registered command. */
struct Entry {
    /** Dotted lowercase, e.g. `item.grant`. The prompt completes on the whole string. */
    const char* name{};
    /** One sentence, shown by bare `help` and by `help <command>`. */
    const char* help{};
    Handler handler{};
    std::array<Parameter, kParameterCapacity> parameters{};
};

} // namespace sunrise::client::console
