#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap::presentation::material_notifications {

inline constexpr std::size_t kIngredientCount = 20, kCapacity = 64, kPerService = 4;
inline constexpr std::uint64_t kLifetimeMs = 30'000;
inline constexpr unsigned kOverflowLogLimit = 8;

struct Notice {
    std::uint64_t accountSoid{}, characterSoid{};
    std::uint8_t ingredientOrdinal{};
    std::int32_t quantity{};
    friend bool operator==(const Notice&, const Notice&) = default;
};
enum class EnqueueResult { enqueued, invalid, full };
enum class Delivery { delivered, retry, discard };
/** Called outside the queue lock, only by service's caller. Must check current owner identity. */
using Consumer = Delivery (*)(void* context, const Notice& notice) noexcept;
struct GainsResult {
    std::size_t enqueued{}, full{};
    bool valid{};
};
struct ServiceResult {
    std::size_t attempted{}, delivered{}, retried{}, discarded{}, expired{};
};

/** Copies one committed acquisition. now is monotonic milliseconds (GetTickCount64 in production).
 */
[[nodiscard]] EnqueueResult enqueue(const Notice& notice, std::uint64_t now) noexcept;
/**
 * Copies actual credited amounts after the outermost SQLite commit. Prepared reward before/after
 * ingredient snapshots may supply these gains; unrelated live balance snapshots must not.
 * Requires exactly twenty nonnegative gains and nonzero owner ids; malformed batches enqueue none.
 * Zero gains are skipped. Capacity refusal can leave a valid batch partially queued, as reported.
 */
[[nodiscard]] GainsResult enqueue_committed_gains(std::uint64_t accountSoid,
                                                  std::uint64_t characterSoid,
                                                  std::span<const std::int32_t> gains,
                                                  std::uint64_t now) noexcept;
/**
 * Thread-confined BAP outer-transaction scope. Construct before preparing its service outcome;
 * publish only after the outermost SQLite commit succeeds. Destruction drops unpublished gains.
 * One grant (at most twenty ingredients) may be staged. Nested scopes refuse staging/publishing.
 * Missing scope refuses staging: a nested State savepoint alone never authorizes a HUD notice.
 */
class CommitScope {
public:
    CommitScope() noexcept;
    ~CommitScope();
    CommitScope(const CommitScope&) = delete;
    CommitScope& operator=(const CommitScope&) = delete;
    [[nodiscard]] GainsResult publish(std::uint64_t now) noexcept;

private:
    friend bool
    stage_committed_gains(std::uint64_t, std::uint64_t, std::span<const std::int32_t>) noexcept;
    CommitScope* previous_{};
    std::uint64_t accountSoid_{}, characterSoid_{};
    std::array<std::int32_t, kIngredientCount> gains_{};
    bool staged_{};
};
/** Copies one successful inner State grant into the active outer commit scope, without enqueueing.
 */
[[nodiscard]] bool stage_committed_gains(std::uint64_t accountSoid,
                                         std::uint64_t characterSoid,
                                         std::span<const std::int32_t> gains) noexcept;
/** Clears queued records and cancels retries from an in-flight callback; cannot undo its delivery.
 */
void clear() noexcept;
/**
 * Caller-owned game-thread pump. At most four initial records are examined, with callbacks outside
 * all queue locks. Retry preserves original age and rotates to the tail. Age >=30s (or a clock
 * regression) discards without calling the consumer. Null/reentrant/concurrent service does no
 * work. The consumer must discard mismatched owners; enqueue never calls it or enters native/UI
 * code.
 */
[[nodiscard]] ServiceResult
service(std::uint64_t now, Consumer consumer, void* context = nullptr) noexcept;

} // namespace sunrise::server::bap::presentation::material_notifications
