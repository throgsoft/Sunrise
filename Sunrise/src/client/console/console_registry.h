#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "definition.h"

namespace sunrise::client::console {

/**
 * Publishes one command. Names are unique; a second registration of a name replaces the first so
 * a module reloading its table cannot leave two handlers for one name.
 *
 * @param entry Command to publish. Its name, help and handler must all be present.
 * @return True when the entry is well formed and the registry had room.
 */
[[nodiscard]] bool add(const Entry& entry) noexcept;

/** Drops every registered command. */
void clear() noexcept;

/** @return Commands currently registered. */
[[nodiscard]] std::size_t count() noexcept;

/**
 * Finds one command by exact name.
 * @param name Whole command name.
 * @param entry Receives the command.
 * @return True when a command of that name is registered.
 */
[[nodiscard]] bool find(std::string_view name, Entry& entry) noexcept;

/**
 * Copies every registered command in ascending name order.
 * @param output Destination.
 * @param count Receives the copied count, or zero when output is too small.
 * @return True when every entry fit.
 */
[[nodiscard]] bool snapshot(std::span<Entry> output, std::size_t& count) noexcept;

} // namespace sunrise::client::console
