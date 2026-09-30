#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/hpack/Http2HpackStaticTable.h"

namespace ruvia::detail {

HpackDecoder::Entry::Entry(
    std::pmr::memory_resource* owner, std::string_view entryName, std::string_view entryValue)
    : resource_(owner),
      nameLength_(entryName.size()),
      valueLength_(entryValue.size()) {
    const auto length = nameLength_ + valueLength_;
    if (isInline()) {
        if (nameLength_ != 0) {
            std::memcpy(storage_.inlineBytes_, entryName.data(), nameLength_);
        }
        if (valueLength_ != 0) {
            std::memcpy(storage_.inlineBytes_ + nameLength_, entryValue.data(), valueLength_);
        }
        return;
    }

    auto allocator = std::pmr::polymorphic_allocator<char>(resource_);
    char* bytes = allocator.allocate(length);
    storage_.heap_ = bytes;
    if (nameLength_ != 0) {
        std::memcpy(bytes, entryName.data(), nameLength_);
    }
    if (valueLength_ != 0) {
        std::memcpy(bytes + nameLength_, entryValue.data(), valueLength_);
    }
}

HpackDecoder::Entry::Entry(Entry&& other) noexcept
    : resource_(other.resource_),
      nameLength_(other.nameLength_),
      valueLength_(other.valueLength_) {
    if (other.isInline()) {
        const auto length = nameLength_ + valueLength_;
        if (length != 0) {
            std::memcpy(storage_.inlineBytes_, other.storage_.inlineBytes_, length);
        }
    } else {
        storage_.heap_ = other.storage_.heap_;
    }
    other.restoreInlineEmpty();
}

HpackDecoder::Entry::~Entry() {
    if (!isInline()) {
        auto allocator = std::pmr::polymorphic_allocator<char>(resource_);
        allocator.deallocate(storage_.heap_, nameLength_ + valueLength_);
    }
}

std::string_view HpackDecoder::Entry::name() const noexcept {
    const char* bytes = isInline() ? storage_.inlineBytes_ : storage_.heap_;
    return {bytes, nameLength_};
}

std::string_view HpackDecoder::Entry::value() const noexcept {
    const char* bytes = isInline() ? storage_.inlineBytes_ : storage_.heap_;
    return {bytes + nameLength_, valueLength_};
}

void HpackDecoder::Entry::restoreInlineEmpty() noexcept {
    const bool wasInline = isInline();
    nameLength_ = 0;
    valueLength_ = 0;
    if (!wasInline) {
        // Built-in assignment starts the inline array's lifetime after a heap move.
        storage_.inlineBytes_[0] = '\0';
    }
}

HpackDecoder::StepResult HpackDecoder::indexedHeader(
    std::uint32_t index, HeaderView& header) const noexcept {
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
    header = HeaderView{entry.name(), entry.value()};
    return std::nullopt;
}

HpackDecoder::StepResult HpackDecoder::indexedName(
    std::uint32_t index, std::string_view& name) const noexcept {
    HeaderView header;
    if (const auto error = indexedHeader(index, header); error.has_value()) {
        return error;
    }
    name = header.name;
    return std::nullopt;
}

void HpackDecoder::addDynamic(std::string_view name, std::string_view value) {
    // Check against the table budget using subtraction before adding untrusted
    // lengths, avoiding overflow while preserving the oversized-entry clear rule.
    if (maxDynamicSize_ < 32) {
        clearDynamic();
        return;
    }
    const auto fieldBudget = maxDynamicSize_ - 32;
    if (name.size() > fieldBudget || value.size() > fieldBudget - name.size()) {
        clearDynamic();
        return;
    }
    const auto size = name.size() + value.size() + 32;

    // Copy name and value into owned storage before ANY operation that can move or
    // destroy a dynamic entry. For a "Literal
    // Header Field with Incremental Indexing -- Indexed Name" whose name indexes a
    // dynamic entry (RFC 7541 6.2.1), `name` aliases storage inside that entry.
    // RFC 7541 4.4 allows the insertion to evict the referenced entry, while a
    // vector reserve can move the entry's inline bytes even before eviction. Either
    // operation would invalidate the view. Materializing into packed owned storage
    // first makes both safe; if either allocation throws, the live table remains
    // untouched and the complete field block is retryable.
    Entry entry(resource_, name, value);

    // Reserve before eviction so push_back cannot throw after logical entries have
    // been removed from the table. Grow geometrically rather than forcing an exact
    // reallocation for every insertion.
    if (dynamic_.size() == dynamic_.capacity()) {
        const auto maxSize = dynamic_.max_size();
        if (dynamic_.size() == maxSize) {
            throw std::length_error("HPACK dynamic table is too large");
        }
        const auto required = dynamic_.size() + 1;
        const auto doubled = dynamic_.capacity() > maxSize / 2
                                 ? maxSize
                                 : dynamic_.capacity() * 2;
        dynamic_.reserve(std::max(required, doubled));
    }
    evictDynamicToFit(size);
    dynamic_.push_back(std::move(entry));
    dynamicSize_ += size;
}

std::size_t HpackDecoder::dynamicEntryCount() const noexcept {
    return dynamic_.size() - dynamicOffset_;
}

const HpackDecoder::Entry& HpackDecoder::dynamicEntryByNewestIndex(
    std::size_t newestIndex) const noexcept {
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
        dynamicSize_ -= entry.tableSize();
    }
    compactDynamic();
}

void HpackDecoder::evictDynamic() {
    while (dynamicSize_ > maxDynamicSize_ && dynamicOffset_ < dynamic_.size()) {
        const auto& entry = dynamic_[dynamicOffset_++];
        dynamicSize_ -= entry.tableSize();
    }
    compactDynamic();
}

void HpackDecoder::compactDynamic() noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Entry>);
    if (decodeTransactionActive_ || dynamicOffset_ == 0) {
        return;
    }
    const auto remaining = dynamicEntryCount();
    for (std::size_t i = 0; i < remaining; ++i) {
        // The source is strictly ahead of the destination. A destination may
        // itself be an earlier moved-from source, but no unread source is
        // destroyed. Reconstruct every slot immediately; the vector retains
        // live elements throughout and resize() destroys the trailing sources.
        // Move construction transfers packed heap blocks without allocator-aware
        // assignment or allocation.
        auto* destination = std::addressof(dynamic_[i]);
        std::destroy_at(destination);
        std::construct_at(destination, std::move(dynamic_[dynamicOffset_ + i]));
    }
    dynamic_.resize(remaining);
    dynamicOffset_ = 0;
}

void HpackDecoder::beginDecodeTransaction() noexcept {
    if (decodeTransactionActive_) {
        std::terminate();
    }
    // Defensive compaction before the snapshot keeps rollback bounded even if a
    // prior mutation left tombstones. No state from this transaction exists yet.
    compactDynamic();
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
    // No rollback remains after commit. Release evicted field storage before
    // returning to the caller; live entries move without allocation or exceptions.
    decodeTransactionActive_ = false;
    compactDynamic();
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
