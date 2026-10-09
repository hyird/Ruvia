#include "ruvia/http/http_accept_encoding.h"

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/http_content_coding.h"

#include "field/http_quality_value.h"

namespace ruvia {

namespace {

[[nodiscard]] bool is_compress_coding_token(std::string_view token) noexcept {
    return detail::http_ascii_equals_ignore_case(token, "compress") ||
           detail::http_ascii_equals_ignore_case(token, "x-compress");
}

}  // namespace

void http_accepted_encoding_quality::update(
    std::string_view accept_encoding, std::string_view coding) noexcept {
    const bool gzip_coding = http_is_gzip_coding_token(coding);
    const bool compress_coding = is_compress_coding_token(coding);
    detail::http_visit_comma_separated_quoted(accept_encoding,
        [coding, gzip_coding, compress_coding, this](std::string_view item) noexcept {
            const auto token = detail::http_header_token_before_parameters(item);
            const bool explicit_match = gzip_coding       ? http_is_gzip_coding_token(token)
                                        : compress_coding ? is_compress_coding_token(token)
                                                          : detail::http_ascii_equals_ignore_case(token, coding);
            if (explicit_match) {
                detail::http_accumulate_accepted_quality(
                    detail::http_weight_parameter(item), explicit_quality_);
            } else if (token == "*") {
                detail::http_accumulate_accepted_quality(
                    detail::http_weight_parameter(item), wildcard_quality_);
            }
            return true;
        });
}

bool http_accepts_encoding(std::string_view accept_encoding, std::string_view coding) noexcept {
    if (accept_encoding.empty()) {
        return detail::http_ascii_equals_ignore_case(coding, "identity");
    }
    http_accepted_encoding_quality quality;
    quality.update(accept_encoding, coding);
    return quality.accepts(detail::http_ascii_equals_ignore_case(coding, "identity"));
}

void http_response_coding_qualities::update(std::string_view accept_encoding) noexcept {
    field_present_ = true;
    detail::http_visit_comma_separated_quoted(accept_encoding, [this](std::string_view item) noexcept {
        const auto token = detail::http_header_token_before_parameters(item);
        has_non_empty_item_ = true;
        if (http_is_gzip_coding_token(token)) {
            detail::http_accumulate_accepted_quality(
                detail::http_weight_parameter(item), gzip_.explicit_quality_);
        } else if (detail::http_ascii_equals_ignore_case(token, "deflate")) {
            detail::http_accumulate_accepted_quality(
                detail::http_weight_parameter(item), deflate_.explicit_quality_);
        } else if (detail::http_ascii_equals_ignore_case(token, "br")) {
            detail::http_accumulate_accepted_quality(
                detail::http_weight_parameter(item), brotli_.explicit_quality_);
        } else if (detail::http_ascii_equals_ignore_case(token, "zstd")) {
            detail::http_accumulate_accepted_quality(
                detail::http_weight_parameter(item), zstd_.explicit_quality_);
        } else if (detail::http_ascii_equals_ignore_case(token, "identity")) {
            detail::http_accumulate_accepted_quality(
                detail::http_weight_parameter(item), identity_.explicit_quality_);
        } else if (token == "*") {
            const auto wildcard = detail::http_weight_parameter(item);
            detail::http_accumulate_accepted_quality(wildcard, gzip_.wildcard_quality_);
            detail::http_accumulate_accepted_quality(wildcard, deflate_.wildcard_quality_);
            detail::http_accumulate_accepted_quality(wildcard, brotli_.wildcard_quality_);
            detail::http_accumulate_accepted_quality(wildcard, zstd_.wildcard_quality_);
            detail::http_accumulate_accepted_quality(wildcard, identity_.wildcard_quality_);
        }
        return true;
    });
}

}  // namespace ruvia
