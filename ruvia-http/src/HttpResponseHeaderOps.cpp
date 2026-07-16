#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpKnownMethod.h"

#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/HttpResponseKnownHeaders.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/HttpNumberFormat.h"
#include "ruvia/http/detail/ResponseHeaderIndexCache.h"

#include <charconv>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ruvia {
namespace {

inline constexpr std::size_t kAllowHeaderMethodSlots = static_cast<std::size_t>(HttpKnownMethod::kOptions) + 1;

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

void validateConnectionControlField(
    std::string_view name,
    std::string_view value) {
    if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
        detail::HttpConnectionOptions options;
        if (options.parseField(
                value,
                detail::HttpFieldListRole::kSender) !=
            detail::HttpFieldListParseStatus::kOk) {
            throw std::invalid_argument("invalid HTTP Connection header");
        }
    } else if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
        detail::HttpUpgradeProtocols protocols;
        if (protocols.parseField(
                value,
                detail::HttpFieldListRole::kSender,
                [](const detail::HttpUpgradeProtocol&) noexcept {
                    return true;
                }) != detail::HttpFieldListParseStatus::kOk) {
            throw std::invalid_argument("invalid HTTP Upgrade header");
        }
    }
}

[[nodiscard]] bool setCookieValueHasWireName(
    std::string_view value,
    std::string_view wirePrefix,
    std::string_view cookieName) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    if (!value.starts_with(wirePrefix)) {
        return false;
    }
    value.remove_prefix(wirePrefix.size());
    if (!value.starts_with(cookieName)) {
        return false;
    }
    value.remove_prefix(cookieName.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    return !value.empty() && value.front() == '=';
}

[[nodiscard]] std::string_view setCookieWireName(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {};
    }
    auto name = value.substr(0, equals);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
    }
    return isValidHttpHeaderName(name) ? name : std::string_view{};
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
        const bool wasAppended = detail::responseHeaderAppend(*header);
        headers_.assignUninitializedValue(*header, key, valueSize, knownBit);
        return wasAppended
            ? collapseResponseHeaders(*header, key, knownBit)
            : *header;
    }

    const auto index = headers_.size();
    auto& header = headers_.addUninitializedValue(key, valueSize, knownBit);
    recordKnownHeaderIndex(knownBit, index);
    return header;
}

std::string_view HttpResponse::knownHeaderValue(std::uint32_t bit) const noexcept {
    const auto* const header = findHeaderForRead({}, bit);
    return header == nullptr ? std::string_view{} : header->value();
}

std::optional<std::string_view>
HttpResponse::header(std::string_view name) const & noexcept {
    if (const auto* const found = findHeaderForRead(name, detail::classifyResponseHeaderName(name))) {
        return found->value();
    }
    return std::nullopt;
}

void HttpResponse::rebuildKnownHeaderIndex() noexcept {
    knownHeaderBits_ = 0;
    knownHeaderIndexes_.fill(detail::kMissingResponseHeaderIndexSlot);
    const auto* const begin = headers_.begin();
    const auto* const end = headers_.end();
    for (auto* cursor = begin; cursor != end; ++cursor) {
        const auto knownBit = detail::responseHeaderKnownBit(*cursor);
        if (knownBit == 0) {
            continue;
        }
        knownHeaderBits_ |= knownBit;
        detail::recordResponseHeaderIndex(
            knownHeaderIndexes_,
            detail::responseKnownHeaderSlot(knownBit),
            static_cast<std::size_t>(cursor - begin));
    }
}

void HttpResponse::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void HttpResponse::header(std::string_view key, std::string_view value, HeaderOptions options) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    validateConnectionControlField(key, value);
    const auto knownBit = detail::classifyResponseHeaderName(key);
    if (options.append) {
        if (detail::responseHeaderAppendForbidden(knownBit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        if (knownBit == detail::kResponseHeaderSetCookie) {
            upsertSetCookieHeaderValidated(value);
        } else {
            appendHeaderValidated(key, value, knownBit);
        }
    } else {
        setHeaderValidated(key, value, knownBit);
    }
}

void HttpResponse::header(std::string_view key, std::nullopt_t) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)removeHeaderValidated(key, detail::classifyResponseHeaderName(key));
}

void HttpResponse::setHeaderValidated(
    std::string_view key,
    std::string_view value,
    std::uint32_t knownBit) {
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        const bool wasAppended = detail::responseHeaderAppend(*header);
        headers_.assign(*header, key, value, knownBit);
        if (wasAppended) {
            (void)collapseResponseHeaders(*header, key, knownBit);
        }
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
    if (detail::responseHeaderAppendForbidden(knownBit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update.
    if (auto* const existing = findHeaderForUpdate(key, knownBit)) {
        detail::setResponseHeaderAppend(*existing, true);
    }
    const auto index = headers_.size();
    auto& header = headers_.add(key, value, knownBit);
    // Mark the append flag so a later merge of this response keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::setResponseHeaderAppend(header, true);
    recordKnownHeaderIndex(knownBit, index);
}

HttpResponseHeader& HttpResponse::appendHeaderUninitializedValue(
    std::string_view key,
    std::size_t valueSize,
    std::uint32_t knownBit) {
    const auto index = headers_.size();
    auto& header = headers_.addUninitializedValue(key, valueSize, knownBit);
    detail::setResponseHeaderAppend(header, true);
    recordKnownHeaderIndex(knownBit, index);
    return header;
}

HttpResponseHeader& HttpResponse::upsertSetCookieHeaderUninitializedValue(
    std::string_view wirePrefix,
    std::string_view cookieName,
    std::size_t valueSize) {
    auto* retained = findSetCookieHeader(wirePrefix, cookieName);
    if (retained == nullptr) {
        return appendHeaderUninitializedValue(
            "Set-Cookie", valueSize, detail::kResponseHeaderSetCookie);
    }

    headers_.assignUninitializedValue(
        *retained, "Set-Cookie", valueSize, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, wirePrefix, cookieName);
    return *retained;
}

void HttpResponse::upsertSetCookieHeaderValidated(std::string_view value) {
    const auto cookieName = setCookieWireName(value);
    if (cookieName.empty()) {
        appendHeaderValidated(
            "Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    auto* retained = findSetCookieHeader({}, cookieName);
    if (retained == nullptr) {
        appendHeaderValidated(
            "Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    headers_.assign(
        *retained, "Set-Cookie", value, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, {}, cookieName);
}

HttpResponseHeader* HttpResponse::findSetCookieHeader(
    std::string_view wirePrefix,
    std::string_view cookieName) noexcept {
    for (auto& header : headers_) {
        if (detail::responseHeaderKnownBit(header) == detail::kResponseHeaderSetCookie &&
            setCookieValueHasWireName(header.value(), wirePrefix, cookieName)) {
            return &header;
        }
    }
    return nullptr;
}

void HttpResponse::eraseLaterSetCookieHeaders(
    HttpResponseHeader& retained,
    std::string_view wirePrefix,
    std::string_view cookieName) noexcept {
    // A response might already contain duplicates introduced through the raw
    // header API. Once an authoritative cookie path owns this name, collapse
    // every later occurrence so the final response has one value.
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = &retained + 1;
    for (auto* read = &retained + 1; read != end; ++read) {
        if (detail::responseHeaderKnownBit(*read) == detail::kResponseHeaderSetCookie &&
            setCookieValueHasWireName(read->value(), wirePrefix, cookieName)) {
            headers_.releaseHeader(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end) {
        if (headers_.spilled_) {
            headers_.heap_.erase(
                headers_.heap_.begin() + static_cast<std::ptrdiff_t>(write - begin),
                headers_.heap_.end());
        } else {
            headers_.size_ = static_cast<std::size_t>(write - begin);
        }
    }
    rebuildKnownHeaderIndex();
}

HttpResponseHeader& HttpResponse::collapseResponseHeaders(
    HttpResponseHeader& retained,
    std::string_view key,
    std::uint32_t knownBit) noexcept {
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* const retainedAddress = &retained;
    auto* collapsedRetained = retainedAddress;
    auto* write = begin;
    for (auto* read = begin; read != end; ++read) {
        const auto headerKnownBit = detail::responseHeaderKnownBit(*read);
        const bool matches = knownBit != 0
            ? headerKnownBit == knownBit
            : detail::httpAsciiEqualsIgnoreCase(read->name(), key);
        if (matches && read != retainedAddress) {
            headers_.releaseHeader(*read);
            continue;
        }
        if (read == retainedAddress) {
            collapsedRetained = write;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end) {
        if (headers_.spilled_) {
            headers_.heap_.erase(
                headers_.heap_.begin() + static_cast<std::ptrdiff_t>(write - begin),
                headers_.heap_.end());
        } else {
            headers_.size_ = static_cast<std::size_t>(write - begin);
        }
    }
    rebuildKnownHeaderIndex();
    return *collapsedRetained;
}

bool HttpResponse::removeHeaderValidated(std::string_view key, std::uint32_t knownBit) noexcept {
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = begin;
    bool removed = false;

    for (auto* read = begin; read != end; ++read) {
        const auto headerKnownBit = detail::responseHeaderKnownBit(*read);
        const bool matches = knownBit != 0
            ? headerKnownBit == knownBit
            : detail::httpAsciiEqualsIgnoreCase(read->name(), key);
        if (matches) {
            headers_.releaseHeader(*read);
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (!removed) {
        return false;
    }

    if (headers_.spilled_) {
        headers_.heap_.erase(
            headers_.heap_.begin() + static_cast<std::ptrdiff_t>(write - begin),
            headers_.heap_.end());
    } else {
        headers_.size_ = static_cast<std::size_t>(write - begin);
    }
    rebuildKnownHeaderIndex();
    return true;
}

void HttpResponse::setHeaderStableView(std::string_view key, std::string_view value) {
    const auto knownBit = detail::classifyResponseHeaderName(key);
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        const bool wasAppended = detail::responseHeaderAppend(*header);
        headers_.assignStableView(*header, key, value, knownBit);
        if (wasAppended) {
            (void)collapseResponseHeaders(*header, key, knownBit);
        }
        return;
    }

    const auto index = headers_.size();
    headers_.addStableView(key, value, knownBit);
    recordKnownHeaderIndex(knownBit, index);
}

void HttpResponse::setHeaderUnsigned(std::string_view key, std::uint64_t value, std::uint32_t knownBit) {
    auto& header = prepareHeaderValueStorage(key, detail::httpUnsignedDecimalSize(value), knownBit);
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
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto endOffset = offset + length - 1;
    const auto valueSize = std::string_view("bytes ").size() +
        detail::httpUnsignedDecimalSize(offset) +
        1 +
        detail::httpUnsignedDecimalSize(endOffset) +
        1 +
        detail::httpUnsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, detail::kResponseHeaderContentRange);
    writeContentRangeHeaderValue(header, offset, length, size);
}

void HttpResponse::setContentRangeUnsatisfied(std::uint64_t size) {
    const auto valueSize = std::string_view("bytes */").size() + detail::httpUnsignedDecimalSize(size);
    auto& header = prepareHeaderValueStorage("Content-Range", valueSize, detail::kResponseHeaderContentRange);
    writeContentRangeUnsatisfiedHeaderValue(header, size);
}

void HttpResponse::reserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

}  // namespace ruvia
