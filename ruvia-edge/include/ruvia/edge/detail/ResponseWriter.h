#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>

#include <asio/awaitable.hpp>

#include "ruvia/edge/detail/proxy/HeaderRules.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::edge {

// Runtime product policy contributed to the HTTP/1 connection plan. HTTP/2
// ignores it because stream completion does not determine connection reuse.
enum class ResponseReusePolicy : std::uint8_t {
    kAllow,
    kClose,
};

[[nodiscard]] inline ResponseReusePolicy responseReusePolicy(bool keepAlive) noexcept {
    return keepAlive ? ResponseReusePolicy::kAllow : ResponseReusePolicy::kClose;
}

// Build the one semantic response consumed by both protocol adapters. Origin
// framing is never copied through. A caller with validated representation
// length metadata supplies it to its protocol adapter; otherwise the HTTP plan
// derives framing from the actual buffered body or streaming mode.
[[nodiscard]] inline HttpResponse makeEdgeResponse(
    std::pmr::memory_resource* resource,
    std::uint16_t status,
    const Headers& headers,
    std::string_view cacheResult,
    std::optional<std::uint64_t> age) {
    HttpResponse response(resource);
    response.status(HttpStatusCode::tryFromValue(status).value_or(http_status::kInternalServerError));
    for (const auto& [name, value] : headers) {
        const bool nominated = connectionNominates(headers, name);
        const bool framing = isConnectionOrFramingField(name);
        if (nominated || framing) {
            continue;
        }
        if (age.has_value() && iequals(name, "age")) {
            continue;
        }
        response.header(name, value);
    }
    response.header("X-Cache", cacheResult);
    if (age.has_value()) {
        std::array<char, 20> digits;
        const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), *age);
        if (ec == std::errc{}) {
            response.header("Age", std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
        }
    }
    return response;
}

// A status-only response: no headers, no body, no age. `cacheResult` is the
// label the access log records -- "ERROR" for a failure the edge generated,
// "MISS" for a request the cache could not answer and was forbidden to fetch.
// Whether the connection survives is a typed runtime policy, not part of the
// status itself. Writer is a compile-time adapter; no virtual dispatch occurs
// for response chunks.
template <typename Writer>
[[nodiscard]] inline asio::awaitable<bool> respondStatusOnly(Writer& writer, std::uint16_t status, std::string_view cacheResult, ResponseReusePolicy reusePolicy) {
    const Headers noHeaders;
    co_return co_await writer.respond(status, noHeaders, {}, cacheResult, std::nullopt, reusePolicy);
}

}  // namespace ruvia::edge
