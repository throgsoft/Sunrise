#include "items_presentation_reader.h"

#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../middleware/content/packages/tables/field_reader.h"
#include "middleware/content/packages/tables/texture_reader.h"

namespace sunrise::client::ui::items::presentation {
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace {
// Presentation tables and resource classes in the v38 package format.
constexpr std::uint32_t kStringBanksTag = 0x81A27211U;
constexpr std::uint32_t kIconsTag = 0x81A291C2U;
constexpr std::uint32_t kItemStringsIndexClass = 0x80805CDBU;
constexpr std::uint32_t kItemStringsClass = 0x80805CE1U;
constexpr std::uint32_t kStringBankRowClass = 0x80805F9EU;
constexpr std::uint32_t kIconIndexClass = 0x80802951U;
constexpr std::uint32_t kIconRowClass = 0x80802957U;
constexpr std::uint32_t kIconClass = 0x80804A53U;
constexpr std::uint32_t kImageSetClass = 0x80804A69U;
constexpr std::uint32_t kImageResourceClass = 0x80804A67U;
constexpr std::uint32_t kImageFrameClass = 0x80804A6CU;
constexpr std::uint32_t kTextureRowClass = 0x80804A6FU;
constexpr std::size_t kStringBankRowStride = 8;
constexpr std::size_t kIconDefinitionSize = 128;
constexpr std::size_t kTableLimit = 2 * 1024 * 1024;
constexpr std::size_t kTextLimit = 1024 * 1024;
constexpr std::size_t kItemStringsLimit = 64 * 1024;
constexpr std::size_t kTextBatchLimit = 32;

bool array(std::span<const std::byte> bytes,
           std::uint32_t cls,
           std::size_t stride,
           tables::Array& rows) noexcept {
    return tables::read_array(bytes, tables::kTableArrayDescriptor, cls, stride, rows)
           && rows.count != 0;
}
} // namespace

PresentationReader::~PresentationReader() noexcept {
    reader::close_files(scratch_);
}

bool PresentationReader::read(std::uint32_t tag,
                              std::size_t limit,
                              std::vector<std::byte>& output,
                              std::uint32_t& reference) noexcept {
    return reader::read_tag(source_, scratch_, tag, output, reference, limit);
}

bool PresentationReader::initialize() noexcept {
    std::uint32_t cls{};
    tables::Array rows{};
    const bool ready =
        read(tables::kItemStringsIndexTag, kTableLimit, strings_, cls)
        && cls == kItemStringsIndexClass
        && array(strings_, tables::kItemStringsIndexRowClass, tables::kItemIndexRowStride, rows)
        && read(kStringBanksTag, kTableLimit, banks_, cls) && cls == text::kStringBankIndexClass
        && array(banks_, kStringBankRowClass, kStringBankRowStride, rows);
    // Optional domains fail independently, leaving names and numeric catalog entries usable.
    if (!read(kIconsTag, kTableLimit, icons_, cls) || cls != kIconIndexClass
        || !array(icons_, kIconRowClass, tables::kItemIndexRowStride, rows)) {
        icons_.clear();
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
        || !array(banks_, kStringBankRowClass, kStringBankRowStride, rows) || bank >= rows.count
        || !tables::read(std::span<const std::byte>{banks_},
                         rows.dataOffset + bank * kStringBankRowStride + 4,
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
    if (!array(strings_, tables::kItemStringsIndexRowClass, tables::kItemIndexRowStride, rows)
        || !tables::index_row(strings_, rows, index, row) || row.definitionHash != hash
        || !read(row.targetTag, kItemStringsLimit, blob_, cls) || cls != kItemStringsClass) {
        return false;
    }
    const std::span<const std::byte> bytes{blob_};
    if (!tables::read(bytes, 0x80, output.iconIndex)) {
        return false;
    }
    output.name = reference(bytes, 132);
    output.description = reference(bytes, 152);
    output.itemType = reference(bytes, tables::kItemStringsTypePairOffset);
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
    return refs.size() <= kTextBatchLimit && text::resolve({this, &read_text, 0, 0}, refs, output);
}

bool PresentationReader::icon(std::uint16_t index, Icon& output) noexcept {
    output = {};
    tables::Array rows{};
    tables::IndexRow row{};
    std::uint32_t cls{}, tag{};
    if (index == kNoIcon || !array(icons_, kIconRowClass, tables::kItemIndexRowStride, rows)
        || !tables::index_row(icons_, rows, index, row)
        || !read(row.targetTag, kIconDefinitionSize, blob_, cls) || cls != kIconClass
        || blob_.size() != kIconDefinitionSize
        || !tables::read(std::span<const std::byte>{blob_}, 0x14, tag)
        || !read(tag, kTableLimit, blob_, cls) || cls != kImageSetClass) {
        return false;
    }
    // The first image supplies the base icon; later images represent item states.
    const std::span<const std::byte> set{blob_};
    std::uint32_t kind{}, resourceClass{};
    tables::Array outer{}, inner{};
    if (!tables::read(set, 8, kind) || kind != 0 || !tables::read(set, 28, resourceClass)
        || resourceClass != kImageResourceClass || !tables::find_array_at(set, 32, outer)
        || outer.count == 0 || outer.elementClass != kImageFrameClass
        || outer.dataOffset > set.size() || outer.count > (set.size() - outer.dataOffset) / 16
        || !tables::find_array_at(set, outer.dataOffset, inner) || inner.count == 0
        || inner.elementClass != kTextureRowClass || inner.dataOffset > set.size()
        || inner.count > (set.size() - inner.dataOffset) / sizeof(tag)
        || !tables::read(set, inner.dataOffset, tag)
        || !read(tag, tables::kTextureHeaderSize, blob_, cls)
        || blob_.size() != tables::kTextureHeaderSize) {
        return false;
    }
    tables::Rgba8Texture header{};
    if (!tables::rgba8_texture(blob_, header) || header.width > kIconSideLimit
        || header.height > kIconSideLimit
        || header.byteCount
               != static_cast<std::uint32_t>(header.width) * header.height
                      * tables::kRgba8BytesPerPixel) {
        return false;
    }
    output.width = header.width;
    output.height = header.height;
    // Texture headers use the entry reference as their pixel tag; verify the reciprocal link.
    const auto pixelsTag = cls;
    if (!read(pixelsTag, kIconByteLimit, output.rgba, cls) || cls != tag
        || output.rgba.size() != header.byteCount) {
        output = {};
        return false;
    }
    return true;
}

} // namespace sunrise::client::ui::items::presentation
