#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../internal.h"

namespace sunrise::server::bap::encrypted {

/** One encoded Family-4 frame and the peer state it promises, held until State commits. */
struct StagedFrame {
    queuez::SessionState after{};
    /** Send nonce after the frame, published only with it. */
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::size_t size{};
};

/** One installed item to grant, and the inventory it lands in. */
struct ItemGrant {
    std::uint16_t itemDefinitionIndex{};
    std::int32_t quantity{};
    WorldRewardKind kind{};
};

/** How far one grant attempt got inside the caller's savepoint. */
enum class ItemGrantStage : std::uint8_t {
    /** State committed into the savepoint; the frame may still have failed. */
    committed,
    /** Nothing was written; a whole poll buffer can hold the frame. */
    waiting,
    /** Nothing was written; State may grant it later. */
    deferred,
    /** Nothing was written; State can never grant it. */
    unresolvable,
    /** Nothing was written; storage, entropy or the State commit failed. */
    failed,
};

/** A grant State committed into the caller's savepoint, and its frame. */
struct StagedItemGrant {
    StagedFrame frame{};
    /** Inventory rows the grant changed. */
    std::uint32_t grantedRows{};
    /** Why the grant has no frame, or null when the frame is complete. */
    const char* unpublished{};
};

/**
 * Prepares, stages, encodes and commits one item grant inside the caller's open store savepoint,
 * drawing any seed here. A stage or encode failure still commits, as no buffer can fix it.
 * @param grant Item, quantity and inventory.
 * @param response Poll output the frame must fit; nothing is copied into it.
 * @param staged Receives the committed grant and its frame.
 * @param reason Receives a static label when nothing was committed.
 */
[[nodiscard]] ItemGrantStage stage_item_grant(const ItemGrant& grant,
                                              Session& session,
                                              Scratch& scratch,
                                              std::span<std::byte> response,
                                              bool& touchesScratch,
                                              StagedItemGrant& staged,
                                              const char*& reason) noexcept;

/** Copies a staged frame into the response and moves the peer onto its after-image. */
void publish_staged_frame(const StagedFrame& frame,
                          Session& session,
                          Scratch& scratch,
                          std::span<std::byte> response,
                          std::size_t& written) noexcept;

/**
 * Publishes a grant once the caller's savepoint has committed it.
 * @return False when the grant has no frame; every Family-4 peer then resyncs the account.
 */
[[nodiscard]] bool publish_item_grant(const StagedItemGrant& staged,
                                      Session& session,
                                      Scratch& scratch,
                                      std::span<std::byte> response,
                                      std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted
