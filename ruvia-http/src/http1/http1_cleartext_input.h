#pragma once

#include <string_view>

#include "ruvia/http/detail/http1/http1_server_request_parser.h"

namespace ruvia::detail {

// A request-line parse failure whose last token is not an HTTP-version is
// non-HTTP traffic (port scan, TLS on a cleartext port) and is dropped. A
// well-formed `HTTP/` version token is a real request and must be answered.
[[nodiscard]] inline bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source failure_source) noexcept {
    if (failure_source != http1_server_request_parse_failure_source::request_line) {
        return false;
    }

    const auto line_end = buffer.find("\r\n");
    if (line_end == std::string_view::npos) {
        return false;
    }

    auto line = buffer.substr(0, line_end);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }

    const auto version_start = line.find_last_of(" \t");
    if (version_start == std::string_view::npos || version_start + 1 >= line.size()) {
        return false;
    }
    const auto version = line.substr(version_start + 1);
    return !version.starts_with("HTTP/");
}

}  // namespace ruvia::detail
