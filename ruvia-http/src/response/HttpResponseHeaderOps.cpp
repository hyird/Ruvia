#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/util/HttpNumberFormat.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseKnownHeaders.h"
#include "ruvia/http/detail/response/ResponseHeaderIndexCache.h"

#include <charconv>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ruvia {
namespace {

void writeUnsignedHeaderValue(HttpResponseHeader& header, std::uint64_t value) {
    auto* const begin = detail::responseHeaderValueBegin(header);
    auto* const end = detail::responseHeaderValueEnd(header);
    const auto [ptr, ec] = std::to_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::logic_error("failed to format HTTP response header value");
    }
}

void validateConnectionControlField(std::string_view name, std::string_view value) {
    if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
        detail::HttpConnectionOptions options;
        if (options.parseField(value, detail::HttpFieldListRole::kSender, [](std::string_view option) noexcept { return !detail::httpConnectionOptionConflictsWithManagedField(option); }) != detail::HttpFieldListParseStatus::kOk) {
            throw std::invalid_argument("invalid HTTP Connection header");
        }
    } else if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
        detail::HttpUpgradeProtocols protocols;
        if (protocols.parseField(value, detail::HttpFieldListRole::kSender, [](const detail::HttpUpgradeProtocol&) noexcept { return true; }) != detail::HttpFieldListParseStatus::kOk) {
            throw std::invalid_argument("invalid HTTP Upgrade header");
        }
    } else if (detail::httpAsciiEqualsIgnoreCase(name, "TE")) {
        throw std::invalid_argument("TE is not a response field");
    }
}

[[nodiscard]] bool responseHeaderModeAppends(HttpResponseHeaderMode mode) {
    switch (mode) {
        case HttpResponseHeaderMode::kReplace:
            return false;
        case HttpResponseHeaderMode::kAppend:
            return true;
    }
    throw std::invalid_argument("invalid HTTP response header mode");
}

}  // namespace

void HttpResponse::recordKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept {
    knownHeaderBits_ |= knownBit;
    detail::recordResponseHeaderIndex(knownHeaderIndexes_, detail::responseKnownHeaderSlot(knownBit), index);
}

HttpResponseHeader* HttpResponse::findHeaderForUpdate(std::string_view key, std::uint32_t knownBit) noexcept {
    return const_cast<HttpResponseHeader*>(std::as_const(*this).findHeaderForRead(key, knownBit));
}

const HttpResponseHeader* HttpResponse::findHeaderForRead(std::string_view key, std::uint32_t knownBit) const noexcept {
    const auto* const begin = headers_.begin();
    const auto* const end = headers_.end();
    const auto* const header = detail::findResponseHeaderIndexed(begin, end, knownHeaderIndexes_, detail::responseKnownHeaderSlot(knownBit), key, knownBit);
    return header == end ? nullptr : header;
}

HttpResponseHeader& HttpResponse::prepareHeaderValueStorage(std::string_view key, std::size_t valueSize, std::uint32_t knownBit) {
    if (auto* const header = findHeaderForUpdate(key, knownBit)) {
        const bool wasAppended = detail::responseHeaderAppend(*header);
        headers_.assignUninitializedValue(*header, key, valueSize, knownBit);
        return wasAppended ? collapseResponseHeaders(*header, key, knownBit) : *header;
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

std::optional<std::string_view> HttpResponse::header(std::string_view name) const& noexcept {
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
        detail::recordResponseHeaderIndex(knownHeaderIndexes_, detail::responseKnownHeaderSlot(knownBit), static_cast<std::size_t>(cursor - begin));
    }
}

void HttpResponse::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void HttpResponse::header(std::string_view key, std::string_view value, HeaderOptions options) {
    // Check the descriptor's representable storage before any grammar scan;
    // callers may provide a view over a bounded buffer with a hostile length.
    detail::validateResponseHeaderStorageSize(key.size(), value.size());
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    validateConnectionControlField(key, value);
    const auto knownBit = detail::classifyResponseHeaderName(key);
    if (knownBit == detail::kResponseHeaderContentType && !detail::isValidHttpContentTypeFieldValue(value)) {
        throw std::invalid_argument("invalid HTTP Content-Type header");
    }
    if (knownBit == detail::kResponseHeaderContentEncoding && !detail::isValidHttpContentEncodingFieldValue(value, detail::HttpFieldListRole::kSender)) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    if (detail::httpAsciiEqualsIgnoreCase(key, "Trailer") && !detail::isValidHttpResponseTrailerFieldValue(value, detail::HttpFieldListRole::kSender)) {
        throw std::invalid_argument("invalid HTTP Trailer header");
    }
    if (responseHeaderModeAppends(options.mode)) {
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

void HttpResponse::removeHeader(std::string_view key) {
    detail::validateResponseHeaderStorageSize(key.size(), 0);
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)removeHeaderValidated(key, detail::classifyResponseHeaderName(key));
}

void HttpResponse::setHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit) {
    detail::validateResponseHeaderStorageSize(key.size(), value.size());
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

void HttpResponse::appendHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit) {
    detail::validateResponseHeaderStorageSize(key.size(), value.size());
    if (detail::responseHeaderAppendForbidden(knownBit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update. Do
    // not set it until the new descriptor has been published: headers_.add()
    // owns bytes before it may allocate the backing table, and a failed append
    // must leave the existing response exactly as it was.
    const bool hadExisting = findHeaderForUpdate(key, knownBit) != nullptr;
    const auto index = headers_.size();
    auto& header = headers_.add(key, value, knownBit);
    if (hadExisting) {
        auto* const existing = findHeaderForUpdate(key, knownBit);
        if (existing != nullptr) {
            detail::setResponseHeaderAppend(*existing, true);
        }
    }
    // Mark the append flag so a later merge of this response keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::setResponseHeaderAppend(header, true);
    recordKnownHeaderIndex(knownBit, index);
}

HttpResponseHeader& HttpResponse::appendHeaderUninitializedValue(std::string_view key, std::size_t valueSize, std::uint32_t knownBit) {
    detail::validateResponseHeaderStorageSize(key.size(), valueSize);
    const auto index = headers_.size();
    auto& header = headers_.addUninitializedValue(key, valueSize, knownBit);
    detail::setResponseHeaderAppend(header, true);
    recordKnownHeaderIndex(knownBit, index);
    return header;
}

HttpResponseHeader& HttpResponse::collapseResponseHeaders(HttpResponseHeader& retained, std::string_view key, std::uint32_t knownBit) noexcept {
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* const retainedAddress = &retained;
    auto* collapsedRetained = retainedAddress;
    auto* write = begin;
    for (auto* read = begin; read != end; ++read) {
        const auto headerKnownBit = detail::responseHeaderKnownBit(*read);
        const bool matches = knownBit != 0 ? headerKnownBit == knownBit : detail::httpAsciiEqualsIgnoreCase(read->name(), key);
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
        detail::HttpResponseHeadersAccess::truncate(headers_, begin, write);
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
        const bool matches = knownBit != 0 ? headerKnownBit == knownBit : detail::httpAsciiEqualsIgnoreCase(read->name(), key);
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

    detail::HttpResponseHeadersAccess::truncate(headers_, begin, write);
    rebuildKnownHeaderIndex();
    return true;
}

void HttpResponse::setHeaderStableView(std::string_view key, std::string_view value) {
    detail::validateResponseHeaderStorageSize(key.size(), value.size());
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

void HttpResponse::reserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

}  // namespace ruvia
