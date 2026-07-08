#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "Http2Frame.h"
#include "HttpRequestInternal.h"
#include "HttpParserInternal.h"
#include "HeaderTokenUtils.h"
#include "ruvia/detail/Base64Url.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

inline constexpr std::string_view kHttp2UpgradeResponsePrefix =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: h2c\r\n"
    "Server: ruvia\r\n";

struct Http2UpgradeRequest final {
    std::pmr::string settingsPayload;
    bool valid{false};

    explicit Http2UpgradeRequest(std::pmr::memory_resource* resource)
        : settingsPayload(resource) {}
};

[[nodiscard]] inline bool isHttp2UpgradeAttempt(const HttpServerParseResult& parsed) noexcept {
    return parsed.flags.upgrade &&
        asciiEqualsIgnoreCase(requestKnownHeader(parsed.request, RequestKnownHeader::kUpgrade), "h2c");
}

[[nodiscard]] inline bool http2UpgradeConnectionHasSettingsToken(const HttpRequest& request) noexcept {
    for (const auto& header : request.headers()) {
        if (asciiEqualsIgnoreCase(header.name(), "Connection") &&
            httpHasToken(header.value(), "HTTP2-Settings")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool http2ShouldDropInvalidCleartextPreface(
    std::string_view buffer,
    HttpParseError error) noexcept {
    if (error != HttpParseError::kUnsupportedMethod &&
        error != HttpParseError::kInvalidRequestLine &&
        error != HttpParseError::kUnsupportedHttpVersion) {
        return false;
    }

    const auto lineEnd = buffer.find("\r\n");
    if (lineEnd == std::string_view::npos) {
        return false;
    }

    auto line = buffer.substr(0, lineEnd);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }

    const auto versionStart = line.find_last_of(" \t");
    if (versionStart == std::string_view::npos || versionStart + 1 >= line.size()) {
        return false;
    }

    const auto version = line.substr(versionStart + 1);
    return version.size() < 5 || version.substr(0, 5) != "HTTP/";
}

[[nodiscard]] inline bool http2DecodeBase64Url(
    std::string_view input,
    std::pmr::string& output) {
    output.clear();
    if (input.empty()) {
        return false;
    }
    if (input.size() % 4 == 1) {
        return false;
    }
    output.reserve(((input.size() + 3) / 4) * 3);

    std::uint32_t buffer = 0;
    std::uint8_t bits = 0;
    for (const auto ch : input) {
        // RFC 7540 §3.2.1 requires HTTP2-Settings to be UNPADDED base64url. '=' is
        // not in the base64url alphabet, so decodeBase64UrlChar rejects it. This
        // also rejects a padded final group whose significant length is 1 -- e.g.
        // "A===" -- which the raw-length `% 4 == 1` guard above misses because the
        // trailing padding makes the total length a multiple of 4.
        const auto value = decodeBase64UrlChar(ch);
        if (value < 0) {
            return false;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits = static_cast<std::uint8_t>(bits + 6);
        if (bits >= 8) {
            bits = static_cast<std::uint8_t>(bits - 8);
            output.push_back(static_cast<char>((buffer >> bits) & 0xffU));
        }
    }
    if (bits != 0) {
        const auto mask = static_cast<std::uint32_t>((1U << bits) - 1U);
        if ((buffer & mask) != 0) {
            return false;
        }
    }
    return output.size() % 6 == 0;
}

[[nodiscard]] inline Http2UpgradeRequest parseHttp2UpgradeRequest(
    const HttpServerParseResult& parsed,
    std::pmr::memory_resource* resource) {
    Http2UpgradeRequest result(resource);
    if (!isHttp2UpgradeAttempt(parsed)) {
        return result;
    }
    if (!http2UpgradeConnectionHasSettingsToken(parsed.request)) {
        return result;
    }

    std::string_view encodedSettings;
    bool seenSettings = false;
    for (const auto& header : parsed.request.headers()) {
        if (!asciiEqualsIgnoreCase(header.name(), "HTTP2-Settings")) {
            continue;
        }
        // RFC 7540 3.2.1: a request with more than one HTTP2-Settings header
        // field is malformed. Track presence with a flag rather than the value's
        // emptiness, so a duplicate whose first occurrence is empty is still
        // rejected -- and so an absent header (only the Connection token) fails.
        if (seenSettings) {
            return result;
        }
        seenSettings = true;
        encodedSettings = header.value();
    }
    if (!seenSettings) {
        return result;
    }
    if (!http2DecodeBase64Url(encodedSettings, result.settingsPayload)) {
        return result;
    }
    result.valid = true;
    return result;
}

}  // namespace ruvia::detail
