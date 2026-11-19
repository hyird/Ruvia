#include "url_encoding.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "http_field_text.h"
#include "http_uri_syntax.h"

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

[[nodiscard]] bool uri_reference_keeps_literal(unsigned char byte, std::size_t index, const uri_reference_layout& layout, bool& fragment_seen) noexcept {
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

template <typename literal_width_type>
[[nodiscard]] std::string_view percent_encode_text(std::string_view input, std::string& storage, literal_width_type literal_width) {
    const bool initially_empty = storage.empty();
    std::string staged;
    std::string* encoded = nullptr;
    try {
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto width = literal_width(i);
            if (width != 0) {
                if (encoded != nullptr) {
                    if (width > encoded->max_size() - encoded->size()) {
                        throw std::length_error("percent-encoded text exceeds string capacity");
                    }
                    detail::reserve_geometric(*encoded, encoded->size() + width);
                    encoded->append(input.substr(i, width));
                }
                i += width - 1;
                continue;
            }
            if (encoded == nullptr) {
                // Preserve nonempty storage through every throwing path.
                // Aliased input always needs independent storage.
                const bool can_reuse = (initially_empty || input.size() <= storage.capacity() / 3) && !detail::http_text_aliases_string(input, storage);
                encoded = can_reuse ? &storage : &staged;
                if (input.size() > encoded->max_size() - 2) {
                    throw std::length_error("percent-encoded text exceeds string capacity");
                }
                detail::reserve_geometric(*encoded, input.size() + 2);
                encoded->clear();
                encoded->append(input.substr(0, i));
            }
            if (encoded->size() > encoded->max_size() - 3) {
                throw std::length_error("percent-encoded text exceeds string capacity");
            }
            detail::reserve_geometric(*encoded, encoded->size() + 3);
            append_percent_byte(*encoded, static_cast<unsigned char>(input[i]));
        }
    } catch (...) {
        if (initially_empty && encoded == &storage) {
            storage.clear();
        }
        throw;
    }
    if (encoded == nullptr) {
        return input;
    }
    if (encoded == &staged) {
        storage = std::move(staged);
    }
    return storage;
}

}  // namespace

std::string_view percent_encode_uri_reference(borrowed_text text, std::string& storage) {
    const auto reference = text.view();
    const auto layout = find_uri_reference_layout(reference);
    bool fragment_seen = false;
    return percent_encode_text(reference, storage, [&](std::size_t i) noexcept -> std::size_t {
        const auto byte = static_cast<unsigned char>(reference[i]);
        if (byte == '%') {
            return decode_percent_byte(reference, i) ? 3 : 0;
        }
        return uri_reference_keeps_literal(byte, i, layout, fragment_seen) ? 1 : 0;
    });
}

std::string_view percent_encode_url_component(borrowed_text text, std::string& storage) {
    const auto input = text.view();
    return percent_encode_text(input, storage, [input](std::size_t i) noexcept -> std::size_t {
        return is_uri_unreserved_byte(static_cast<unsigned char>(input[i])) ? 1 : 0;
    });
}

std::optional<std::string_view> decode_url_component(borrowed_text text, std::string& storage, url_decode_mode mode) {
    const auto input = text.view();
    // The literal prefix before the first byte decoding transforms or rejects
    // is copied, not decoded; without such a byte the input is the result.
    std::size_t first = 0;
    for (; first < input.size(); ++first) {
        const char c = input[first];
        if ((c == '+' && (mode == url_decode_mode::form || mode == url_decode_mode::strict_form)) || (c == '%' && (mode != url_decode_mode::form || decode_percent_byte(input, first)))) {
            break;
        }
    }
    if (first == input.size()) {
        return input;
    }
    // A rejecting mode can reuse empty storage only if it need not grow.
    const bool can_write_in_place = (mode == url_decode_mode::form || (storage.empty() && input.size() <= storage.capacity())) && !detail::http_text_aliases_string(input, storage);
    std::string staged;
    auto& target = can_write_in_place ? storage : staged;
    target.resize(input.size());
    input.copy(target.data(), first);
    const auto size = decode_url_bytes(input.substr(first), target.data() + first, mode);
    if (!size) {
        if (can_write_in_place) {
            storage.clear();
        }
        return std::nullopt;
    }
    target.resize(first + *size);
    if (&target == &staged) {
        storage = std::move(staged);
    }
    return std::string_view(storage);
}

}  // namespace ruvia
