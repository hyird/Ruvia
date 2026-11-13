#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/http1_response_head_error.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_coding.h"
#include "field/http_connection_fields.h"
#include "memory/http_c_library_memory.h"

namespace ruvia::detail {

[[nodiscard]] constexpr http_response_content_semantics classify_http_response_content_semantics(
    http_known_method request_method, http_status_code status_code) noexcept {
    if (status_code == http_status::switching_protocols) {
        return http_response_content_semantics::protocol_switch;
    }
    if (status_code.is_informational()) {
        return http_response_content_semantics::informational;
    }
    if (request_method == http_known_method::connect && status_code.is_successful()) {
        return http_response_content_semantics::connect_tunnel;
    }
    if (request_method == http_known_method::head || status_code == http_status::no_content ||
        status_code == http_status::not_modified) {
        return http_response_content_semantics::without_content;
    }
    return http_response_content_semantics::with_content;
}

struct http_response_body_access final {
    [[nodiscard]] static const http_response_body& body(const http_response& response) noexcept {
        return response.body_;
    }
    [[nodiscard]] static const http_response_body& body(const http_response&& response) = delete;
};

[[nodiscard]] inline const http_response_body& response_body(const http_response& response) noexcept {
    return http_response_body_access::body(response);
}
[[nodiscard]] const http_response_body& response_body(const http_response&& response) = delete;

struct http_response_header_state_access final {
    [[nodiscard]] static std::uint32_t known_bits(const http_response_headers& headers) noexcept {
        return headers.known_header_bits_;
    }

    [[nodiscard]] static const http_response_header* known_header(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        return headers.find_header_for_read({}, bit);
    }

    [[nodiscard]] static std::string_view known_header_value(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        const auto* const header_value = known_header(headers, bit);
        return header_value == nullptr ? std::string_view{} : header_value->value();
    }
};

struct http_response_header_access final {
    [[nodiscard]] static constexpr http_response_header make(const char* bytes_value, std::uint32_t name_size,
        std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
        http_response_header header_value{};
        header_value.bytes_ = bytes_value;
        header_value.name_size_ = name_size;
        header_value.value_size_ = value_size;
        header_value.known_bit_ = known_bit;
        header_value.owned_ = owned;
        header_value.append_ = false;
        return header_value;
    }

    [[nodiscard]] static char* value_begin(http_response_header& header_value) noexcept {
        if (header_value.bytes_ == nullptr) {
            return nullptr;
        }
        return const_cast<char*>(header_value.bytes_) + header_value.name_size_;
    }

    [[nodiscard]] static char* value_end(http_response_header& header_value) noexcept {
        auto* const begin = value_begin(header_value);
        return begin == nullptr ? nullptr : begin + header_value.value_size_;
    }

    [[nodiscard]] static std::uint32_t known_bit(const http_response_header& header_value) noexcept {
        return header_value.known_bit_;
    }

    [[nodiscard]] static bool append(const http_response_header& header_value) noexcept {
        return header_value.append_;
    }

    static void set_append(http_response_header& header_value) noexcept {
        header_value.append_ = true;
    }
};

[[nodiscard]] inline std::uint32_t response_known_header_bits(const http_response& response) noexcept {
    return http_response_header_state_access::known_bits(response.headers());
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response_headers& headers, std::uint32_t bit) noexcept {
    return (http_response_header_state_access::known_bits(headers) & bit) != 0;
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return response_has_known_header(response.headers(), bit);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// http_response_header stores both lengths in uint32_t and releases the one
// name/value allocation using their sum. Never let a public or runtime helper
// construct a descriptor whose wire/storage lengths cannot be represented;
// truncating either field would make the response header view disagree with
// the allocation and could turn a large application value into malformed wire
// bytes or an out-of-bounds read during emission.
[[nodiscard]] inline constexpr bool response_header_storage_size_fits(
    std::size_t name_size, std::size_t value_size) noexcept {
    constexpr auto max_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    return name_size <= max_size && value_size <= max_size && value_size <= max_size - name_size;
}

inline void validate_response_header_storage_size(std::size_t name_size, std::size_t value_size) {
    if (!response_header_storage_size_fits(name_size, value_size)) {
        throw std::length_error("HTTP response header is too large");
    }
}

// The one field-identity rule: known fields match by bit, unknown fields by
// case-insensitive name among unknown fields only.
[[nodiscard]] inline bool response_header_matches(
    const http_response_header& header_value, std::string_view key, std::uint32_t known_bit) noexcept {
    const auto header_known_bit = http_response_header_access::known_bit(header_value);
    return known_bit != 0 ? header_known_bit == known_bit
                          : header_known_bit == 0 && http_ascii_equals_ignore_case(header_value.name(), key);
}

[[nodiscard]] inline bool response_header_storage_overlaps(
    const http_response_header& header_value, std::string_view value) noexcept {
    const auto name = header_value.name();
    if (name.data() == nullptr) {
        return false;
    }
    return http_text_overlaps(
        {name.data(), name.size() + header_value.value().size()}, value);
}

[[nodiscard]] inline bool response_header_views_contiguous(
    std::string_view name, std::string_view value) noexcept {
    if (name.data() == nullptr) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    if (value.data() == nullptr) {
        return false;
    }
    return reinterpret_cast<std::uintptr_t>(value.data()) ==
           reinterpret_cast<std::uintptr_t>(name.data() + name.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http_binary_final_response_control_plan_error : std::uint8_t {
    invalid_status,
    upgrade_unavailable,
    connection_specific_field_forbidden
};

class http1_final_response_control;

[[nodiscard]] http1_final_response_control http1_final_response_control_plan(const http_response& response);

[[nodiscard]] std::optional<http_binary_final_response_control_plan_error> http_binary_final_response_control_plan(
    const http_response& response) noexcept;
// The protocol writer alone frames a response (RFC 9112 section 6.1, RFC 9113
// section 8.2.2), so an application Transfer-Encoding is accepted only as a
// hint that names exactly the single `chunked` coding, which every writer
// replaces with its own framing. Any other value claims a coding the writer
// never applies and fails closed on both protocols.
[[nodiscard]] inline bool response_transfer_encoding_replaceable(const http_response& response) noexcept {
    if (!response_has_known_header(response, response_header_transfer_encoding)) {
        return true;
    }
    return http_ascii_equals_ignore_case(
        http_trim_ows(http_response_header_state_access::known_header_value(
            response.headers(), response_header_transfer_encoding)),
        "chunked");
}

class http1_final_response_control final {
public:
    [[nodiscard]] http_connection_options connection_options() const noexcept {
        return connection_options_;
    }

    [[nodiscard]] http_upgrade_protocols upgrade_protocols() const noexcept {
        return upgrade_protocols_;
    }

private:
    friend http1_final_response_control http1_final_response_control_plan(const http_response&);

    http1_final_response_control(
        http_connection_options connection_options, http_upgrade_protocols upgrade_protocols) noexcept
        : connection_options_(connection_options),
          upgrade_protocols_(upgrade_protocols) {}

    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
};

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields. http_response
// already rejects a non-final status and malformed Connection, Upgrade and TE
// fields on insertion; these checks keep the commit self-contained.
[[nodiscard]] inline http1_final_response_control http1_final_response_control_plan(const http_response& response) {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        throw http1_response_head_error("invalid final HTTP response status");
    }
    if (!response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    if (response_has_known_header(
            response, response_header_connection | response_header_upgrade)) {
        for (const auto& header : response.headers()) {
            switch (http_response_header_access::known_bit(header)) {
                case response_header_connection:
                    if (connection_options.parse_field(header.value(), http_connection_field_role::sender) !=
                        http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Connection header");
                    }
                    break;
                case response_header_upgrade:
                    if (upgrade_protocols.parse_field(header.value(), http_field_list_role::sender) !=
                        http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Upgrade header");
                    }
                    break;
                default:
                    break;
            }
        }
    }
    // RFC 9110 §15.5.22: a 426 response MUST send an Upgrade field.
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        throw http1_response_head_error("Upgrade Required response requires an Upgrade protocol");
    }
    return http1_final_response_control(connection_options, upgrade_protocols);
}

// Validate HTTP/2 and HTTP/3 control semantics before field encoding or stream
// state is mutated. Success proves that no connection-specific field exists;
// RFC 9113 section 8.2.2 and RFC 9114 section 4.2 require rejection rather than
// silent filtering. A replaceable Transfer-Encoding hint is not application
// metadata but writer-owned framing, so both encoders omit it.
[[nodiscard]] inline std::optional<http_binary_final_response_control_plan_error>
http_binary_final_response_control_plan(const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        return http_binary_final_response_control_plan_error::invalid_status;
    }
    if (status_code == http_status::upgrade_required) {
        return http_binary_final_response_control_plan_error::upgrade_unavailable;
    }
    if (response_has_known_header(response,
            response_header_connection | response_header_upgrade | response_header_keep_alive |
                response_header_proxy_connection) ||
        !response_transfer_encoding_replaceable(response)) {
        return http_binary_final_response_control_plan_error::connection_specific_field_forbidden;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail

// Shared response Content-Length semantics. HTTP/1 adds its chunked and
// close-delimited framing policy; HTTP/2 and HTTP/3 delimit content with DATA
// frames but enforce the same normalized field value.

namespace ruvia::detail {

enum class http_framed_response_head_plan_error : std::uint8_t {
    invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch
};

class http_framed_response_head_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // The normalized Content-Length field to encode, if any.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::omit
                   ? std::nullopt
                   : std::optional<std::uint64_t>(content_length_);
    }

    // Only an application-declared streaming length constrains subsequent DATA.
    // Framework-generated buffered lengths are already bound to their response
    // representation and therefore do not create a streaming accounting limit.
    [[nodiscard]] std::optional<std::uint64_t> streaming_content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::explicit_value
                   ? std::optional<std::uint64_t>(content_length_)
                   : std::nullopt;
    }

    [[nodiscard]] static http_framed_response_head_plan canonical(
        http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::canonical, value);
    }

    [[nodiscard]] static std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>
    preserve_explicit(http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = http_response_header_state_access::known_header_value(
            response.headers(), response_header_content_length);
        http_content_length_state state;
        if (state.parse_field(value) != http_content_length_parse_status::ok) {
            return http_framed_response_head_plan_error::invalid_content_length;
        }
        return http_framed_response_head_plan(
            body_plan, content_length_mode_type::explicit_value, *state.value());
    }

    [[nodiscard]] static http_framed_response_head_plan omit(http_response_body_plan body_plan) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::omit);
    }

private:
    enum class content_length_mode_type : std::uint8_t {
        omit,
        canonical,
        explicit_value,
    };

    http_framed_response_head_plan(http_response_body_plan body_plan,
        content_length_mode_type content_length_mode, std::uint64_t content_length = 0) noexcept
        : body_plan_(body_plan),
          content_length_mode_(content_length_mode),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    content_length_mode_type content_length_mode_;
    std::uint64_t content_length_{0};
};

static_assert(std::is_trivially_copyable_v<http_framed_response_head_plan>);
static_assert(sizeof(http_framed_response_head_plan) <= 24);

using http_framed_response_head_plan_result =
    std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>;

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_response_body_plan& body_plan, std::uint64_t content_length,
    const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (content_length != body_plan.buffered_representation_length(response)) {
        return http_framed_response_head_plan_error::response_representation_mismatch;
    }
    // An empty suppressed HEAD body describes the GET representation only
    // through its declared Content-Length, if any (RFC 9110 section 8.6).
    if (body_plan.body_suppressed() && body_plan.status_allows_body() &&
        body_plan.explicit_content_length_allowed() && content_length == 0 &&
        (response.has_head_metadata_body() ||
            response_has_known_header(response, response_header_content_length))) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http_framed_response_head_plan::canonical(
            body_plan, body_plan.status_allows_body() ? content_length : 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    return http_framed_buffered_response_head_plan(
        write_plan.body_plan(), write_plan.content_length(), response);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_streaming_response_head_plan(
    const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http_framed_response_head_plan::canonical(body_plan, 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

// A successful CONNECT opens a tunnel and carries no Content-Length (RFC 9110
// §9.3.6); any other outcome must take the generic final-response path.
[[nodiscard]] inline http_framed_response_head_plan_result http_framed_connect_response_head_plan(
    const http_response_body_plan& body_plan) noexcept {
    if (body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel) {
        return http_framed_response_head_plan::omit(body_plan);
    }
    return http_framed_response_head_plan_error::connect_tunnel_required;
}

}  // namespace ruvia::detail
