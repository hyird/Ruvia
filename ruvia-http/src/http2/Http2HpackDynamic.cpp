#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2OffsetVector.h"
#include "ruvia/http/detail/http2/Http2HpackStaticTable.h"

namespace ruvia::detail {

std::size_t HpackDecoder::entrySize(std::string_view name, std::string_view value) noexcept {
    return name.size() + value.size() + 32;
}

HpackError HpackDecoder::indexedHeader(std::uint32_t index, HeaderView& header) const noexcept {
    if (index == 0) {
        return HpackError::kInvalidIndex;
    }
    if (index <= kHpackStaticTableSize) {
        const auto& entry = hpackStaticHeaderAt(index);
        header = HeaderView{entry.name, entry.value};
        return HpackError::kNone;
    }
    const auto dynamicIndex = index - static_cast<std::uint32_t>(kHpackStaticTableSize);
    if (dynamicIndex == 0 || dynamicIndex > dynamicEntryCount()) {
        return HpackError::kInvalidIndex;
    }
    const auto& entry = dynamicEntryByNewestIndex(static_cast<std::size_t>(dynamicIndex - 1));
    header = HeaderView{entry.name, entry.value};
    return HpackError::kNone;
}

HpackError HpackDecoder::indexedName(std::uint32_t index, std::string_view& name) const noexcept {
    HeaderView header;
    if (const auto error = indexedHeader(index, header); error != HpackError::kNone) {
        return error;
    }
    name = header.name;
    return HpackError::kNone;
}

void HpackDecoder::addDynamic(std::string_view name, std::string_view value) {
    const auto size = entrySize(name, value);
    if (size > maxDynamicSize_) {
        clearDynamic();
        return;
    }

    // Copy name and value into owned storage BEFORE evicting. For a "Literal
    // Header Field with Incremental Indexing -- Indexed Name" whose name indexes a
    // dynamic entry (RFC 7541 6.2.1), `name` aliases that entry's heap buffer --
    // and RFC 7541 4.4 explicitly allows a new entry to reference the name of an
    // entry the same insertion evicts. evictDynamicToFit() -> compactDynamic()
    // move-assigns survivors over the evicted front slots (or clears the vector on
    // full eviction), freeing the referenced buffer; copying `name` afterwards
    // would then read freed memory. Materializing first makes the insert safe.
    Entry entry{
        std::pmr::string(name, resource_),
        std::pmr::string(value, resource_)};
    evictDynamicToFit(size);
    dynamic_.push_back(std::move(entry));
    dynamicSize_ += size;
}

std::size_t HpackDecoder::dynamicEntryCount() const noexcept {
    return dynamic_.size() - dynamicOffset_;
}

const HpackDecoder::Entry& HpackDecoder::dynamicEntryByNewestIndex(std::size_t newestIndex) const noexcept {
    return dynamic_[dynamic_.size() - newestIndex - 1];
}

void HpackDecoder::clearDynamic() noexcept {
    dynamic_.clear();
    dynamicSize_ = 0;
    dynamicOffset_ = 0;
}

void HpackDecoder::evictDynamicToFit(std::size_t entrySize) {
    const auto targetSize = maxDynamicSize_ - entrySize;
    while (dynamicSize_ > targetSize && dynamicOffset_ < dynamic_.size()) {
        const auto& entry = dynamic_[dynamicOffset_++];
        dynamicSize_ -= HpackDecoder::entrySize(entry.name, entry.value);
    }
    compactDynamic();
}

void HpackDecoder::evictDynamic() {
    while (dynamicSize_ > maxDynamicSize_ && dynamicOffset_ < dynamic_.size()) {
        const auto& entry = dynamic_[dynamicOffset_++];
        dynamicSize_ -= entrySize(entry.name, entry.value);
    }
    compactDynamic();
}

void HpackDecoder::compactDynamic() {
    http2CompactMovableOffsetVector(dynamic_, dynamicOffset_, 16);
}

}  // namespace ruvia::detail
