#include "inventory_edit.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "../../build_data/runtime.h"
#include "../../investment/store_internal.h"
#include "../account_state.h"

namespace sunrise::state::account::inventory::edit {

bool set_quantity(std::uint64_t characterSoid,
                  std::uint64_t instanceSoid,
                  std::uint32_t definitionHash,
                  std::int32_t mutationSerial,
                  std::int32_t expectedQuantity,
                  std::int32_t quantity) noexcept {
    const std::unique_ptr<AccountState> snapshot(new (std::nothrow) AccountState);
    investment::store::Transaction transaction;
    build_data::items::Definition definition{};
    build_data::inventory::buckets::Descriptor bucket{};
    build_data::items::details::Definition detail{};
    if (quantity < 0 || !snapshot || !transaction.ready()
        || !investment::store::read_account(*snapshot) || !account::valid(*snapshot)
        || !build_data::find_item_definition_hash(definitionHash, definition)
        || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)) {
        return false;
    }
    if (quantity != 0
        && (!build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionHash != definitionHash || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState
                   != build_data::items::details::InstancedDefinitionState::stackable
            || quantity > detail.maxStackSize)) {
        return false;
    }
    const auto end =
        snapshot->characters.begin() + static_cast<std::ptrdiff_t>(snapshot->characterCount);
    const auto selected = std::find_if(snapshot->characters.begin(),
                                       end,
                                       [](const auto& character) { return character.selected; });
    if (selected == end || selected->soid != characterSoid) {
        return false;
    }
    for (std::size_t c = 0; instanceSoid != 0 && c < snapshot->characterCount; ++c) {
        for (const auto& equipped : snapshot->characters[c].equipment.slots) {
            if (equipped && equipped->instanceSoid == instanceSoid) {
                return false;
            }
        }
    }
    const auto edit = [&](auto& rows, std::size_t& count, const auto& matches, auto nextSerial) {
        for (std::size_t i = 0; i < count; ++i) {
            if (rows[i].definitionHash != definitionHash || rows[i].mutationSerial != mutationSerial
                || rows[i].quantity != expectedQuantity || !matches(rows[i])) {
                continue;
            }
            if (quantity == 0) {
                std::move(rows.begin() + i + 1, rows.begin() + count, rows.begin() + i);
                rows[--count] = {};
            } else {
                const auto serial = nextSerial();
                if (serial < 0) {
                    return false;
                }
                rows[i].quantity = quantity;
                rows[i].mutationSerial = serial;
            }
            return true;
        }
        return false;
    };
    const auto sameInstance = [instanceSoid](const auto& row) {
        return row.instanceSoid == instanceSoid;
    };
    const auto characterSerial = [&]() -> std::int32_t {
        if (selected->nextInventorySerial
            >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            return -1;
        }
        return static_cast<std::int32_t>(selected->nextInventorySerial++);
    };
    bool changed = false;
    if (bucket.arraySelector == build_data::inventory::buckets::ArraySelector::profile) {
        changed = edit(snapshot->profileItems, snapshot->profileItemCount, sameInstance, [&]() {
            const auto serial = account::greatest_profile_serial(*snapshot);
            return serial == (std::numeric_limits<std::int32_t>::max)() ? -1 : serial + 1;
        });
    } else if (bucket.arraySelector == build_data::inventory::buckets::ArraySelector::character) {
        auto& character = *selected;
        changed = instanceSoid != 0 ? edit(character.inventory.values,
                                           character.inventory.count,
                                           sameInstance,
                                           characterSerial)
                                    : edit(
                                          character.stacks.values,
                                          character.stacks.count,
                                          [](const auto&) { return true; },
                                          characterSerial);
    }
    return changed && account::valid(*snapshot) && investment::store::write_account(*snapshot)
           && transaction.commit();
}

} // namespace sunrise::state::account::inventory::edit
