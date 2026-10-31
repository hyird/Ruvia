#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/http_client.h"

namespace ruvia::detail {

[[nodiscard]] constexpr bool is_valid_http_request_field_source(http_request_field_source source) noexcept {
    return source == http_request_field_source::generated || source == http_request_field_source::forwarded;
}

// The only version-dependent request-field semantics: HTTP/1.1 negotiates
// transfer codings through TE, while HTTP/2 permits only "trailers".
enum class request_field_framing : std::uint8_t { http1,
    binary };

enum class request_field_status : std::uint8_t {
    ok,
    // Grammar violation, or a second instance of a field that may occur once.
    invalid,
    // A client generated the field itself. Expect and Content-Length are
    // derived from the request plan on every HTTP version, so a raw field
    // would be a second source of truth for the body gate or the framing.
    managed_by_sender,
};

// Per field section: which once-only fields were seen, plus the framing whose
// TE rules apply. One instance spans exactly one request header section.
class request_field_state final {
public:
    explicit request_field_state(request_field_framing framing) noexcept
        : framing_(framing) {}

    // True once a field that may occur only once (Host and every
    // singleton_request_header_bit() kind) has been validated.
    [[nodiscard]] bool seen(request_header_kind kind) const noexcept;

private:
    friend request_field_status validate_request_field(request_header_kind kind, std::string_view value,
        http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept;

    std::uint32_t seen_{0};
    request_field_framing framing_;
};

// The authoritative semantic check for one request field, shared by the
// HTTP/1 and HTTP/2 request writers (role sender) and request
// recipients (role recipient). Callers classify the name once with
// classify_request_header() and keep version-specific concerns themselves:
// framing state (Content-Length values on receipt, Transfer-Encoding,
// Connection, Upgrade), Host/:authority agreement, cookie joining, and parsing
// accepted Expect members.
[[nodiscard]] request_field_status validate_request_field(request_header_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state,
    http_request_field_source source = http_request_field_source::generated) noexcept;

}  // namespace ruvia::detail
