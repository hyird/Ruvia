#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::edge {

// An owned header list: the shape a response carries once it leaves the borrowed
// parser buffers (origin response, cache entry, response to the client).
using Headers = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] inline char toLowerAscii(char c) noexcept {
    return static_cast<char>(ruvia::detail::httpAsciiToLower(static_cast<unsigned char>(c)));
}

[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    return ruvia::detail::httpAsciiEqualsIgnoreCase(a, b);
}

[[nodiscard]] inline bool istartsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && iequals(value.substr(0, prefix.size()), prefix);
}

// Fields a proxy must not forward (RFC 9110 section 7.6.1), plus the framing
// fields the edge regenerates itself. Matching is case-insensitive so callers
// do not allocate a lowercase copy on the request path. Age is handled
// separately: it is dropped only when the edge emits its own computed value.
[[nodiscard]] bool isConnectionOrFramingField(std::string_view name) noexcept;

// The client's precondition and Range fields. A caching edge answers these
// itself from the stored response, so it must not also forward them.
[[nodiscard]] bool isConditionalOrRangeField(std::string_view name) noexcept;

// Whether a Connection header nominates `fieldName` as hop-by-hop.
[[nodiscard]] bool connectionNominates(std::span<const HttpHeaderView> headers, std::string_view fieldName) noexcept;

[[nodiscard]] bool connectionNominates(const Headers& headers, std::string_view fieldName) noexcept;

// Strip the origin hop-by-hop section before it can enter either response
// serialization or persistent cache metadata. Content-Length is retained as
// representation metadata; the client writer still owns the actual framing.
[[nodiscard]] Headers endToEndResponseHeaders(const Headers& headers);

// Update a stored response's headers with those from a 304 (RFC 9111 section
// 4.3.4): keep the stored fields, but replace any also present in the 304, and
// ignore the 304's connection/framing fields.
[[nodiscard]] Headers mergeStoredHeaders(const Headers& stored, const Headers& updates);

// Whether a response may be cached given its Vary header. Absent Vary or a Vary
// of only Accept-Encoding is cacheable (the key already accounts for it); Vary:*
// or any other varying field is not (this MVP keys only on Accept-Encoding).
[[nodiscard]] bool cacheableUnderVary(const Headers& headers);

[[nodiscard]] std::optional<std::string_view> findHeaderValue(const Headers& headers, std::string_view name);

// Look up a request header (case-insensitive) in a borrowed header span.
[[nodiscard]] std::optional<std::string_view> findRequestHeader(std::span<const HttpHeaderView> headers, std::string_view name);

// Combine every field line exactly as RFC 9110 section 5.2 permits a recipient
// to combine a list field. The raw weights, order and malformed bytes remain in
// the cache key: interpreting them here could merge requests for which an origin
// legitimately selects different representations. `nullopt` is distinct from
// a present empty field because Accept-Encoding assigns those states different
// semantics (RFC 9110 section 12.5.3).
[[nodiscard]] std::optional<std::string> combinedRequestFieldValue(std::span<const HttpHeaderView> headers, std::string_view name);

// The host without its optional :port, for origin lookup and cache keys. An IPv6
// literal keeps its brackets ("[::1]:443" -> "[::1]").
[[nodiscard]] std::string_view hostWithoutPort(std::string_view host) noexcept;

}  // namespace ruvia::edge
