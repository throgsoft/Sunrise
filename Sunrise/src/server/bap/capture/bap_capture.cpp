#include "bap_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

#include "../../../core/logging/log.h"
#include "../../../middleware/bap/frame.h"
#include "../../../middleware/datagen/definitions.h"
#include "../../../middleware/datagen/family4/account/layout.h"
#include "../../../middleware/queuez/queuez_update.h"
#include "../../../middleware/web_service/web_service_envelope.h"

namespace sunrise::server::bap::capture {
namespace {

std::atomic<bool> g_enabled{false};
std::mutex g_mutex;
Status g_status{};
std::atomic<unsigned> g_storeHandshakeRows{};
std::atomic<unsigned> g_storeAccountRows{};

/** Credential bodies stay hidden; activity uses the prefix common to both native request arms. */
std::size_t allowed_bytes(std::uint16_t service) noexcept {
    switch (service) {
    case 6:
        return 5; // Discriminator and length; the following protobuf contains credentials.
    case 8:
    case 9:
        return 17; // Compact svc8 and svc9 payloads start at 17; full svc8 starts at 21.
    case 25:
    case 26:
    case 304:
    case 305:
        return 0; // SignOn/secure-channel parameters and Steam certificate wrappers.
    default:
        return kBodyLimit;
    }
}

const char* kind_name(Kind kind) noexcept {
    switch (kind) {
    case Kind::request:
        return "request";
    case Kind::response:
        return "response";
    case Kind::notification:
        return "notification";
    }
    return "unknown";
}

} // namespace

void set_enabled(bool enabled) noexcept {
    const std::lock_guard lock(g_mutex);
    if (enabled) {
        ++g_status.run;
        g_status.frames = 0;
        g_status.markers = 0;
        g_status.objects = 0;
    }
    g_status.enabled = enabled;
    g_enabled.store(enabled, std::memory_order_relaxed);
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=bap_capture stage=control run=%llu enabled=%u frames=%zu limit=%zu",
                      static_cast<unsigned long long>(g_status.run),
                      enabled ? 1U : 0U,
                      g_status.frames,
                      kFrameLimit);
}

Status status() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_status;
}

void record(Kind kind,
            std::uint16_t service,
            std::uint32_t correlation,
            std::span<const std::byte> body,
            const middleware::queuez::Family* family) noexcept {
    // These startup exchanges precede the console. Keep bounded routing metadata even
    // with the raw probe off; no product lists, credentials or account identifiers.
    middleware::web_service::Message sync{};
    const bool syncWeb = (service == 10 || service == 11 || service == 110 || service == 112)
        && middleware::web_service::parse_request(body, sync) && sync.opcode == 104;
    if ((service == 21 || service == 22 || syncWeb)
        && g_storeHandshakeRows.fetch_add(1, std::memory_order_relaxed) < 32) {
        core::log::writef(core::log::Channel::server, core::log::Level::info,
            "ev=store_sync stage=%s svc=%u header_id=%u opcode=%u transaction=%u bytes=%zu "
            "request_byte=%d",
            kind == Kind::request ? "received" : "encoded",
            static_cast<unsigned>(service), correlation, static_cast<unsigned>(sync.opcode),
            sync.transactionId, body.size(),
            syncWeb && kind == Kind::request && sync.payload.size() == 1
                ? std::to_integer<int>(sync.payload[0]) : -1);
    }
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const std::lock_guard lock(g_mutex);
    if (!g_status.enabled || g_status.frames >= kFrameLimit) return;
    const auto allowed = allowed_bytes(service);
    const auto shown = (std::min)(body.size(), allowed);
    constexpr char digits[] = "0123456789ABCDEF";
    std::array<char, kBodyLimit * 2 + 1> hex{};
    for (std::size_t i = 0; i < shown; ++i) {
        const auto value = std::to_integer<unsigned>(body[i]);
        hex[i * 2] = digits[value >> 4U];
        hex[i * 2 + 1] = digits[value & 15U];
    }
    // Both Web Service directions echo the same byte-aligned opcode/transaction header.
    middleware::web_service::Message message{};
    const bool web = (service == 10 || service == 11 || service == 110 || service == 112)
                     && middleware::web_service::parse_request(body, message);
    ++g_status.frames;
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=bap_capture run=%llu seq=%zu dir=%s kind=%s stage=%s outer=%u svc=%u "
                      "header_id=%u status=%s ws=%u opcode=%u transaction=%u bytes=%zu shown=%zu "
                      "cut=%u redacted=%u family=%u version=%d flags=%u objects=%zu raw=%s",
                      static_cast<unsigned long long>(g_status.run),
                      g_status.frames,
                      kind == Kind::request ? "in" : "out",
                      kind_name(kind),
                      kind == Kind::request ? "decoded" : "encoded",
                      static_cast<unsigned>(middleware::bap::FrameType::encrypted),
                      static_cast<unsigned>(service),
                      correlation,
                      kind == Kind::response ? "200" : "none",
                      web ? 1U : 0U,
                      static_cast<unsigned>(message.opcode),
                      message.transactionId,
                      body.size(),
                      shown,
                      body.size() > shown ? 1U : 0U,
                      body.size() > shown && allowed < kBodyLimit ? 1U : 0U,
                      family != nullptr ? family->type : 0U,
                      family != nullptr ? family->version : 0,
                      family != nullptr ? family->flags : 0U,
                      family != nullptr ? family->objects.size() : std::size_t{0},
                      hex.data());
    if (g_status.frames == kFrameLimit) {
        g_status.enabled = false;
        g_enabled.store(false, std::memory_order_relaxed);
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=bap_capture stage=stop reason=frame_limit run=%llu frames=%zu",
                          static_cast<unsigned long long>(g_status.run),
                          g_status.frames);
    }
}

bool mark(std::string_view label) noexcept {
    const std::lock_guard lock(g_mutex);
    if (!g_status.enabled || g_status.markers >= kMarkerLimit) return false;
    std::array<char, 97> text{};
    const auto count = (std::min)(label.size(), text.size() - 1);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = static_cast<unsigned char>(label[i]);
        text[i] = value >= 32 && value <= 126 && value != '"' && value != '\\'
                      ? static_cast<char>(value)
                      : '_';
    }
    ++g_status.markers;
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=bap_capture dir=mark run=%llu seq=%zu marker=%zu label=\"%s\" cut=%u",
                      static_cast<unsigned long long>(g_status.run),
                      g_status.frames,
                      g_status.markers,
                      text.data(),
                      count < label.size() ? 1U : 0U);
    return true;
}

void record_account(std::uint32_t definitionId,
                    std::span<const std::byte> image) noexcept {
    namespace datagen = middleware::datagen;
    namespace layout = datagen::family4::account::layout;
    if (definitionId != datagen::kAccountObjectId || image.size() != sizeof(layout::Object))
        return;
    const auto& object = *reinterpret_cast<const layout::Object*>(image.data());
    if (g_storeAccountRows.fetch_add(1, std::memory_order_relaxed) < 8)
        core::log::writef(core::log::Channel::server, core::log::Level::info,
            "ev=store_sync stage=account_staged store_revision=%u store_state=%d",
            object.storeSyncRevision, static_cast<int>(object.storeSyncState));
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const std::lock_guard lock(g_mutex);
    if (!g_status.enabled || g_status.objects >= kObjectLimit) return;
    ++g_status.objects;
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=bap_capture run=%llu seq=%zu kind=account_object stage=staged "
                      "object_seq=%zu definition=%08X store_revision=%u store_state=%d",
                      static_cast<unsigned long long>(g_status.run),
                      g_status.frames,
                      g_status.objects,
                      definitionId,
                      object.storeSyncRevision,
                      static_cast<int>(object.storeSyncState));
}

} // namespace sunrise::server::bap::capture
