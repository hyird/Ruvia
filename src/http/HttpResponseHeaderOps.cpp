#include "ruvia/http/HttpResponse.h"

#include "HttpResponseHeaderAccess.h"
#include "HttpResponseHeaderBits.h"
#include "HttpResponseKnownHeaders.h"
#include "ruvia/detail/NumberFormat.h"
#include "ResponseHeaderIndexCache.h"

#include <charconv>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ruvia {
namespace {

inline constexpr std::size_t kAllowHeaderMethodSlots = static_cast<std::size_t>(HttpMethod::kOptions) + 1;

void writeUnsignedHeaderValue(HttpResponseHeader& header, std::uint64_t value) {
    auto* const begin = detail::responseHeaderValueBegin(header);
    auto* const end = detail::responseHeaderValueEnd(header);
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

    auto* cursor = detail::responseHeaderValueBegin(header);
    auto* const end = detail::responseHeaderValueEnd(header);
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
    auto* cursor = detail::responseHeaderValueBegin(header);
    auto* const end = detail::responseHeaderValueEnd(header);
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
    auto* cursor = detail::responseHeaderValueBegin(header);
    auto* const end = detail::responseHeaderValueEnd(header);
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

void HttpResponse::recordKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept {
    knownHeaderBits_ |= knownBit;
    detail::recordResponseHeaderIndex(knownHeaderIndexes_, detail::responseKnownHeaderSlot(knownBit), index);
}

HttpResponseHeader* HttpResponse::findHeaderForUpdate(std::string_view key, std::uint32_t knownBit) noexcept {
    return const_cast<HttpResponseHeader*>(
        static_cast<const HttpResponse&>(*this).findHeaderForRead(key, knownBit));
}

const HttpResponseHeader* HttpResponse::findHeaderForRead(
    std::string_view key,
    std::uint32_t knownBit) const noexcept {
    const auto* const begin = headers_.begin();
    const auto* const end = headers_.end();
    const auto* const header = detail::findResponseHeaderIndexed(
        begin,
        end,
        knownHeaderIndexes_,
        detail::responseKnownHeaderSlot(knownBit),
        key,
        knownBit);
    return header == end ? nullptr : header;
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

std::string_view HttpResponse::knownHeaderValue(std::uint32_t bit) const noexcept {
    const auto* const begin = headers_.begin();
    const auto* const end = headers_.end();
    const auto* const header = detail::findResponseHeaderIndexed(
        begin,
        end,
        knownHeaderIndexes_,
        detail::responseKnownHeaderSlot(bit),
        {},
        bit);
    return header == end ? std::string_view{} : header->value();
}

std::string_view HttpResponse::header(std::string_view name) const noexcept {
    if (const auto* const found = findHeaderForRead(name, detail::classifyResponseHeaderName(name))) {
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
    setHeaderValidated(key, value, detail::classifyResponseHeaderName(key));
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

void HttpResponse::appendHeaderValidated(
    std::string_view key,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto index = headers_.size();
    headers_.add(key, value, knownBit);
    recordKnownHeaderIndex(knownBit, index);
}

void HttpResponse::setHeaderStableView(std::string_view key, std::string_view value) {
    const auto knownBit = detail::classifyResponseHeaderName(key);
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        headers_.assignStableView(*header, key, value, knownBit);
        return;
    }

    const auto index = headers_.size();
    headers_.addStableView(key, value, knownBit);
    recordKnownHeaderIndex(knownBit, index);
}

void HttpResponse::setHeaderUnsigned(std::string_view key, std::uint64_t value, std::uint32_t knownBit) {
    auto& header = prepareHeaderValueStorage(key, detail::unsignedDecimalSize(value), knownBit);
    writeUnsignedHeaderValue(header, value);
}

void HttpResponse::setAllowHeader(std::uint32_t methodMask) {
    auto& header = prepareHeaderValueStorage("Allow", allowHeaderValueSize(methodMask), detail::kResponseHeaderAllow);
    writeAllowHeaderValue(header, methodMask);
}

void HttpResponse::setContentRange(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    const auto endOffset = offset + length - 1;
    const auto valueSize = std::string_view("bytes ").size() +
        detail::unsignedDecimalSize(offset) +
        1 +
        detail::unsignedDecimalSize(endOffset) +
        1 +
        detail::unsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, detail::kResponseHeaderContentRange);
    writeContentRangeHeaderValue(header, offset, length, size);
}

void HttpResponse::setContentRangeUnsatisfied(std::uint64_t size) {
    const auto valueSize = std::string_view("bytes */").size() + detail::unsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, detail::kResponseHeaderContentRange);
    writeContentRangeUnsatisfiedHeaderValue(header, size);
}

void HttpResponse::reserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

}  // namespace ruvia
