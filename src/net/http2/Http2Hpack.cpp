#include "Http2Hpack.h"

#include <algorithm>

namespace ruvia::detail {

HpackDecoder::HpackDecoder(std::pmr::memory_resource* resource)
    : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      dynamic_(resource_),
      nameScratch_(resource_),
      valueScratch_(resource_) {
    dynamic_.reserve(maxDynamicSize_ / entrySize({}, {}));
    buildHuffmanTree();
}

void HpackDecoder::setMaxDynamicTableSize(std::size_t bytes) {
    allowedDynamicSize_ = bytes;
    maxDynamicSize_ = std::min(maxDynamicSize_, bytes);
    evictDynamic();
}

HpackError HpackDecoder::decodeInteger(
    const unsigned char*& cursor,
    const unsigned char* end,
    std::uint8_t prefixBits,
    std::uint32_t& value) const noexcept {
    if (cursor == end || prefixBits == 0 || prefixBits > 8) {
        return HpackError::kNeedMore;
    }

    const auto prefixMask = static_cast<std::uint32_t>((1U << prefixBits) - 1U);
    value = static_cast<std::uint32_t>(*cursor++ & prefixMask);
    if (value < prefixMask) {
        return HpackError::kNone;
    }

    std::uint32_t shift = 0;
    for (;;) {
        if (cursor == end) {
            return HpackError::kNeedMore;
        }
        const auto byte = static_cast<std::uint32_t>(*cursor++);
        if (shift >= 28 && (byte & 0x7fU) > 0x0fU) {
            return HpackError::kIntegerOverflow;
        }
        value += (byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            return HpackError::kNone;
        }
        shift += 7;
        if (shift > 28) {
            return HpackError::kIntegerOverflow;
        }
    }
}

HpackError HpackDecoder::decodeString(
    const unsigned char*& cursor,
    const unsigned char* end,
    std::pmr::string& scratch,
    std::string_view& value) {
    if (cursor == end) {
        return HpackError::kNeedMore;
    }
    const bool huffman = (*cursor & 0x80U) != 0;
    std::uint32_t size = 0;
    if (const auto error = decodeInteger(cursor, end, 7, size); error != HpackError::kNone) {
        return error;
    }
    if (static_cast<std::size_t>(end - cursor) < size) {
        return HpackError::kNeedMore;
    }

    const std::string_view encoded(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    if (!huffman) {
        value = encoded;
        return HpackError::kNone;
    }

    if (const auto error = decodeHuffman(encoded, scratch); error != HpackError::kNone) {
        return error;
    }
    value = scratch;
    return HpackError::kNone;
}

HpackDecodeResult HpackDecoder::decode(std::string_view block, void* target, HeaderCallback callback) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(block.data());
    const auto* const end = cursor + block.size();
    bool sawHeader = false;

    while (cursor < end) {
        const auto first = *cursor;
        if ((first & 0x80U) != 0) {
            std::uint32_t index = 0;
            if (const auto error = decodeInteger(cursor, end, 7, index); error != HpackError::kNone) {
                return {error};
            }
            HeaderView header;
            if (const auto error = indexedHeader(index, header); error != HpackError::kNone) {
                return {error};
            }
            if (callback != nullptr && !callback(target, header.name, header.value)) {
                return {HpackError::kCallbackRejected};
            }
            sawHeader = true;
            continue;
        }

        if ((first & 0x40U) != 0) {
            std::uint32_t nameIndex = 0;
            if (const auto error = decodeInteger(cursor, end, 6, nameIndex); error != HpackError::kNone) {
                return {error};
            }
            std::string_view name;
            if (nameIndex == 0) {
                if (const auto error = decodeString(cursor, end, nameScratch_, name); error != HpackError::kNone) {
                    return {error};
                }
            } else if (const auto error = indexedName(nameIndex, name); error != HpackError::kNone) {
                return {error};
            }

            std::string_view value;
            if (const auto error = decodeString(cursor, end, valueScratch_, value); error != HpackError::kNone) {
                return {error};
            }
            if (callback != nullptr && !callback(target, name, value)) {
                return {HpackError::kCallbackRejected};
            }
            addDynamic(name, value);
            sawHeader = true;
            continue;
        }

        if ((first & 0xe0U) == 0x20U) {
            if (sawHeader) {
                return {HpackError::kDynamicTableSize};
            }
            std::uint32_t size = 0;
            if (const auto error = decodeInteger(cursor, end, 5, size); error != HpackError::kNone) {
                return {error};
            }
            if (size > allowedDynamicSize_) {
                return {HpackError::kDynamicTableSize};
            }
            maxDynamicSize_ = size;
            evictDynamic();
            continue;
        }

        if ((first & 0xf0U) == 0x00U || (first & 0xf0U) == 0x10U) {
            std::uint32_t nameIndex = 0;
            if (const auto error = decodeInteger(cursor, end, 4, nameIndex); error != HpackError::kNone) {
                return {error};
            }
            std::string_view name;
            if (nameIndex == 0) {
                if (const auto error = decodeString(cursor, end, nameScratch_, name); error != HpackError::kNone) {
                    return {error};
                }
            } else if (const auto error = indexedName(nameIndex, name); error != HpackError::kNone) {
                return {error};
            }

            std::string_view value;
            if (const auto error = decodeString(cursor, end, valueScratch_, value); error != HpackError::kNone) {
                return {error};
            }
            if (callback != nullptr && !callback(target, name, value)) {
                return {HpackError::kCallbackRejected};
            }
            sawHeader = true;
            continue;
        }

        return {HpackError::kInvalidIndex};
    }

    return {};
}

}  // namespace ruvia::detail
