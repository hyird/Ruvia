#include "coding/http_content_coding.h"

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_response.h"

#include "coding/http_content_codec.h"

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

}  // namespace ruvia

namespace ruvia::detail {

void http_content_coding_field_parser::update(std::string_view value) {
    if (invalid_) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = http_trim_ows(value.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (token.empty()) {
            if (role_ == http_field_list_role::sender) {
                invalid_ = true;
                return;
            }
        } else if (!is_valid_http_header_name(token)) {
            invalid_ = true;
            return;
        } else {
            http_content_coding coding = http_content_coding::identity;
            if (http_ascii_equals_ignore_case(token, "identity")) {
                coding = http_content_coding::identity;
            } else if (http_is_gzip_coding_token(token)) {
                coding = http_content_coding::gzip;
            } else if (http_ascii_equals_ignore_case(token, "deflate")) {
                coding = http_content_coding::deflate;
            } else if (http_ascii_equals_ignore_case(token, "br")) {
                coding = http_content_coding::brotli;
            } else if (http_ascii_equals_ignore_case(token, "zstd")) {
                coding = http_content_coding::zstd;
            } else {
                unsupported_ = true;
                if (comma == std::string_view::npos) {
                    break;
                }
                begin = comma + 1;
                continue;
            }
            codings_.push_back(coding);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
}

http_content_coding_field_result http_content_coding_field_parser::finish() && {
    if (invalid_) {
        return http_content_coding_field_result_access::invalid();
    }
    if (unsupported_) {
        return http_content_coding_field_result_access::unsupported();
    }
    return http_content_coding_field_result_access::coding(std::move(codings_));
}

bool is_valid_http_content_encoding_field_value(std::string_view value, http_field_list_role role) noexcept {
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = http_trim_ows(value.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (token.empty() && role == http_field_list_role::sender) {
            return false;
        }
        if (!token.empty() && !is_valid_http_header_name(token)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        begin = comma + 1;
    }
    return true;
}

}  // namespace ruvia::detail

namespace ruvia {

http_content_coding_field_result parse_http_content_coding(
    std::string_view value, std::pmr::memory_resource* resource) {
    detail::http_content_coding_field_parser parser(detail::http_field_list_role::recipient, resource);
    parser.update(value);
    return std::move(parser).finish();
}

http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers, std::pmr::memory_resource* resource) {
    return detail::http_content_coding_from_headers(headers, resource);
}

http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers, std::pmr::memory_resource* resource) {
    return detail::http_content_coding_from_headers(headers, resource);
}

http_content_decode_result decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options) {
    if (input.empty() && !http_content_coding_token(coding).empty()) {
        // A zero-length coded representation carries no content: user agents
        // treat it as empty rather than as a truncated coding stream.
        return detail::http_content_decode_result_access::decoded(
            std::pmr::string(detail::http_pmr_resource_or_default(options.resource_)));
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
                return detail::http_content_decode_result_access::failure(
                    http_content_decode_error::decoded_size_exceeded);
            }
            return detail::http_content_decode_result_access::decoded(std::pmr::string(
                input, detail::http_pmr_resource_or_default(options.resource_)));
    }
    return detail::http_content_decode_result_access::failure(
        http_content_decode_error::unsupported_coding);
}

http_content_encode_result encode_http_content(
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
                return detail::http_content_encode_result_access::failure(
                    http_content_encode_error::encoded_size_exceeded);
            }
            return detail::http_content_encode_result_access::encoded(std::pmr::string(
                input, detail::http_pmr_resource_or_default(options.resource_)));
    }
    return detail::http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
}

http_content_encode_result encode_http_content(std::span<const http_content_coding> codings,
    std::string_view input, http_content_encode_options options) {
    auto* const resource = detail::http_pmr_resource_or_default(options.resource_);
    if (codings.empty()) {
        if (input.size() > options.max_encoded_bytes_) {
            return detail::http_content_encode_result_access::failure(
                http_content_encode_error::encoded_size_exceeded);
        }
        return detail::http_content_encode_result_access::encoded(std::pmr::string(input, resource));
    }

    std::pmr::string current(resource);
    std::string_view source_value = input;
    for (const auto coding : codings) {
        auto encoded = encode_http_content(coding, source_value, options);
        auto* content = encoded.encoded();
        if (content == nullptr) {
            return detail::http_content_encode_result_access::failure(encoded.failure()->error());
        }
        current = std::move(*content).take_bytes();
        source_value = current;
    }
    return detail::http_content_encode_result_access::encoded(std::move(current));
}

http_content_decode_result decode_http_content(std::span<const http_content_coding> codings,
    std::string_view input, http_content_decode_options options) {
    auto* const resource = detail::http_pmr_resource_or_default(options.resource_);
    if (codings.empty()) {
        if (input.size() > options.max_decoded_bytes_) {
            return detail::http_content_decode_result_access::failure(
                http_content_decode_error::decoded_size_exceeded);
        }
        return detail::http_content_decode_result_access::decoded(std::pmr::string(input, resource));
    }

    std::pmr::string current(resource);
    std::string_view source_value = input;
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        auto decoded = decode_http_content(*coding, source_value, options);
        auto* content = decoded.decoded();
        if (content == nullptr) {
            return detail::http_content_decode_result_access::failure(decoded.failure()->error());
        }
        current = std::move(*content).take_bytes();
        source_value = current;
    }
    return detail::http_content_decode_result_access::decoded(std::move(current));
}

}  // namespace ruvia
