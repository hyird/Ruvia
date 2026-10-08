#pragma once

#include <string_view>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"

namespace ruvia::detail {

// A request-line parse failure whose last token is not an HTTP-version is
// non-HTTP traffic (port scan, TLS on a cleartext port) and is dropped. A
// well-formed `HTTP/` version token is a real request and must be answered.
[[nodiscard]] inline bool shouldDropInvalidCleartextHttp1Input(
    std::string_view buffer, Http1ServerRequestParseFailureSource failureSource) noexcept {
    if (failureSource != Http1ServerRequestParseFailureSource::kRequestLine) {
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
    return !version.starts_with("HTTP/");
}

}  // namespace ruvia::detail
