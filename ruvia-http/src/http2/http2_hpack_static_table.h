#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/http_status.h"

#include "field/static_field_lookup.h"

namespace ruvia::detail {

struct hpack_static_header final {
    std::string_view name_;
    std::string_view value_;
};

inline constexpr auto hpack_status_ok_token = http_status_code_token(http_status::ok);
inline constexpr auto hpack_status_no_content_token = http_status_code_token(http_status::no_content);
inline constexpr auto hpack_status_partial_content_token =
    http_status_code_token(http_status::partial_content);
inline constexpr auto hpack_status_not_modified_token = http_status_code_token(http_status::not_modified);
inline constexpr auto hpack_status_bad_request_token = http_status_code_token(http_status::bad_request);
inline constexpr auto hpack_status_not_found_token = http_status_code_token(http_status::not_found);
inline constexpr auto hpack_status_internal_server_error_token =
    http_status_code_token(http_status::internal_server_error);

// Normative HPACK static table from RFC 7541 Appendix A.
inline constexpr std::array<hpack_static_header, 61> hpack_static_table{{
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", http_status_code_token_view(hpack_status_ok_token)},
    {":status", http_status_code_token_view(hpack_status_no_content_token)},
    {":status", http_status_code_token_view(hpack_status_partial_content_token)},
    {":status", http_status_code_token_view(hpack_status_not_modified_token)},
    {":status", http_status_code_token_view(hpack_status_bad_request_token)},
    {":status", http_status_code_token_view(hpack_status_not_found_token)},
    {":status", http_status_code_token_view(hpack_status_internal_server_error_token)},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

inline constexpr std::size_t hpack_static_table_size = hpack_static_table.size();
inline constexpr static_field_lookup<hpack_static_table> hpack_static_fields;

[[nodiscard]] inline const hpack_static_header& hpack_static_header_at(std::uint32_t index) noexcept {
    return hpack_static_table[index - 1];
}

}  // namespace ruvia::detail
