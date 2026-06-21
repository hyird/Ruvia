#include "Http2Hpack.h"
#include "Http2HpackStaticTable.h"

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
    if (dynamicIndex == 0 || dynamicIndex > dynamic_.size()) {
        return HpackError::kInvalidIndex;
    }
    const auto& entry = dynamic_[dynamicIndex - 1];
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
        dynamic_.clear();
        dynamicSize_ = 0;
        return;
    }

    evictDynamicToFit(size);
    Entry entry{
        std::pmr::string(name, resource_),
        std::pmr::string(value, resource_)};
    dynamic_.insert(dynamic_.begin(), std::move(entry));
    dynamicSize_ += size;
}

void HpackDecoder::evictDynamicToFit(std::size_t entrySize) noexcept {
    const auto targetSize = maxDynamicSize_ - entrySize;
    while (dynamicSize_ > targetSize && !dynamic_.empty()) {
        const auto& entry = dynamic_.back();
        dynamicSize_ -= HpackDecoder::entrySize(entry.name, entry.value);
        dynamic_.pop_back();
    }
}

void HpackDecoder::evictDynamic() noexcept {
    while (dynamicSize_ > maxDynamicSize_ && !dynamic_.empty()) {
        const auto& entry = dynamic_.back();
        dynamicSize_ -= entrySize(entry.name, entry.value);
        dynamic_.pop_back();
    }
}

}  // namespace ruvia::detail
