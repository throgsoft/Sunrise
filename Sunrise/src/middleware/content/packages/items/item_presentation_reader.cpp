#include "item_presentation_reader.h"

#include "../reader/block_cache.h"
#include "../reader/internal.h"
#include "../reader/package_table_cache.h"
#include "../tables/definition_index_table.h"
#include "../tables/internal.h"

namespace sunrise::middleware::content::packages::items {
namespace {
// Serialized layouts verified against the supported v38 installation. These are format
// identifiers and offsets, never an exported item-to-asset list.
constexpr std::uint32_t kStringBanksTag = 0x81A27211U;
constexpr std::uint32_t kIconsTag = 0x81A291C2U;
constexpr std::uint32_t kObjectivesTag = 0x81613CF5U;
constexpr std::size_t kTableLimit = 2 * 1024 * 1024;
constexpr std::size_t kTextLimit = 1024 * 1024;

bool array(std::span<const std::byte> bytes,
           std::uint32_t cls,
           std::size_t stride,
           tables::Array& rows) noexcept {
    return tables::find_array_at(bytes, 8, rows) && rows.elementClass == cls
           && rows.dataOffset <= bytes.size()
           && rows.count <= (bytes.size() - rows.dataOffset) / stride;
}
} // namespace

PresentationReader::~PresentationReader() noexcept {
    reader::close_files(scratch_);
}

bool PresentationReader::read(std::uint32_t tag,
                              std::size_t limit,
                              std::vector<std::byte>& output,
                              std::uint32_t& reference) noexcept {
    output.clear();
    if (tag < reader::layout::kTagBase || tag == 0xFFFFFFFFU) {
        return false;
    }
    const auto handle = tag - reader::layout::kTagBase;
    const auto package = static_cast<std::uint16_t>(handle >> reader::layout::kTagEntryBits);
    const auto index = handle & reader::layout::kTagEntryMask;
    const reader::PackageLocation* location = nullptr;
    reader::Header header{};
    reader::layout::EntryRecord entry{};
    if (!reader::resolve_latest(scratch_, source_.directory, package, location) || !location
        || !reader::block_cache::load_header(
            location->latestPath, package, location->patchIndex, scratch_, header)
        || index >= header.entryCount
        || !reader::table_cache::entry_record(scratch_, location->latestPath, header, index, entry)
        || reader::layout::placement(entry).size == 0
        || reader::layout::placement(entry).size > limit) {
        return false;
    }
    return reader::read_tag(source_, scratch_, tag, output, reference);
}

bool PresentationReader::initialize() noexcept {
    std::uint32_t cls{};
    tables::Array rows{};
    const bool ready = read(tables::kItemStringsIndexTag, kTableLimit, strings_, cls)
                       && cls == 0x80805CDBU
                       && array(strings_, tables::kItemStringsIndexRowClass, 24, rows)
                       && read(kStringBanksTag, kTableLimit, banks_, cls)
                       && cls == text::kStringBankIndexClass && array(banks_, 0x80805F9EU, 8, rows);
    // Optional domains fail independently, leaving names and numeric catalog entries usable.
    if (!read(kIconsTag, kTableLimit, icons_, cls) || cls != 0x80802951U
        || !array(icons_, 0x80802957U, 24, rows)) {
        icons_.clear();
    }
    if (!read(kObjectivesTag, kTableLimit, objectives_, cls) || cls != 0x808059ECU
        || !array(objectives_, 0x808059F0U, 64, rows)) {
        objectives_.clear();
    }
    return ready;
}

text::Reference PresentationReader::reference(std::span<const std::byte> bytes,
                                              std::size_t offset) const noexcept {
    std::uint16_t bank = 0xFFFF;
    text::Reference output{};
    tables::Array rows{};
    if (!tables::read(bytes, offset, bank) || bank == 0xFFFF
        || !tables::read(bytes, offset + 4, output.stringHash)
        || !array(banks_, 0x80805F9EU, 8, rows) || bank >= rows.count
        || !tables::read(std::span<const std::byte>{banks_},
                         rows.dataOffset + bank * 8 + 4,
                         output.containerTag)) {
        return {};
    }
    return output;
}

bool PresentationReader::item(std::uint16_t index, std::uint32_t hash, Display& output) noexcept {
    output = {};
    tables::Array rows{};
    tables::IndexRow row{};
    std::uint32_t cls{};
    if (!array(strings_, tables::kItemStringsIndexRowClass, 24, rows)
        || !tables::index_row(strings_, rows, index, row) || row.definitionHash != hash
        || !read(row.targetTag, 64 * 1024, blob_, cls) || cls != 0x80805CE1U) {
        return false;
    }
    const std::span<const std::byte> bytes{blob_};
    if (!tables::read(bytes, 0x80, output.iconIndex)) {
        return false;
    }
    output.name = reference(bytes, 132);
    output.description = reference(bytes, 152);
    output.itemType = reference(bytes, 144);
    return true;
}

bool PresentationReader::objective(std::uint16_t index,
                                   std::uint32_t hash,
                                   text::Reference& output) const noexcept {
    output = {};
    tables::Array rows{};
    std::uint32_t actual{};
    if (!array(objectives_, 0x808059F0U, 64, rows) || index >= rows.count) {
        return false;
    }
    const auto at = rows.dataOffset + index * 64;
    if (!tables::read(std::span<const std::byte>{objectives_}, at, actual) || actual != hash) {
        return false;
    }
    output = reference(objectives_, at + 24);
    return true;
}

bool PresentationReader::read_text(void* context,
                                   std::uint32_t tag,
                                   std::uint32_t expectedClass,
                                   std::vector<std::byte>& output) noexcept {
    std::uint32_t actual{};
    return static_cast<PresentationReader*>(context)->read(tag, kTextLimit, output, actual)
           && actual == expectedClass;
}

bool PresentationReader::resolve(std::span<const text::Reference> refs,
                                 text::Snapshot& output) noexcept {
    // The text resolver caches banks for this call. Cap its batch as well as each bank.
    return refs.size() <= 32 && text::resolve({this, &read_text, 0, 0}, refs, output);
}

bool PresentationReader::icon(std::uint16_t index, Icon& output) noexcept {
    output = {};
    tables::Array rows{};
    tables::IndexRow row{};
    std::uint32_t cls{}, tag{};
    if (index == kNoIcon || !array(icons_, 0x80802957U, 24, rows)
        || !tables::index_row(icons_, rows, index, row) || !read(row.targetTag, 128, blob_, cls)
        || cls != 0x80804A53U || blob_.size() != 128
        || !tables::read(std::span<const std::byte>{blob_}, 0x14, tag)
        || !read(tag, 132, blob_, cls) || cls != 0x80804A69U || blob_.size() != 132) {
        return false;
    }
    // Only the simple texture set is supported; animated and conditional images stay absent.
    // A sequence's first frame is the base icon, and the later frames are item states the
    // module does not track.
    const std::span<const std::byte> set{blob_};
    std::uint32_t kind{}, resourceClass{};
    tables::Array outer{}, inner{};
    if (!tables::read(set, 8, kind) || kind != 0 || !tables::read(set, 28, resourceClass)
        || resourceClass != 0x80804A67U || !tables::find_array_at(set, 32, outer)
        || outer.count == 0 || outer.elementClass != 0x80804A6CU
        || !tables::find_array_at(set, outer.dataOffset, inner) || inner.count == 0
        || inner.elementClass != 0x80804A6FU || !tables::read(set, inner.dataOffset, tag)
        || !read(tag, 40, blob_, cls) || blob_.size() != 40) {
        return false;
    }
    const std::span<const std::byte> header{blob_};
    std::uint32_t size{}, format{}, large{};
    std::uint16_t marker{}, depth{}, layers{};
    if (!tables::read(header, 0, size) || !tables::read(header, 4, format) || format != 28
        || !tables::read(header, 12, marker) || marker != 0xCAFE
        || !tables::read(header, 14, output.width) || !tables::read(header, 16, output.height)
        || !tables::read(header, 18, depth) || depth != 1 || !tables::read(header, 20, layers)
        || layers != 1 || !tables::read(header, 36, large) || large != 0xFFFFFFFFU
        || output.width == 0 || output.height == 0 || output.width > kIconSideLimit
        || output.height > kIconSideLimit
        || size != static_cast<std::uint32_t>(output.width) * output.height * 4) {
        output = {};
        return false;
    }
    // Texture headers use the entry reference as their pixel tag; verify the reciprocal link.
    const auto pixelsTag = cls;
    if (!read(pixelsTag, kIconByteLimit, output.rgba, cls) || cls != tag
        || output.rgba.size() != size) {
        output = {};
        return false;
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::items
