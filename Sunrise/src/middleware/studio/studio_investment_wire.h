#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace sunrise::middleware::studio::investment_wire {

/** Investment requests use their own local mapping. The magic reads "SSIV" in memory. */
inline constexpr std::uint32_t kMagic = 0x56495353;
inline constexpr std::uint32_t kVersion = 2;
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Investment.%lu";
/** How often the game looks for a missing editor mapping. */
inline constexpr std::uint64_t kConnectIntervalMs = 1'000;
/** An editor that has not refreshed its heartbeat this long is gone. */
inline constexpr std::uint64_t kHeartbeatTimeoutMs = 3'000;
/**
 * Size of the NUL-terminated ASCII reason carried by a refusal, an invalid request or a grant that
 * stopped part way.
 */
inline constexpr std::size_t kReasonBytes = 64;

/** Studio and the game use these fixed offsets. The control block is the same in every version. */
inline constexpr std::size_t kControlBytes = 64;
inline constexpr std::size_t kHeartbeatOffset = 16, kGameVersionOffset = 24, kGamePidOffset = 28;
inline constexpr std::size_t kCapabilitiesOffset = 32, kGameStartedOffset = 40;
inline constexpr std::size_t kRequestOffset = 48, kResponseOffset = 52;
inline constexpr std::size_t kRequestBlockOffset = 64, kResponseBlockOffset = 144;
inline constexpr std::size_t kResponsePayloadOffset = 256;

// --- Capacities: the server asserts each one equals the State constant that owns it ---

/** The profile array's native slot count. */
inline constexpr std::size_t kProfileRowCapacity = 701;
/** Unequipped instances (350 native rows less 17 equipped) plus one per equipment slot. */
inline constexpr std::size_t kCharacterItemRecordCapacity = 333 + 17;
/** Stacks share the 333 unequipped character rows with instances. */
inline constexpr std::size_t kCharacterStackRecordCapacity = 333;
/** Every bucket id except the all-ones unavailable id. */
inline constexpr std::size_t kBucketRecordCapacity = 255;
inline constexpr std::size_t kPlugRecordCapacity = 12;
inline constexpr std::size_t kCharacterRecordCapacity = 3;
inline constexpr std::size_t kSeasonPassRowCapacity = 256;
/** Held pursuits are unequipped character instances. */
inline constexpr std::size_t kHeldPursuitCapacity = 333;
inline constexpr std::size_t kPursuitObjectiveCapacity = 7;
/**
 * Sources one unlockValues request can carry. The fixed block offsets set this limit: the request
 * block ends where the response block begins, which leaves room after the request header for a
 * 4-byte count and nine 4-byte sources.
 */
inline constexpr std::size_t kUnlockReadCapacity = 9;
/** An item whose definition names no installed bucket. */
inline constexpr std::uint8_t kNoBucket = 0xFF;
/** A character item that occupies no equipment slot. */
inline constexpr std::uint8_t kUnequipped = 0xFF;

// --- Enumerations ---

/** The game advertises each opcode it serves by setting the capability bit at its value. */
enum class Opcode : std::uint32_t {
    none,
    hello,
    inventorySnapshot,
    seasonPassSnapshot,
    pursuitSnapshot,
    grantItem,
    setQuantity,
    setSeasonalExperience,
    claimSeasonPassReward,
    resetSeasonPassClaim,
    readReceipt,
    acknowledgeReceipt,
    withdrawOperation,
    unlockValues,
    setPursuitObjective,
    completePursuitObjectives,
    count,
};
static_assert(static_cast<std::uint32_t>(Opcode::count) <= 64);

/** Transport outcome of one request. A mutation's own outcome is in its receipt. */
enum class Status : std::uint32_t { none, ok, invalid, refused };

/** Unknown means the game holds no receipt with that id in this process. */
enum class ReceiptState : std::uint32_t {
    unknown,
    queued,
    deferred,
    refused,
    published,
    /**
     * The game saved the change but could not send its frame, so the client resyncs the account.
     * A grant ends at that step: the rest of its quantity is never granted, and changedRows counts
     * the rows its steps changed, not the quantity they granted.
     */
    committedUnpublished,
};

/** Why a non-terminal operation is waiting. */
enum class Wait : std::uint32_t { none, publicationPeer, accountResync, catalog };

enum class InventoryScope : std::uint8_t { profile, characterItem, characterStack };

/** What one source reads. Zero is never valid, so an unused source is all zero. */
enum class UnlockKind : std::uint8_t { flag = 1, value };

/** The saved bank one source reads. The profile bank holds only flags. */
enum class UnlockBank : std::uint8_t { account = 1, profile, character };

/** A source is invalid when its bank cannot hold that kind or its index is outside the bank. */
enum class UnlockReadState : std::uint8_t { invalid, known };

/** Why a held pursuit's completion stands where it does, in the game's priority order. */
enum class PursuitReason : std::uint16_t {
    contractUnavailable,
    operationBodyUnavailable,
    valueDestinationUnavailable,
    sweepSuppressed,
    objectivesIncomplete,
    valueAlreadyNonzero,
    objectiveSourceUnavailable,
    valueSourceUnavailable,
    automaticConditionsMet,
    manualActionNotEstablished,
    itemProgressNotSaved,
    externalValue,
};

/** One bit per game catalog that has finished loading. */
enum Readiness : std::uint32_t {
    readyItems = 1,
    readyItemDetails = 2,
    readyBuckets = 4,
    readyProgressions = 8,
    readySeasonPass = 16,
    readyRewards = 32,
    readyObjectives = 64,
    readyPursuits = 128,
};

/** Whether a signed-in game client can show a change to the bound account. */
enum Publication : std::uint32_t {
    peerPresent = 1,
    peerOwnsAccount = 2,
    peerResyncArmed = 4,
};

enum ItemRecordFlags : std::uint8_t {
    itemSeen = 1,
};

enum SeasonPassRowFlags : std::uint16_t {
    rowAvailable = 1,
    rowClaimed = 2,
    rowAcquired = 4,
    rowAutomatic = 8,
    rowRankMet = 16,
    rowConditionKnown = 32,
    rowConditionMet = 64,
};

enum PursuitFlags : std::uint16_t {
    pursuitBounty = 1,
    pursuitBindingMatches = 2,
    pursuitConditionsKnown = 4,
    pursuitObjectivesKnown = 8,
    pursuitObjectivesComplete = 16,
    pursuitEligible = 32,
    pursuitEffectsKnown = 64,
};

enum ObjectiveFlags : std::uint8_t {
    objectiveKnown = 1,
    objectiveComplete = 2,
    objectiveSkipped = 4,
};

/** @return The capability bit the game sets when it serves this opcode. */
[[nodiscard]] constexpr std::uint64_t capability(Opcode opcode) noexcept {
    return std::uint64_t{1} << static_cast<std::uint32_t>(opcode);
}

/** @return True for opcodes that submit an operation against an expected context. */
[[nodiscard]] constexpr bool is_mutation(Opcode opcode) noexcept {
    return (opcode >= Opcode::grantItem && opcode <= Opcode::resetSeasonPassClaim)
           || opcode == Opcode::setPursuitObjective || opcode == Opcode::completePursuitObjectives;
}

/** @return True for opcodes that name an earlier mutation's operation id. */
[[nodiscard]] constexpr bool is_receipt_request(Opcode opcode) noexcept {
    return opcode >= Opcode::readReceipt && opcode <= Opcode::withdrawOperation;
}

// --- Identity block ---

/** The account, selected character and package content one request or answer is bound to. */
struct Context final {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    /** The build-data content hash of the installed and mod packages. */
    std::uint64_t contentHash{};
};
static_assert(sizeof(Context) == 24);
static_assert(offsetof(Context, characterSoid) == 8 && offsetof(Context, contentHash) == 16);

// --- Request payloads ---

/**
 * quantity must be positive and no more than the account can hold of the item at once. For an
 * inventory item, that is a full stack in every row of its own bucket and of its recovery bucket.
 * A larger quantity is refused as "quantity".
 */
struct GrantItemRequest final {
    std::uint32_t itemHash{};
    std::uint32_t quantity{};
};
static_assert(sizeof(GrantItemRequest) == 8 && offsetof(GrantItemRequest, quantity) == 4);

/**
 * A quantity of zero removes the row. instanceSoid is zero for plain profile rows and character
 * stacks. A character row belongs to the expected context's character.
 */
struct SetQuantityRequest final {
    InventoryScope scope{};
    std::array<std::uint8_t, 7> reserved{};
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t expectedSerial{};
    std::int32_t expectedQuantity{};
    std::int32_t quantity{};
};
static_assert(sizeof(SetQuantityRequest) == 32);
static_assert(offsetof(SetQuantityRequest, reserved) == 1
              && offsetof(SetQuantityRequest, instanceSoid) == 8
              && offsetof(SetQuantityRequest, definitionHash) == 16
              && offsetof(SetQuantityRequest, expectedSerial) == 20
              && offsetof(SetQuantityRequest, expectedQuantity) == 24
              && offsetof(SetQuantityRequest, quantity) == 28);

struct SetSeasonalExperienceRequest final {
    std::int32_t expected{};
    std::int32_t replacement{};
};
static_assert(sizeof(SetSeasonalExperienceRequest) == 8
              && offsetof(SetSeasonalExperienceRequest, replacement) == 4);

/**
 * Names one pass row for a claim or a claim reset. rowIndex is the native claim index, and
 * itemHash must match the item that row grants.
 */
struct SeasonPassRowRequest final {
    std::uint16_t rowIndex{};
    std::uint16_t reserved{};
    std::uint32_t itemHash{};
};
static_assert(sizeof(SeasonPassRowRequest) == 8 && offsetof(SeasonPassRowRequest, reserved) == 2
              && offsetof(SeasonPassRowRequest, itemHash) == 4);

/** One saved flag or value, named by its bank and row. */
struct UnlockSource final {
    UnlockKind kind{};
    UnlockBank bank{};
    std::uint16_t index{};
};
static_assert(sizeof(UnlockSource) == 4 && offsetof(UnlockSource, bank) == 1
              && offsetof(UnlockSource, index) == 2);

/** Sources after count must be zero. */
struct UnlockValuesRequest final {
    std::uint16_t count{};
    std::uint16_t reserved{};
    std::array<UnlockSource, kUnlockReadCapacity> sources{};
};
static_assert(sizeof(UnlockValuesRequest) == 40);
static_assert(offsetof(UnlockValuesRequest, reserved) == 2
              && offsetof(UnlockValuesRequest, sources) == 4);

/**
 * Sets the saved row one objective of a held pursuit reads. expected is the value the editor read;
 * a flag reads 0 or 1.
 */
struct SetPursuitObjectiveRequest final {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t expectedSerial{};
    std::uint32_t objectiveHash{};
    /** Position in the installed contract, below kPursuitObjectiveCapacity. */
    std::uint8_t ordinal{};
    std::uint8_t reserved{};
    std::uint16_t reserved2{};
    std::int32_t expected{};
    std::int32_t value{};
};
static_assert(sizeof(SetPursuitObjectiveRequest) == 32);
static_assert(offsetof(SetPursuitObjectiveRequest, itemHash) == 8
              && offsetof(SetPursuitObjectiveRequest, expectedSerial) == 12
              && offsetof(SetPursuitObjectiveRequest, objectiveHash) == 16
              && offsetof(SetPursuitObjectiveRequest, ordinal) == 20
              && offsetof(SetPursuitObjectiveRequest, reserved) == 21
              && offsetof(SetPursuitObjectiveRequest, reserved2) == 22
              && offsetof(SetPursuitObjectiveRequest, expected) == 24
              && offsetof(SetPursuitObjectiveRequest, value) == 28);

struct CompletePursuitObjectivesRequest final {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t expectedSerial{};
};
static_assert(sizeof(CompletePursuitObjectivesRequest) == 16);
static_assert(offsetof(CompletePursuitObjectivesRequest, itemHash) == 8
              && offsetof(CompletePursuitObjectivesRequest, expectedSerial) == 12);

inline constexpr std::size_t kRequestPayloadBytes =
    (std::max)({sizeof(GrantItemRequest),
                sizeof(SetQuantityRequest),
                sizeof(SetSeasonalExperienceRequest),
                sizeof(SeasonPassRowRequest),
                sizeof(UnlockValuesRequest),
                sizeof(SetPursuitObjectiveRequest),
                sizeof(CompletePursuitObjectivesRequest)});
static_assert(kRequestPayloadBytes == 40);
// The unlock read is the largest request, so its capacity sets the payload size. The response
// block's fixed offset, asserted with the mapping below, keeps that payload from growing.
static_assert(sizeof(UnlockValuesRequest) == kRequestPayloadBytes);

/** @return The payload size a request with this opcode carries; zero for those with none. */
[[nodiscard]] constexpr std::size_t request_bytes(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::grantItem:
        return sizeof(GrantItemRequest);
    case Opcode::setQuantity:
        return sizeof(SetQuantityRequest);
    case Opcode::setSeasonalExperience:
        return sizeof(SetSeasonalExperienceRequest);
    case Opcode::claimSeasonPassReward:
    case Opcode::resetSeasonPassClaim:
        return sizeof(SeasonPassRowRequest);
    case Opcode::unlockValues:
        return sizeof(UnlockValuesRequest);
    case Opcode::setPursuitObjective:
        return sizeof(SetPursuitObjectiveRequest);
    case Opcode::completePursuitObjectives:
        return sizeof(CompletePursuitObjectivesRequest);
    default:
        return 0;
    }
}

// --- Response payloads: readers decode each counted prefix, and every later slot is zero ---

struct CharacterEntry final {
    std::uint64_t soid{};
    std::uint8_t characterClass{};
    std::uint8_t selected{};
    std::uint16_t reserved{};
    std::uint32_t reserved2{};
};
static_assert(sizeof(CharacterEntry) == 16);
static_assert(offsetof(CharacterEntry, characterClass) == 8
              && offsetof(CharacterEntry, selected) == 9 && offsetof(CharacterEntry, reserved) == 10
              && offsetof(CharacterEntry, reserved2) == 12);

/** gameStarted identifies the game process instance. Receipts do not outlive it. */
struct Hello final {
    std::uint32_t protocolVersion{};
    std::uint32_t readiness{};
    std::uint64_t capabilities{};
    std::uint64_t gameStarted{};
    std::uint32_t characterCount{};
    std::uint32_t publication{};
    std::array<CharacterEntry, kCharacterRecordCapacity> characters{};
    std::uint64_t investmentRevision{};
};
static_assert(sizeof(Hello) == 88);
static_assert(offsetof(Hello, readiness) == 4 && offsetof(Hello, capabilities) == 8
              && offsetof(Hello, gameStarted) == 16 && offsetof(Hello, characterCount) == 24
              && offsetof(Hello, publication) == 28 && offsetof(Hello, characters) == 32
              && offsetof(Hello, investmentRevision) == 80);

/** Mirrors the State bucket descriptor field for field. */
struct Bucket final {
    std::uint8_t bucketId{};
    std::uint8_t arraySelector{};
    std::uint16_t firstSlot{};
    std::uint16_t slotCount{};
    /** -1 for a bucket that equips nothing. */
    std::int8_t equipmentSlot{};
    std::uint8_t reserved{};
};
static_assert(sizeof(Bucket) == 8);
static_assert(offsetof(Bucket, arraySelector) == 1 && offsetof(Bucket, firstSlot) == 2
              && offsetof(Bucket, slotCount) == 4 && offsetof(Bucket, equipmentSlot) == 6
              && offsetof(Bucket, reserved) == 7);

struct ProfileRow final {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    /** kNoBucket when the definition names no installed bucket. */
    std::uint8_t bucketId{};
    /** ItemRecordFlags. */
    std::uint8_t flags{};
    std::uint16_t reserved{};
};
static_assert(sizeof(ProfileRow) == 24);
static_assert(offsetof(ProfileRow, definitionHash) == 8 && offsetof(ProfileRow, quantity) == 12
              && offsetof(ProfileRow, mutationSerial) == 16 && offsetof(ProfileRow, bucketId) == 20
              && offsetof(ProfileRow, flags) == 21 && offsetof(ProfileRow, reserved) == 22);

/** One equipped or unequipped instance of the selected character. */
struct CharacterItem final {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::int32_t level{};
    /** Native accumulated item-state bits. */
    std::uint32_t itemFlags{};
    std::uint8_t bucketId{};
    /** The State equipment slot, or kUnequipped. */
    std::uint8_t equipmentSlot{};
    /** Zero for native socket defaults, one for authored lanes. */
    std::uint8_t socketPolicy{};
    std::uint8_t plugCount{};
    /** Bit i is set when lane i holds a plug, so a clear bit below plugCount is an empty lane. */
    std::uint16_t plugPresent{};
    /** ItemRecordFlags. */
    std::uint8_t flags{};
    std::uint8_t reserved{};
    std::array<std::uint32_t, kPlugRecordCapacity> plugs{};
    std::uint32_t reserved2{};
};
static_assert(sizeof(CharacterItem) == 88);
static_assert(
    offsetof(CharacterItem, definitionHash) == 8 && offsetof(CharacterItem, quantity) == 12
    && offsetof(CharacterItem, mutationSerial) == 16 && offsetof(CharacterItem, level) == 20
    && offsetof(CharacterItem, itemFlags) == 24 && offsetof(CharacterItem, bucketId) == 28
    && offsetof(CharacterItem, equipmentSlot) == 29 && offsetof(CharacterItem, socketPolicy) == 30
    && offsetof(CharacterItem, plugCount) == 31 && offsetof(CharacterItem, plugPresent) == 32
    && offsetof(CharacterItem, flags) == 34 && offsetof(CharacterItem, reserved) == 35
    && offsetof(CharacterItem, plugs) == 36 && offsetof(CharacterItem, reserved2) == 84);
static_assert(kPlugRecordCapacity
                  <= std::numeric_limits<decltype(CharacterItem::plugPresent)>::digits,
              "plugPresent holds one bit per lane");

struct CharacterStack final {
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::uint8_t bucketId{};
    std::array<std::uint8_t, 3> reserved{};
};
static_assert(sizeof(CharacterStack) == 16);
static_assert(offsetof(CharacterStack, quantity) == 4
              && offsetof(CharacterStack, mutationSerial) == 8
              && offsetof(CharacterStack, bucketId) == 12
              && offsetof(CharacterStack, reserved) == 13);

/** The selected character and the profile, read under one State lock. */
struct InventorySnapshot final {
    std::uint16_t bucketCount{};
    std::uint16_t profileCount{};
    /** Equipped and unequipped instances. */
    std::uint16_t itemCount{};
    std::uint16_t stackCount{};
    std::uint64_t investmentRevision{};
    std::array<Bucket, kBucketRecordCapacity> buckets{};
    std::array<ProfileRow, kProfileRowCapacity> profile{};
    std::array<CharacterItem, kCharacterItemRecordCapacity> items{};
    std::array<CharacterStack, kCharacterStackRecordCapacity> stacks{};
};
static_assert(sizeof(InventorySnapshot) == 55'008);
static_assert(offsetof(InventorySnapshot, profileCount) == 2
              && offsetof(InventorySnapshot, itemCount) == 4
              && offsetof(InventorySnapshot, stackCount) == 6
              && offsetof(InventorySnapshot, investmentRevision) == 8
              && offsetof(InventorySnapshot, buckets) == 16
              && offsetof(InventorySnapshot, profile) == 2'056
              && offsetof(InventorySnapshot, items) == 18'880
              && offsetof(InventorySnapshot, stacks) == 49'680);

/** The row index is its array position, which is the native claim index. */
struct SeasonPassRow final {
    /** Zero for an unavailable placeholder row. */
    std::uint32_t itemHash{};
    std::uint32_t quantity{};
    std::uint8_t requiredRank{};
    std::uint8_t reserved{};
    /** SeasonPassRowFlags. */
    std::uint16_t flags{};
    std::uint32_t reserved2{};
};
static_assert(sizeof(SeasonPassRow) == 16);
static_assert(offsetof(SeasonPassRow, quantity) == 4 && offsetof(SeasonPassRow, requiredRank) == 8
              && offsetof(SeasonPassRow, reserved) == 9 && offsetof(SeasonPassRow, flags) == 10
              && offsetof(SeasonPassRow, reserved2) == 12);

struct SeasonPassSnapshot final {
    std::int32_t experience{};
    std::uint16_t rank{};
    /** Catalog rows, including unavailable placeholders. */
    std::uint16_t rowCount{};
    std::uint64_t investmentRevision{};
    std::array<SeasonPassRow, kSeasonPassRowCapacity> rows{};
};
static_assert(sizeof(SeasonPassSnapshot) == 4'112);
static_assert(offsetof(SeasonPassSnapshot, rank) == 4 && offsetof(SeasonPassSnapshot, rowCount) == 6
              && offsetof(SeasonPassSnapshot, investmentRevision) == 8
              && offsetof(SeasonPassSnapshot, rows) == 16);

struct PursuitObjective final {
    std::uint32_t objectiveHash{};
    std::int32_t value{};
    std::int32_t target{};
    /** ObjectiveFlags. */
    std::uint8_t flags{};
    std::array<std::uint8_t, 3> reserved{};
};
static_assert(sizeof(PursuitObjective) == 16);
static_assert(offsetof(PursuitObjective, value) == 4 && offsetof(PursuitObjective, target) == 8
              && offsetof(PursuitObjective, flags) == 12
              && offsetof(PursuitObjective, reserved) == 13);

struct HeldPursuit final {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t mutationSerial{};
    std::int32_t quantity{};
    std::uint32_t itemFlags{};
    /** PursuitFlags. */
    std::uint16_t flags{};
    std::uint8_t objectiveCount{};
    std::uint8_t reserved{};
    PursuitReason reason{};
    std::uint16_t reserved2{};
    std::array<PursuitObjective, kPursuitObjectiveCapacity> objectives{};
};
static_assert(sizeof(HeldPursuit) == 144);
static_assert(offsetof(HeldPursuit, itemHash) == 8 && offsetof(HeldPursuit, mutationSerial) == 12
              && offsetof(HeldPursuit, quantity) == 16 && offsetof(HeldPursuit, itemFlags) == 20
              && offsetof(HeldPursuit, flags) == 24 && offsetof(HeldPursuit, objectiveCount) == 26
              && offsetof(HeldPursuit, reserved) == 27 && offsetof(HeldPursuit, reason) == 28
              && offsetof(HeldPursuit, reserved2) == 30 && offsetof(HeldPursuit, objectives) == 32);

struct PursuitSnapshot final {
    std::uint16_t heldCount{};
    std::uint16_t reserved{};
    std::uint32_t reserved2{};
    std::uint64_t investmentRevision{};
    std::array<HeldPursuit, kHeldPursuitCapacity> held{};
};
static_assert(sizeof(PursuitSnapshot) == 47'968);
static_assert(offsetof(PursuitSnapshot, reserved) == 2 && offsetof(PursuitSnapshot, reserved2) == 4
              && offsetof(PursuitSnapshot, investmentRevision) == 8
              && offsetof(PursuitSnapshot, held) == 16);

/** One mutation's retained outcome. Every field after operationId is zero for an unknown id. */
struct Receipt final {
    std::uint64_t operationId{};
    Opcode opcode{};
    ReceiptState state{};
    std::uint32_t reserved{};
    Wait wait{};
    /** Family-4 version of the last frame the operation published. */
    std::int32_t family4Version{};
    std::uint32_t changedRows{};
    /** Server GetTickCount64 values. finishedTick stays zero while the receipt is pending. */
    std::uint64_t submittedTick{};
    std::uint64_t finishedTick{};
};
static_assert(sizeof(Receipt) == 48);
static_assert(offsetof(Receipt, opcode) == 8 && offsetof(Receipt, state) == 12
              && offsetof(Receipt, reserved) == 16 && offsetof(Receipt, wait) == 20
              && offsetof(Receipt, family4Version) == 24 && offsetof(Receipt, changedRows) == 28
              && offsetof(Receipt, submittedTick) == 32 && offsetof(Receipt, finishedTick) == 40);

struct UnlockValue final {
    UnlockReadState state{};
    std::uint8_t reserved{};
    std::uint16_t reserved2{};
    /** A flag reads 0 or 1. */
    std::int32_t value{};
};
static_assert(sizeof(UnlockValue) == 8);
static_assert(offsetof(UnlockValue, reserved) == 1 && offsetof(UnlockValue, reserved2) == 2
              && offsetof(UnlockValue, value) == 4);

/** Value i answers request source i, all read from one saved state of the selected character. */
struct UnlockValues final {
    std::uint16_t count{};
    std::uint16_t reserved{};
    std::uint32_t reserved2{};
    std::uint64_t investmentRevision{};
    std::array<UnlockValue, kUnlockReadCapacity> values{};
};
static_assert(sizeof(UnlockValues) == 88);
static_assert(offsetof(UnlockValues, reserved) == 2 && offsetof(UnlockValues, reserved2) == 4
              && offsetof(UnlockValues, investmentRevision) == 8
              && offsetof(UnlockValues, values) == 16);

inline constexpr std::size_t kResponsePayloadBytes = (std::max)({sizeof(Hello),
                                                                 sizeof(InventorySnapshot),
                                                                 sizeof(SeasonPassSnapshot),
                                                                 sizeof(PursuitSnapshot),
                                                                 sizeof(Receipt),
                                                                 sizeof(UnlockValues)});
static_assert(kResponsePayloadBytes == 55'008);

// --- The mapping ---

/** Studio writes the header, heartbeat and request id, and the game writes the rest. */
struct alignas(8) Control final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t bytes{};
    std::uint32_t editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    /** Written whenever the magic matches, so Studio can explain a version mismatch. */
    std::uint32_t gameVersion{};
    std::uint32_t gamePid{};
    std::uint64_t capabilities{};
    /** The game process's creation FILETIME. */
    std::uint64_t gameStarted{};
    alignas(4) volatile std::int32_t request{};
    alignas(4) volatile std::int32_t response{};
    std::uint64_t reserved{};
};
static_assert(sizeof(Control) == kControlBytes);
static_assert(offsetof(Control, version) == 4 && offsetof(Control, bytes) == 8
              && offsetof(Control, editorPid) == 12);
static_assert(offsetof(Control, heartbeat) == kHeartbeatOffset);
static_assert(offsetof(Control, gameVersion) == kGameVersionOffset);
static_assert(offsetof(Control, gamePid) == kGamePidOffset);
static_assert(offsetof(Control, capabilities) == kCapabilitiesOffset);
static_assert(offsetof(Control, gameStarted) == kGameStartedOffset);
static_assert(offsetof(Control, request) == kRequestOffset);
static_assert(offsetof(Control, response) == kResponseOffset);
static_assert(offsetof(Control, reserved) == 56);

/** Only Studio writes the request block. */
struct Request final {
    Opcode opcode{};
    /** Equals request_bytes(opcode). */
    std::uint32_t payloadBytes{};
    /** Nonzero exactly for mutations and receipt requests. */
    std::uint64_t operationId{};
    /** Complete for mutations, zero otherwise. */
    Context expected{};
    std::array<std::byte, kRequestPayloadBytes> payload{};
};
static_assert(sizeof(Request) == 80);
static_assert(offsetof(Request, payloadBytes) == 4 && offsetof(Request, operationId) == 8
              && offsetof(Request, expected) == 16 && offsetof(Request, payload) == 40);

/** Only the game writes the response block. */
struct Response final {
    Status status{};
    std::uint32_t reserved{};
    std::uint64_t operationId{};
    Opcode opcode{};
    /** The opcode's fixed payload size, or zero. */
    std::uint32_t payloadBytes{};
    /** The identity the payload is bound to, or zero for an invalid request. */
    Context context{};
    std::array<char, kReasonBytes> reason{};
    std::array<std::byte, kResponsePayloadBytes> payload{};
};
static_assert(offsetof(Response, reserved) == 4 && offsetof(Response, operationId) == 8
              && offsetof(Response, opcode) == 16 && offsetof(Response, payloadBytes) == 20
              && offsetof(Response, context) == 24 && offsetof(Response, reason) == 48
              && offsetof(Response, payload) == 112);

/** Each side writes its block in full before writing the id that announces it. */
struct alignas(8) SharedInvestment final {
    Control control;
    Request request;
    Response response;
};

inline constexpr std::size_t kMappingBytes = kControlBytes + sizeof(Request) + sizeof(Response);
static_assert(offsetof(SharedInvestment, request) == kRequestBlockOffset);
static_assert(offsetof(SharedInvestment, response) == kResponseBlockOffset);
static_assert(kResponseBlockOffset + offsetof(Response, payload) == kResponsePayloadOffset);
static_assert(sizeof(SharedInvestment) == kMappingBytes && kMappingBytes == 55'264);
static_assert(std::is_standard_layout_v<SharedInvestment>);

} // namespace sunrise::middleware::studio::investment_wire
