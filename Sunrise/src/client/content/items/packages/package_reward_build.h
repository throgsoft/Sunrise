#pragma once

#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/rewards/definition.h"

namespace sunrise::client::content::items::packages {

/** Resolves expression references and unlock slots once for a package pass. */
class RewardConditions {
public:
    [[nodiscard]] bool load(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            std::span<const std::byte> root) noexcept;
    [[nodiscard]] bool bind(state::build_data::rewards::Instruction& instruction) const noexcept;
    [[nodiscard]] bool read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<state::build_data::rewards::Instruction>& bank,
                            state::build_data::rewards::Range& range) const noexcept;
    [[nodiscard]] bool read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<state::build_data::rewards::Instruction> output,
                                 std::size_t& count) const noexcept;

private:
    [[nodiscard]] bool append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<state::build_data::rewards::Instruction>& bank,
                                         std::size_t depth) const noexcept;
    std::vector<std::byte> flags_;
    std::vector<std::byte> values_;
    std::vector<std::byte> expressions_;
    middleware::content::packages::tables::Array flagRows_{};
    middleware::content::packages::tables::Array valueRows_{};
    middleware::content::packages::tables::Array expressionRows_{};
};

[[nodiscard]] bool build_rewards(const middleware::content::packages::reader::Source& source,
                                 middleware::content::packages::reader::Scratch& scratch,
                                 std::span<const std::byte> root) noexcept;
/** Progression rewards and pool entries use the same socket-override layout. */
[[nodiscard]] bool read_reward_sockets(std::span<const std::byte> blob,
                                       std::size_t at,
                                       std::span<state::build_data::rewards::SocketOverride> output,
                                       std::size_t& count) noexcept;

} // namespace sunrise::client::content::items::packages
