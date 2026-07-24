#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpKnownMethod.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "ruvia/http/detail/util/HttpNumberFormat.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
// The two response fields Ruvia formats itself rather than taking as a value:
// Allow, built from the method mask a route matched, and Content-Range in both
// its satisfied and unsatisfied forms. Each writes straight into the response's
// own header storage, sized before it is filled.

namespace ruvia {
namespace {

inline constexpr std::size_t kAllowHeaderMethodSlots = static_cast<std::size_t>(HttpKnownMethod::kOptions) + 1;

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

void writeContentRangeHeaderValue(HttpResponseHeader& header, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
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
        size += knownHttpMethodToken(static_cast<HttpKnownMethod>(i)).size();
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
        appendHeaderValueLiteral(cursor, knownHttpMethodToken(static_cast<HttpKnownMethod>(i)));
    }
    if (cursor != end) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

}  // namespace

void HttpResponse::setAllowHeader(std::uint32_t methodMask) {
    auto& header = prepareHeaderValueStorage("Allow", allowHeaderValueSize(methodMask), detail::kResponseHeaderAllow);
    writeAllowHeaderValue(header, methodMask);
}

void HttpResponse::setContentRange(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto endOffset = offset + length - 1;
    const auto valueSize = std::string_view("bytes ").size() + detail::httpUnsignedDecimalSize(offset) + 1 + detail::httpUnsignedDecimalSize(endOffset) + 1 + detail::httpUnsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, detail::kResponseHeaderContentRange);
    writeContentRangeHeaderValue(header, offset, length, size);
}

void HttpResponse::setContentRangeUnsatisfied(std::uint64_t size) {
    const auto valueSize = std::string_view("bytes */").size() + detail::httpUnsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, detail::kResponseHeaderContentRange);
    writeContentRangeUnsatisfiedHeaderValue(header, size);
}

}  // namespace ruvia
