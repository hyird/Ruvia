#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// Method requirements for a request that explicitly carries content. This is
// shared by senders and recipients so the selected wire version and endpoint
// role cannot change the request's method semantics. RFC 9110 Sections 9.3.6,
// 9.3.7, and 9.3.8 define the CONNECT, OPTIONS, and TRACE requirements below.
enum class HttpRequestContentSemantics : std::uint8_t {
    kNoAdditionalRequirements,
    kForbidden,
    kContentTypeRequired,
};

[[nodiscard]] constexpr HttpRequestContentSemantics httpRequestContentSemantics(std::string_view method) noexcept {
    if (method == "CONNECT" || method == "TRACE") {
        return HttpRequestContentSemantics::kForbidden;
    }
    if (method == "OPTIONS") {
        return HttpRequestContentSemantics::kContentTypeRequired;
    }
    return HttpRequestContentSemantics::kNoAdditionalRequirements;
}

}  // namespace ruvia::detail
