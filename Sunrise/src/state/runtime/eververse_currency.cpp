#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "../investment/store_internal.h"
#include "eververse_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::eververse {
namespace store = investment::store;
namespace helpers = runtime::detail;

bool detail::wallet_balance(const AccountState& account,
                            std::uint32_t currencyHash,
                            std::int32_t& balance,
                            std::int32_t& cap) noexcept {
    balance = 0;
    cap = 0;
    if (currencyHash != kSilverHash && currencyHash != kBrightDustHash) return false;
    build_data::items::Definition item{};
    build_data::items::details::Definition definition{};
    build_data::inventory::buckets::Descriptor bucket{};
    if (!account::valid(account) || !helpers::valid_profile_inventory(account)
        || !build_data::find_item_definition_hash(currencyHash, item)
        || !build_data::find_configured_item_detail(item.definitionIndex, definition)
        || definition.definitionHash != currencyHash || definition.bucketId != item.bucketId
        || definition.maxStackSize <= 0 || definition.equipmentSlot
        || definition.instancedDefinitionState
               != build_data::items::details::InstancedDefinitionState::stackable
        || build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
        || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::profile)
        return false;
    cap = definition.maxStackSize;
    bool found = false;
    for (std::size_t i = 0; i < account.profileItemCount; ++i) {
        const auto& row = account.profileItems[i];
        if (row.definitionHash != currencyHash) continue;
        if (found || row.instanceSoid != 0 || row.quantity < 1 || row.quantity > cap) return false;
        found = true;
        balance = row.quantity;
    }
    return true;
}

bool detail::stage_wallet_balance(AccountState& account,
                                  std::uint32_t currencyHash,
                                  std::int64_t amount) noexcept {
    std::int32_t before{}, cap{};
    if (!wallet_balance(account, currencyHash, before, cap) || amount < 0 || amount > cap)
        return false;
    if (amount == before) return true;
    std::size_t position = account.profileItemCount;
    std::int32_t serial{};
    for (std::size_t i = 0; i < account.profileItemCount; ++i) {
        const auto& row = account.profileItems[i];
        serial = (std::max)(serial, row.mutationSerial);
        if (row.definitionHash == currencyHash) position = i;
    }
    const auto maximum = (std::numeric_limits<std::int32_t>::max)();
    if (amount == 0) {
        // Every row shifted by removal receives a fresh generation, as with material charges.
        if (position == account.profileItemCount
            || account.profileItemCount - position - 1 > static_cast<std::size_t>(maximum - serial))
            return false;
        for (std::size_t i = position; i + 1 < account.profileItemCount; ++i) {
            account.profileItems[i] = account.profileItems[i + 1];
            account.profileItems[i].mutationSerial = ++serial;
        }
        account.profileItems[--account.profileItemCount] = {};
    } else {
        if (serial == maximum || position >= account.profileItems.size()) return false;
        auto& row = account.profileItems[position];
        if (position == account.profileItemCount) {
            row = {};
            row.definitionHash = currencyHash;
            row.seen = true;
            ++account.profileItemCount;
        }
        row.quantity = static_cast<std::int32_t>(amount);
        row.mutationSerial = serial + 1;
    }
    return account::valid(account) && helpers::valid_profile_inventory(account);
}

Result set_silver(std::int64_t amount) noexcept {
    if (amount < 0 || amount > (std::numeric_limits<std::int32_t>::max)())
        return {false, 0, "Silver amount is outside the nonnegative signed-32-bit range"};
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    if (!account) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*account))
        return {false, 0, "investment database unavailable"};
    std::int32_t balance{}, cap{};
    if (!detail::wallet_balance(*account, kSilverHash, balance, cap))
        return {false, 0, "installed Silver wallet unavailable or saved wallet invalid"};
    if (amount > cap) return {false, balance, "amount exceeds installed Silver wallet cap"};
    if (!detail::stage_wallet_balance(*account, kSilverHash, amount)
        || (amount != balance && !store::write_account(*account)) || !transaction.commit())
        return {false, balance, "Silver transaction refused; no changes committed"};
    return {true, static_cast<std::int32_t>(amount), "local Silver balance committed"};
}

bool read_silver(std::int32_t& amount) noexcept {
    amount = 0;
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    std::int32_t cap{};
    return account && store::read_account(*account)
           && detail::wallet_balance(*account, kSilverHash, amount, cap);
}

bool read_wallet_sync(std::uint64_t accountSoid, std::uint32_t& revision) noexcept {
    revision = 0;
    if (accountSoid == 0) return false;
    store::Transaction transaction;
    store::Statement row("SELECT revision FROM eververse_wallet_sync WHERE account_soid=?");
    if (!transaction.ready() || !row.parameters(accountSoid)) return false;
    const int result = row.step();
    if (result == SQLITE_ROW) {
        if (!row.column(0, revision) || revision == 0 || row.step() != SQLITE_DONE) return false;
    } else if (result != SQLITE_DONE) {
        return false;
    }
    return transaction.commit();
}

bool synchronize_wallet(std::uint32_t& revision) noexcept {
    revision = 0;
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    store::Transaction transaction;
    std::int32_t balance{}, cap{};
    std::uint32_t before{};
    if (!account || !transaction.ready() || !store::read_account(*account)
        || !detail::wallet_balance(*account, kSilverHash, balance, cap)
        || !read_wallet_sync(account->primarySoid, before)
        || before == (std::numeric_limits<std::uint32_t>::max)())
        return false;
    // Platform receipts in the request are not purchase authority. This server reconciles
    // its own SQLite wallet; no platform offer, currency or cosmetic is minted here.
    store::Statement row(
        "INSERT INTO eververse_wallet_sync(account_soid,revision) VALUES (?,?) "
        "ON CONFLICT(account_soid) DO UPDATE SET revision=excluded.revision");
    if (!row.write(account->primarySoid, before + 1U) || !transaction.commit()) return false;
    revision = before + 1U;
    return true;
}

} // namespace sunrise::state::eververse
