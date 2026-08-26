#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include <algorithm>
#include <exception>

namespace ruvia::detail {

HpackDecoder::HpackDecoder(HpackDecoderOptions options)
    : resource_(httpPmrResourceOrDefault(options.resource)),
      dynamic_(resource_),
      nameScratch_(resource_),
      valueScratch_(resource_) {}

HpackDecoder::DecodeTransaction::DecodeTransaction(HpackDecoder& decoder) noexcept
    : decoder_(&decoder) {
    decoder_->beginDecodeTransaction();
}

HpackDecoder::DecodeTransaction::~DecodeTransaction() {
    rollback();
}

void HpackDecoder::DecodeTransaction::commit() noexcept {
    if (!active_) {
        return;
    }
    decoder_->commitDecodeTransaction();
    active_ = false;
}

void HpackDecoder::DecodeTransaction::rollback() noexcept {
    if (!active_) {
        return;
    }
    decoder_->rollbackDecodeTransaction();
    active_ = false;
}

void HpackDecoder::setMaxDynamicTableSize(std::size_t bytes) {
    allowedDynamicSize_ = bytes;
    maxDynamicSize_ = std::min(maxDynamicSize_, bytes);
    evictDynamic();
}

HpackDecoder::DecodeTransaction HpackDecoder::beginTransaction() noexcept {
    return DecodeTransaction(*this);
}

HpackDecoder::StepResult HpackDecoder::decodeInteger(const unsigned char*& cursor,
    const unsigned char* end, std::uint8_t prefixBits, std::uint32_t& value) const noexcept {
    if (cursor == end || prefixBits == 0 || prefixBits > 8) {
        return HpackDecodeError::kNeedMore;
    }

    const auto prefixMask = static_cast<std::uint32_t>((1U << prefixBits) - 1U);
    value = static_cast<std::uint32_t>(*cursor++ & prefixMask);
    if (value < prefixMask) {
        return std::nullopt;
    }

    std::uint32_t shift = 0;
    for (;;) {
        if (cursor == end) {
            return HpackDecodeError::kNeedMore;
        }
        const auto byte = static_cast<std::uint32_t>(*cursor++);
        // Bound the chunk so the shift itself stays within uint32...
        if (shift >= 28 && (byte & 0x7fU) > 0x0fU) {
            return HpackDecodeError::kIntegerOverflow;
        }
        // ...then reject the case where the accumulated sum would still overflow
        // uint32 (e.g. FF FF FF FF FF 0F): the chunk guard alone permits 0x0f<<28,
        // which added to a maxed lower value wraps past UINT32_MAX (RFC 7541 5.1).
        const auto addend = (byte & 0x7fU) << shift;
        if (value > 0xFFFFFFFFU - addend) {
            return HpackDecodeError::kIntegerOverflow;
        }
        value += addend;
        if ((byte & 0x80U) == 0) {
            return std::nullopt;
        }
        shift += 7;
        if (shift > 28) {
            return HpackDecodeError::kIntegerOverflow;
        }
    }
}

HpackDecoder::StepResult HpackDecoder::decodeString(const unsigned char*& cursor,
    const unsigned char* end, std::pmr::string& scratch, std::string_view& value) {
    if (cursor == end) {
        return HpackDecodeError::kNeedMore;
    }
    const bool huffman = (*cursor & 0x80U) != 0;
    std::uint32_t size = 0;
    if (const auto error = decodeInteger(cursor, end, 7, size); error.has_value()) {
        return error;
    }
    if (static_cast<std::size_t>(end - cursor) < size) {
        return HpackDecodeError::kNeedMore;
    }

    const std::string_view encoded(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    if (!huffman) {
        value = encoded;
        return std::nullopt;
    }

    if (const auto error = decodeHuffman(encoded, scratch); error.has_value()) {
        return error;
    }
    value = scratch;
    return std::nullopt;
}

HpackDecoder::StepResult HpackDecoder::decodeLiteralHeader(const unsigned char*& cursor,
    const unsigned char* end, std::uint8_t nameIndexPrefixBits, bool indexIntoDynamic, void* target,
    HeaderCallback callback, bool& rejected) {
    std::uint32_t nameIndex = 0;
    if (const auto error = decodeInteger(cursor, end, nameIndexPrefixBits, nameIndex);
        error.has_value()) {
        return error;
    }

    std::string_view name;
    if (nameIndex == 0) {
        if (const auto error = decodeString(cursor, end, nameScratch_, name); error.has_value()) {
            return error;
        }
    } else if (const auto error = indexedName(nameIndex, name); error.has_value()) {
        return error;
    }

    std::string_view value;
    if (const auto error = decodeString(cursor, end, valueScratch_, value); error.has_value()) {
        return error;
    }
    // Suppress the callback once rejected, but ALWAYS apply the dynamic-table insertion
    // below -- skipping it would desync the connection-global table for every later
    // block (RFC 7541 §4.1). The rejection is reported after the whole block decodes.
    if (!rejected && callback != nullptr && !callback(target, name, value)) {
        rejected = true;
    }
    if (indexIntoDynamic) {
        addDynamic(name, value);
    }
    return std::nullopt;
}

void HpackDecoder::releaseScratch() {
    clearPmrStringRetainingSmall(nameScratch_);
    clearPmrStringRetainingSmall(valueScratch_);
}

HpackDecodeResult HpackDecoder::decode(
    std::string_view block, void* target, HeaderCallback callback) {
    auto transaction = beginTransaction();
    auto result = decode(block, target, callback, transaction);
    if (transaction.active()) {
        transaction.commit();
    }
    return result;
}

HpackDecodeResult HpackDecoder::decode(
    std::string_view block, void* target, HeaderCallback callback, DecodeTransaction& transaction) {
    if (transaction.decoder_ != this || !transaction.active_) {
        std::terminate();
    }
    try {
        auto result = decodeBlock(block, target, callback);
        if (const auto* failure = result.failure();
            failure != nullptr && failure->error() != HpackDecodeError::kCallbackRejected) {
            transaction.rollback();
        }
        return result;
    } catch (...) {
        // A field block is the HPACK transaction boundary. In particular, a
        // later allocation failure must not leave earlier incremental-indexing
        // entries visible when the caller retries the same block.
        transaction.rollback();
        throw;
    }
}

HpackDecodeResult HpackDecoder::decodeBlock(
    std::string_view block, void* target, HeaderCallback callback) {
    struct ScratchReleaseGuard final {
        HpackDecoder& decoder;

        ~ScratchReleaseGuard() {
            decoder.releaseScratch();
        }
    };

    ScratchReleaseGuard scratchGuard{*this};
    const auto* cursor = reinterpret_cast<const unsigned char*>(block.data());
    const auto* const end = cursor + block.size();
    bool sawHeader = false;
    std::uint8_t sizeUpdateCount = 0;
    std::uint32_t firstSizeUpdate = 0;
    // Callback rejection does NOT abort the block: the whole field block must be
    // decoded so the connection-global dynamic table stays consistent (RFC 7541 §4.1 /
    // RFC 9113 §4.3). We keep going with the callback suppressed and report the
    // rejection at the end; the caller then RST_STREAMs the (validated-as-bad) stream
    // while the connection -- and every later block on it -- decodes correctly.
    bool rejected = false;

    while (cursor < end) {
        const auto first = *cursor;
        if ((first & 0x80U) != 0) {
            std::uint32_t index = 0;
            if (const auto error = decodeInteger(cursor, end, 7, index); error.has_value()) {
                return HpackDecodeResult(*error);
            }
            HeaderView header;
            if (const auto error = indexedHeader(index, header); error.has_value()) {
                return HpackDecodeResult(*error);
            }
            if (!rejected && callback != nullptr && !callback(target, header.name, header.value)) {
                rejected = true;
            }
            sawHeader = true;
            continue;
        }

        if ((first & 0x40U) != 0) {
            if (const auto error =
                    decodeLiteralHeader(cursor, end, 6, true, target, callback, rejected);
                error.has_value()) {
                return HpackDecodeResult(*error);
            }
            sawHeader = true;
            continue;
        }

        if ((first & 0xe0U) == 0x20U) {
            if (sawHeader || sizeUpdateCount == 2) {
                return HpackDecodeResult(HpackDecodeError::kDynamicTableSize);
            }
            std::uint32_t size = 0;
            if (const auto error = decodeInteger(cursor, end, 5, size); error.has_value()) {
                return HpackDecodeResult(*error);
            }
            // RFC 7541 section 4.2 permits at most two updates at the start
            // of a field block. When two are present, the first is the
            // smallest intervening maximum and the second is the final
            // maximum, so the second value cannot be lower than the first.
            if (size > allowedDynamicSize_ || (sizeUpdateCount == 1 && size < firstSizeUpdate)) {
                return HpackDecodeResult(HpackDecodeError::kDynamicTableSize);
            }
            if (sizeUpdateCount == 0) {
                firstSizeUpdate = size;
            }
            ++sizeUpdateCount;
            maxDynamicSize_ = size;
            evictDynamic();
            continue;
        }

        if ((first & 0xf0U) == 0x00U || (first & 0xf0U) == 0x10U) {
            if (const auto error =
                    decodeLiteralHeader(cursor, end, 4, false, target, callback, rejected);
                error.has_value()) {
                return HpackDecodeResult(*error);
            }
            sawHeader = true;
            continue;
        }

        return HpackDecodeResult(HpackDecodeError::kInvalidIndex);
    }

    // The whole block decoded and the dynamic table is consistent; surface a late
    // callback rejection now so the owner RST_STREAMs without desyncing the connection.
    return rejected ? HpackDecodeResult(HpackDecodeError::kCallbackRejected) : HpackDecodeResult();
}

}  // namespace ruvia::detail
