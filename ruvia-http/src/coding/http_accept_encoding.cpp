#include "ruvia/http/http_accept_encoding.h"

#include <algorithm>
#include <initializer_list>

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/http_content_coding.h"

#include "field/http_quality_value.h"

namespace ruvia {

namespace {

[[nodiscard]] bool is_compress_coding_token(std::string_view token) noexcept {
    return http_ascii_equals_ignore_case(token, "compress") ||
           http_ascii_equals_ignore_case(token, "x-compress");
}

[[nodiscard]] int& explicit_quality(http_response_coding_qualities& qualities, http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::gzip:
            return qualities.gzip_.explicit_quality_;
        case http_content_coding::deflate:
            return qualities.deflate_.explicit_quality_;
        case http_content_coding::brotli:
            return qualities.brotli_.explicit_quality_;
        case http_content_coding::zstd:
            return qualities.zstd_.explicit_quality_;
        case http_content_coding::identity:
            break;
    }
    return qualities.identity_.explicit_quality_;
}

// Hands every nonempty Accept-Encoding member's coding token and weight to
// `visitor`. A repeated coding keeps its highest weight.
template <typename visitor_type>
void visit_accept_encoding_members(std::string_view accept_encoding, visitor_type&& visitor) noexcept {
    detail::http_visit_comma_separated_quoted(accept_encoding, [&visitor](std::string_view item) noexcept {
        visitor(detail::http_header_token_before_parameters(item), detail::http_weight_parameter(item));
        return true;
    });
}

}  // namespace

void http_accepted_encoding_quality::update(
    std::string_view accept_encoding, std::string_view coding) noexcept {
    const bool gzip_coding = http_is_gzip_coding_token(coding);
    const bool compress_coding = is_compress_coding_token(coding);
    visit_accept_encoding_members(accept_encoding,
        [coding, gzip_coding, compress_coding, this](std::string_view token, int weight) noexcept {
            const bool explicit_match = gzip_coding       ? http_is_gzip_coding_token(token)
                                        : compress_coding ? is_compress_coding_token(token)
                                                          : http_ascii_equals_ignore_case(token, coding);
            if (explicit_match) {
                explicit_quality_ = std::max(explicit_quality_, weight);
            } else if (token == "*") {
                wildcard_quality_ = std::max(wildcard_quality_, weight);
            }
        });
}

bool http_accepts_encoding(std::string_view accept_encoding, std::string_view coding) noexcept {
    if (accept_encoding.empty()) {
        return http_ascii_equals_ignore_case(coding, "identity");
    }
    http_accepted_encoding_quality quality;
    quality.update(accept_encoding, coding);
    return quality.accepts(http_ascii_equals_ignore_case(coding, "identity"));
}

void http_response_coding_qualities::update(std::string_view accept_encoding) noexcept {
    field_present_ = true;
    visit_accept_encoding_members(accept_encoding, [this](std::string_view token, int weight) noexcept {
        has_non_empty_item_ = true;
        if (const auto coding = http_content_coding_from_token(token)) {
            auto& quality = explicit_quality(*this, *coding);
            quality = std::max(quality, weight);
        } else if (token == "*") {
            for (auto* coding_quality : {&gzip_, &deflate_, &brotli_, &zstd_, &identity_}) {
                coding_quality->wildcard_quality_ = std::max(coding_quality->wildcard_quality_, weight);
            }
        }
    });
}

}  // namespace ruvia
