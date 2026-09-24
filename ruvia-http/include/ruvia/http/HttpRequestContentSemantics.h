#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

// RFC 9110 method requirements for a request that explicitly carries content.
// Senders and recipients use the same protocol rule regardless of wire version.
enum class HttpRequestContentSemantics : std::uint8_t {
    kNoAdditionalRequirements,
    kForbidden,
    kContentTypeRequired,
};

[[nodiscard]] constexpr HttpRequestContentSemantics httpRequestContentSemantics(
    std::string_view method) noexcept {
    if (method == "CONNECT" || method == "TRACE") {
        return HttpRequestContentSemantics::kForbidden;
    }
    if (method == "OPTIONS") {
        return HttpRequestContentSemantics::kContentTypeRequired;
    }
    return HttpRequestContentSemantics::kNoAdditionalRequirements;
}

}  // namespace ruvia
