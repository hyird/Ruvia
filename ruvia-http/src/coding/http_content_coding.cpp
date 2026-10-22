#include "coding/http_content_coding.h"

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_response.h"

#include "coding/http_content_codec.h"
#include "response/http_response_header_access.h"

// The Content-Encoding field itself (RFC 9110 sections 8.4 and 5.6.1): the token
// each coding is spelled with, the list-grammar accumulator across field lines,
// and which codec a decode or encode of that coding runs on.

namespace ruvia {

std::string_view http_content_coding_token(http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::brotli:
            return "br";
        case http_content_coding::zstd:
            return "zstd";
        case http_content_coding::gzip:
            return "gzip";
        case http_content_coding::deflate:
            return "deflate";
        case http_content_coding::identity:
            return "identity";
    }
    return {};
}

std::optional<http_content_coding> http_content_coding_from_token(std::string_view token) noexcept {
    if (http_ascii_equals_ignore_case(token, "identity")) {
        return http_content_coding::identity;
    }
    if (http_is_gzip_coding_token(token)) {
        return http_content_coding::gzip;
    }
    if (http_ascii_equals_ignore_case(token, "deflate")) {
        return http_content_coding::deflate;
    }
    if (http_ascii_equals_ignore_case(token, "br")) {
        return http_content_coding::brotli;
    }
    if (http_ascii_equals_ignore_case(token, "zstd")) {
        return http_content_coding::zstd;
    }
    return std::nullopt;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// The one Content-Encoding list walk (RFC 9110 section 5.6.1): members are
// comma-separated tokens; recipients ignore empty members, senders cannot
// generate them. Returns false on invalid grammar, after which the visitor is
// not called again.
template <typename visitor_type>
[[nodiscard]] bool visit_content_coding_members(
    std::string_view value, http_field_list_role role, visitor_type&& visit) noexcept {
    std::size_t begin = 0;
    for (;;) {
        const auto comma = value.find(',', begin);
        const auto token = http_trim_ows(value.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (token.empty()) {
            if (role == http_field_list_role::sender) {
                return false;
            }
        } else if (!is_valid_http_header_name(token)) {
            return false;
        } else {
            visit(token);
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        begin = comma + 1;
    }
}

}  // namespace

void http_content_coding_field_parser::update(std::string_view value) noexcept {
    if (invalid_) {
        return;
    }
    invalid_ = !visit_content_coding_members(value, http_field_list_role::recipient, [this](std::string_view token) noexcept {
        const auto coding = http_content_coding_from_token(token);
        if (!coding.has_value()) {
            unsupported_ = true;
            return;
        }
        if (codings_.size_ == max_content_codings) {
            // Keep scanning: later malformed members still invalidate the field.
            unsupported_ = true;
        } else {
            codings_.values_[codings_.size_++] = *coding;
        }
    });
}

http_content_coding_field_result http_content_coding_field_parser::finish() const noexcept {
    if (invalid_) {
        return http_content_coding_field_result_access::invalid();
    }
    if (unsupported_) {
        return http_content_coding_field_result_access::unsupported();
    }
    return http_content_coding_field_result_access::coding(codings_);
}

bool is_valid_http_content_encoding_field_value(std::string_view value, http_field_list_role role) noexcept {
    // Received metadata is interpreted by the content decoder, not message framing.
    return role == http_field_list_role::recipient ||
           visit_content_coding_members(value, role, [](std::string_view) noexcept {});
}

}  // namespace ruvia::detail

namespace ruvia {

http_content_coding_field_result parse_http_content_coding(
    std::string_view value) noexcept {
    detail::http_content_coding_field_parser parser;
    parser.update(value);
    return parser.finish();
}

http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers) noexcept {
    return detail::http_content_coding_from_headers(headers);
}

http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers) noexcept {
    detail::http_content_coding_field_parser parser;
    if (!detail::response_has_known_header(headers, detail::response_header_content_encoding)) {
        return parser.finish();
    }
    for (const auto& header : headers) {
        if (detail::http_response_header_access::known_bit(header) ==
            detail::response_header_content_encoding) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

std::variant<std::pmr::string, http_content_decode_error> decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options) {
    if (input.empty() && !http_content_coding_token(coding).empty()) {
        // A zero-length coded representation carries no content: user agents
        // treat it as empty rather than as a truncated coding stream.
        return std::pmr::string(detail::http_pmr_resource_or_default(options.resource_));
    }
    switch (coding) {
        case http_content_coding::gzip:
            return detail::decode_gzip_content(input, options.max_decoded_bytes_, options.resource_);
        case http_content_coding::deflate:
            return detail::decode_deflate_content(input, options.max_decoded_bytes_, options.resource_);
        case http_content_coding::brotli:
            return detail::decode_brotli_content(input, options.max_decoded_bytes_, options.resource_);
        case http_content_coding::zstd:
            return detail::decode_zstd_content(input, options.max_decoded_bytes_, options.resource_);
        case http_content_coding::identity:
            if (input.size() > options.max_decoded_bytes_) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            return std::pmr::string(
                input, detail::http_pmr_resource_or_default(options.resource_));
    }
    return http_content_decode_error::unsupported_coding;
}

std::variant<std::pmr::string, http_content_encode_error> encode_http_content(
    http_content_coding coding, std::string_view input, http_content_encode_options options) {
    switch (coding) {
        case http_content_coding::brotli:
            return detail::encode_brotli_content(input, options.max_encoded_bytes_, options.resource_);
        case http_content_coding::zstd:
            return detail::encode_zstd_content(input, options.max_encoded_bytes_, options.resource_);
        case http_content_coding::gzip:
            return detail::encode_gzip_content(input, options.max_encoded_bytes_, options.resource_);
        case http_content_coding::deflate:
            return detail::encode_deflate_content(input, options.max_encoded_bytes_, options.resource_);
        case http_content_coding::identity:
            if (input.size() > options.max_encoded_bytes_) {
                return http_content_encode_error::encoded_size_exceeded;
            }
            return std::pmr::string(
                input, detail::http_pmr_resource_or_default(options.resource_));
    }
    return http_content_encode_error::encoder_failure;
}

std::variant<std::pmr::string, http_content_encode_error> encode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_encode_options options) {
    auto* const resource = detail::http_pmr_resource_or_default(options.resource_);
    std::pmr::string current(resource);
    std::string_view source_value = input;
    bool encoded = false;
    for (const auto coding : codings) {
        // identity transforms nothing; only the size limit below applies to it.
        if (coding == http_content_coding::identity) {
            continue;
        }
        auto stage = encode_http_content(coding, source_value, options);
        auto* content = std::get_if<std::pmr::string>(&stage);
        if (content == nullptr) {
            return std::get<http_content_encode_error>(stage);
        }
        current = std::move(*content);
        source_value = current;
        encoded = true;
    }
    if (!encoded) {
        if (input.size() > options.max_encoded_bytes_) {
            return http_content_encode_error::encoded_size_exceeded;
        }
        current.assign(input);
    }
    return current;
}

std::variant<std::pmr::string, http_content_decode_error> decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options) {
    auto* const resource = detail::http_pmr_resource_or_default(options.resource_);
    std::pmr::string current(resource);
    std::string_view source_value = input;
    bool decoded = false;
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        // identity transforms nothing; only the size limit below applies to it.
        if (*coding == http_content_coding::identity) {
            continue;
        }
        auto stage = decode_http_content(*coding, source_value, options);
        auto* content = std::get_if<std::pmr::string>(&stage);
        if (content == nullptr) {
            return std::get<http_content_decode_error>(stage);
        }
        current = std::move(*content);
        source_value = current;
        decoded = true;
    }
    if (!decoded) {
        if (input.size() > options.max_decoded_bytes_) {
            return http_content_decode_error::decoded_size_exceeded;
        }
        current.assign(input);
    }
    return current;
}
}  // namespace ruvia
