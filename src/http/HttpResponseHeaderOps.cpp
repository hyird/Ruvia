#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/HeaderUtils.h"

#include <bit>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ruvia {
namespace {

inline constexpr std::size_t kAllowHeaderMethodSlots = static_cast<std::size_t>(HttpMethod::kOptions) + 1;

[[nodiscard]] std::size_t unsignedDecimalSize(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

void writeUnsignedHeaderValue(HttpResponseHeader& header, std::uint64_t value) {
    auto* const begin = const_cast<char*>(header.bytes) + header.nameSize;
    auto* const end = begin + header.valueSize;
    const auto [ptr, ec] = std::to_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::logic_error("failed to format HTTP response header value");
    }
}

void appendHeaderValueLiteral(char*& cursor, std::string_view value) noexcept {
    std::memcpy(cursor, value.data(), value.size());
    cursor += value.size();
}

void appendHeaderValueUnsigned(char*& cursor, char* end, std::uint64_t value) {
    const auto [ptr, ec] = std::to_chars(cursor, end, value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format HTTP response header value");
    }
    cursor = ptr;
}

void writeContentRangeHeaderValue(
    HttpResponseHeader& header,
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }

    auto* cursor = const_cast<char*>(header.bytes) + header.nameSize;
    auto* const end = cursor + header.valueSize;
    appendHeaderValueLiteral(cursor, "bytes ");
    appendHeaderValueUnsigned(cursor, end, offset);
    *cursor++ = '-';
    appendHeaderValueUnsigned(cursor, end, offset + length - 1);
    *cursor++ = '/';
    appendHeaderValueUnsigned(cursor, end, size);
    if (cursor != end) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

void writeContentRangeUnsatisfiedHeaderValue(HttpResponseHeader& header, std::uint64_t size) {
    auto* cursor = const_cast<char*>(header.bytes) + header.nameSize;
    auto* const end = cursor + header.valueSize;
    appendHeaderValueLiteral(cursor, "bytes */");
    appendHeaderValueUnsigned(cursor, end, size);
    if (cursor != end) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

[[nodiscard]] std::size_t allowHeaderValueSize(std::uint32_t methodMask) noexcept {
    std::size_t size = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < kAllowHeaderMethodSlots; ++i) {
        if ((methodMask & (1U << i)) == 0) {
            continue;
        }
        size += methodName(static_cast<HttpMethod>(i)).size();
        ++count;
    }
    return count > 1 ? size + (count - 1) * 2 : size;
}

void writeAllowHeaderValue(HttpResponseHeader& header, std::uint32_t methodMask) {
    auto* cursor = const_cast<char*>(header.bytes) + header.nameSize;
    auto* const end = cursor + header.valueSize;
    bool first = true;
    for (std::size_t i = 0; i < kAllowHeaderMethodSlots; ++i) {
        if ((methodMask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            appendHeaderValueLiteral(cursor, ", ");
        }
        first = false;
        appendHeaderValueLiteral(cursor, methodName(static_cast<HttpMethod>(i)));
    }
    if (cursor != end) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

}  // namespace

std::size_t HttpResponse::knownHeaderSlot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t knownMask = (1U << kKnownHeaderCount) - 1U;
    if (bit == 0 || (bit & ~knownMask) != 0 || (bit & (bit - 1U)) != 0) {
        return kKnownHeaderCount;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

void HttpResponse::recordKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept {
    const auto slot = knownHeaderSlot(knownBit);
    if (slot >= knownHeaderIndexes_.size()) {
        return;
    }
    knownHeaderBits_ |= knownBit;
    if (knownHeaderIndexes_[slot] < 0) {
        knownHeaderIndexes_[slot] = static_cast<std::int32_t>(index);
    }
}

HttpResponseHeader* HttpResponse::findHeaderForUpdate(std::string_view key, std::uint32_t knownBit) noexcept {
    return const_cast<HttpResponseHeader*>(
        static_cast<const HttpResponse&>(*this).findHeaderForRead(key, knownBit));
}

const HttpResponseHeader* HttpResponse::findHeaderForRead(
    std::string_view key,
    std::uint32_t knownBit) const noexcept {
    const auto knownSlot = knownHeaderSlot(knownBit);
    if (knownSlot < knownHeaderIndexes_.size()) {
        const auto index = knownHeaderIndexes_[knownSlot];
        if (index >= 0) {
            return headers_.begin() + index;
        }
        return nullptr;
    }

    for (const auto& header : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), key)) {
            return &header;
        }
    }
    return nullptr;
}

HttpResponseHeader& HttpResponse::prepareHeaderValueStorage(
    std::string_view key,
    std::size_t valueSize,
    std::uint32_t knownBit) {
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        return headers_.assignUninitializedValue(*header, key, valueSize, knownBit);
    }

    const auto index = headers_.size();
    auto& header = headers_.addUninitializedValue(key, valueSize, knownBit);
    recordKnownHeaderIndex(knownBit, index);
    return header;
}

std::string_view HttpResponse::header(KnownHeaderBit bit) const noexcept {
    const auto slot = knownHeaderSlot(bit);
    if (slot < knownHeaderIndexes_.size()) {
        const auto index = knownHeaderIndexes_[slot];
        if (index >= 0) {
            return headers_.begin()[index].value();
        }
    }
    return {};
}

std::string_view HttpResponse::header(std::string_view name) const noexcept {
    if (const auto* const found = findHeaderForRead(name, classifyKnownHeader(name))) {
        return found->value();
    }
    return {};
}

void HttpResponse::setHeader(std::string_view key, std::string_view value) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    setHeaderValidated(key, value, classifyKnownHeader(key));
}

void HttpResponse::setHeaderValidated(
    std::string_view key,
    std::string_view value,
    std::uint32_t knownBit) {
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        headers_.assign(*header, key, value, knownBit);
        return;
    }

    const auto index = headers_.size();
    headers_.add(key, value, knownBit);
    recordKnownHeaderIndex(knownBit, index);
}

void HttpResponse::appendHeader(std::string_view key, std::string_view value) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    appendHeaderValidated(key, value, classifyKnownHeader(key));
}

void HttpResponse::appendHeaderValidated(
    std::string_view key,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto index = headers_.size();
    headers_.add(key, value, knownBit);
    recordKnownHeaderIndex(knownBit, index);
}

void HttpResponse::setHeaderStableView(std::string_view key, std::string_view value) {
    const auto knownBit = classifyKnownHeader(key);
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        headers_.assignStableView(*header, key, value, knownBit);
        return;
    }

    const auto index = headers_.size();
    headers_.addStableView(key, value, knownBit);
    recordKnownHeaderIndex(knownBit, index);
}

void HttpResponse::setHeaderUnsigned(std::string_view key, std::uint64_t value, std::uint32_t knownBit) {
    auto& header = prepareHeaderValueStorage(key, unsignedDecimalSize(value), knownBit);
    writeUnsignedHeaderValue(header, value);
}

void HttpResponse::setAllowHeader(std::uint32_t methodMask) {
    auto& header = prepareHeaderValueStorage("Allow", allowHeaderValueSize(methodMask), kKnownHeaderAllow);
    writeAllowHeaderValue(header, methodMask);
}

void HttpResponse::setContentRange(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    const auto endOffset = offset + length - 1;
    const auto valueSize = std::string_view("bytes ").size() +
        unsignedDecimalSize(offset) +
        1 +
        unsignedDecimalSize(endOffset) +
        1 +
        unsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, kKnownHeaderContentRange);
    writeContentRangeHeaderValue(header, offset, length, size);
}

void HttpResponse::setContentRangeUnsatisfied(std::uint64_t size) {
    const auto valueSize = std::string_view("bytes */").size() + unsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, kKnownHeaderContentRange);
    writeContentRangeUnsatisfiedHeaderValue(header, size);
}

void HttpResponse::reserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

namespace detail {

void setResponseHeaderStableView(HttpResponse& response, std::string_view key, std::string_view value) {
    response.setHeaderStableView(key, value);
}

void setResponseHeaderUnsigned(HttpResponse& response, std::string_view key, std::uint64_t value, std::uint32_t knownBit) {
    response.setHeaderUnsigned(key, value, knownBit);
}

void setResponseAllowHeader(HttpResponse& response, std::uint32_t methodMask) {
    response.setAllowHeader(methodMask);
}

}  // namespace detail

}  // namespace ruvia
