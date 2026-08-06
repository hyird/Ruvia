#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/frame/Http2OffsetVector.h"
#include "ruvia/http/detail/http2/hpack/Http2HpackStaticTable.h"

#include <exception>

namespace ruvia::detail {

std::size_t HpackDecoder::entrySize(std::string_view name, std::string_view value) noexcept {
    return name.size() + value.size() + 32;
}

HpackDecoder::StepResult HpackDecoder::indexedHeader(std::uint32_t index, HeaderView& header) const noexcept {
    if (index == 0) {
        return HpackDecodeError::kInvalidIndex;
    }
    if (index <= kHpackStaticTableSize) {
        const auto& entry = hpackStaticHeaderAt(index);
        header = HeaderView{entry.name, entry.value};
        return std::nullopt;
    }
    const auto dynamicIndex = index - static_cast<std::uint32_t>(kHpackStaticTableSize);
    if (dynamicIndex == 0 || dynamicIndex > dynamicEntryCount()) {
        return HpackDecodeError::kInvalidIndex;
    }
    const auto& entry = dynamicEntryByNewestIndex(static_cast<std::size_t>(dynamicIndex - 1));
    header = HeaderView{entry.name, entry.value};
    return std::nullopt;
}

HpackDecoder::StepResult HpackDecoder::indexedName(std::uint32_t index, std::string_view& name) const noexcept {
    HeaderView header;
    if (const auto error = indexedHeader(index, header); error.has_value()) {
        return error;
    }
    name = header.name;
    return std::nullopt;
}

void HpackDecoder::addDynamic(std::string_view name, std::string_view value) {
    const auto size = entrySize(name, value);
    if (size > maxDynamicSize_) {
        clearDynamic();
        return;
    }

    // Eviction advances dynamicOffset_ and may compact/destroy the old entries.
    // Reserve the vector slot first: if this allocation fails, the dynamic table
    // remains untouched and the caller can retry the complete field block. Without
    // this preflight, push_back() could throw after evictDynamicToFit() had already
    // removed the entries needed to decode the next indexed field.
    dynamic_.reserve(dynamic_.size() + 1);

    // Copy name and value into owned storage BEFORE evicting. For a "Literal
    // Header Field with Incremental Indexing -- Indexed Name" whose name indexes a
    // dynamic entry (RFC 7541 6.2.1), `name` aliases that entry's heap buffer --
    // and RFC 7541 4.4 explicitly allows a new entry to reference the name of an
    // entry the same insertion evicts. evictDynamicToFit() -> compactDynamic()
    // move-assigns survivors over the evicted front slots (or clears the vector on
    // full eviction), freeing the referenced buffer; copying `name` afterwards
    // would then read freed memory. Materializing first makes the insert safe.
    Entry entry{std::pmr::string(name, resource_), std::pmr::string(value, resource_)};
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
    if (decodeTransactionActive_) {
        // Keep the physical entries alive until the field-block transaction
        // commits. A later allocation failure can then restore the original
        // vector by truncating only entries appended by this decode.
        dynamicOffset_ = dynamic_.size();
        dynamicSize_ = 0;
        return;
    }
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
    if (decodeTransactionActive_) {
        return;
    }
    http2CompactMovableOffsetVector(dynamic_, dynamicOffset_, 16);
}

void HpackDecoder::beginDecodeTransaction() noexcept {
    if (decodeTransactionActive_) {
        std::terminate();
    }
    decodeTransactionActive_ = true;
    transactionDynamicSize_ = dynamicSize_;
    transactionDynamicOffset_ = dynamicOffset_;
    transactionDynamicVectorSize_ = dynamic_.size();
    transactionMaxDynamicSize_ = maxDynamicSize_;
}

void HpackDecoder::commitDecodeTransaction() noexcept {
    if (!decodeTransactionActive_) {
        std::terminate();
    }
    // Do not compact here: compaction moves owning strings and is deliberately
    // deferred until a normal non-transactional mutation. The logical offset is
    // already part of the committed table state, while leaving physical storage
    // untouched keeps this commit itself non-throwing.
    decodeTransactionActive_ = false;
}

void HpackDecoder::rollbackDecodeTransaction() noexcept {
    if (!decodeTransactionActive_) {
        return;
    }
    if (dynamic_.size() < transactionDynamicVectorSize_) {
        std::terminate();
    }
    dynamic_.resize(transactionDynamicVectorSize_);
    dynamicSize_ = transactionDynamicSize_;
    dynamicOffset_ = transactionDynamicOffset_;
    maxDynamicSize_ = transactionMaxDynamicSize_;
    decodeTransactionActive_ = false;
}

}  // namespace ruvia::detail
