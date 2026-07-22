#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/HttpConnectionFields.h"

// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: how many wire
// bytes it costs, which singleton fields it already carries, and the connection
// and upgrade options it expresses.

namespace ruvia {

// The line terminator both the sizing pass and the encoder count in bytes.
inline constexpr std::string_view kCrlf = "\r\n";

struct RequestHeaderFacts final {
    std::size_t wireBytes{0};
    std::uint32_t singletonHeaders{0};
    detail::HttpConnectionOptions connectionOptions;
    detail::HttpUpgradeProtocols upgradeProtocols;
    bool hasContentType{false};
    bool hasTe{false};
};

// Decimal width of a value, for sizing a head before it is filled.
[[nodiscard]] constexpr std::size_t decimalDigits(std::size_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// Accumulate head bytes, refusing to exceed the header-section ceiling.
[[nodiscard]] bool addHeadBytes(std::size_t& total, std::size_t bytes) noexcept;

// A client TE field may only offer "trailers" (RFC 9110 section 10.1.4); a
// transfer coding there would ask the origin for a framing this client does not
// implement.
[[nodiscard]] bool isValidClientTeField(std::string_view value) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyzeHeaders(
    std::span<const HttpHeaderView> headers,
    RequestHeaderFacts& facts,
    Http1ClientRequestPrepareError& error) noexcept;

}  // namespace ruvia
