#include <cstddef>
#include <string_view>

#include "ruvia/http/url_encoding.h"

#include "parser/http_request_target.h"
#include "parser/http_uri_grammar.h"

namespace ruvia {
namespace {

constexpr auto npos = std::string_view::npos;

// Positions whose literal legality depends on the reference's own structure.
struct uri_reference_layout final {
    // The only '[' and ']' that delimit the authority's IP literal.
    std::size_t ip_literal_open_{npos};
    std::size_t ip_literal_close_{npos};
    // Authority bytes [authority_begin_, userinfo_end_) are userinfo, which
    // cannot carry '@' literally (RFC 3986 section 3.2.1); userinfo_end_ is
    // the delimiting '@'.
    std::size_t authority_begin_{npos};
    std::size_t userinfo_end_{npos};
    // Bytes before this index form the colon-free first segment of a
    // relative-path reference; zero when the reference has a scheme or an
    // authority, or starts with a path, query, or fragment delimiter.
    std::size_t colon_free_end_{0};
};

[[nodiscard]] uri_reference_layout find_uri_reference_layout(std::string_view reference) noexcept {
    uri_reference_layout layout;
    const auto first_delimiter = reference.find_first_of("/?#");
    const auto colon = reference.find(':');
    std::size_t authority_begin = npos;
    if (colon < first_delimiter && detail::is_valid_uri_scheme(reference.substr(0, colon))) {
        if (reference.substr(colon + 1).starts_with("//")) {
            authority_begin = colon + 3;
        }
    } else {
        layout.colon_free_end_ = first_delimiter == npos ? reference.size() : first_delimiter;
        if (reference.starts_with("//")) {
            authority_begin = 2;
        }
    }
    if (authority_begin == npos) {
        return layout;
    }

    const auto authority_end_index = reference.find_first_of("/?#", authority_begin);
    const auto authority_end = authority_end_index == npos ? reference.size() : authority_end_index;
    const auto authority = reference.substr(authority_begin, authority_end - authority_begin);
    const auto userinfo_end = authority.rfind('@');
    if (userinfo_end != npos) {
        layout.authority_begin_ = authority_begin;
        layout.userinfo_end_ = authority_begin + userinfo_end;
    }
    const auto host_begin = authority_begin + (userinfo_end == npos ? 0 : userinfo_end + 1);
    if (host_begin >= authority_end || reference[host_begin] != '[') {
        return layout;
    }
    const auto close = reference.find(']', host_begin + 1);
    if (close < authority_end) {
        layout.ip_literal_open_ = host_begin;
        layout.ip_literal_close_ = close;
    }
    return layout;
}

[[nodiscard]] bool uri_reference_keeps_literal(
    unsigned char byte, std::size_t index, const uri_reference_layout& layout, bool& fragment_seen) noexcept {
    switch (byte) {
        case '[':
        case ']':
            return index == layout.ip_literal_open_ || index == layout.ip_literal_close_;
        case ':':
            return index >= layout.colon_free_end_;
        case '#': {
            const bool keeps_separator = !fragment_seen;
            fragment_seen = true;
            return keeps_separator;
        }
        case '@':
            return index < layout.authority_begin_ || index >= layout.userinfo_end_;
        case '/':
        case '?':
            return true;
        default:
            return detail::is_uri_pchar(byte);
    }
}

[[nodiscard]] std::size_t encoded_uri_reference_size(std::string_view reference,
    std::size_t first_encoded_index, const uri_reference_layout& layout, bool fragment_seen) noexcept {
    std::size_t total = reference.size();
    for (std::size_t i = first_encoded_index; i < reference.size(); ++i) {
        const auto byte = static_cast<unsigned char>(reference[i]);
        const bool escape = byte == '%' && http_try_decode_percent_byte(reference, i).has_value();
        if (escape || (byte != '%' && uri_reference_keeps_literal(byte, i, layout, fragment_seen))) {
            if (escape) {
                i += 2;
            }
            continue;
        }
        total += 2;
    }
    return total;
}
}  // namespace

std::optional<std::pmr::string> percent_encode_uri_reference(
    std::string_view reference, std::pmr::memory_resource* resource) {
    const auto layout = find_uri_reference_layout(reference);
    bool fragment_seen = false;
    std::optional<std::pmr::string> encoded;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto byte = static_cast<unsigned char>(reference[i]);
        // A lone or malformed '%' is not an escape and is encoded like any
        // other octet the reference cannot carry literally.
        const bool escape = byte == '%' && http_try_decode_percent_byte(reference, i).has_value();
        if (escape || (byte != '%' && uri_reference_keeps_literal(byte, i, layout, fragment_seen))) {
            const std::size_t width = escape ? 3 : 1;
            if (encoded.has_value()) {
                encoded->append(reference.substr(i, width));
            }
            i += width - 1;
            continue;
        }
        if (!encoded.has_value()) {
            const auto total_size =
                encoded_uri_reference_size(reference, i, layout, fragment_seen);
            encoded.emplace(detail::http_pmr_resource_or_default(resource));
            encoded->reserve(total_size);
            encoded->append(reference.substr(0, i));
        }
        http_append_percent_byte(*encoded, byte);
    }
    return encoded;
}

}  // namespace ruvia
