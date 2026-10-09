#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"
#include "util/http_number_format.h"
// The two response fields Ruvia formats itself rather than taking as a value:
// Allow, built from the method mask a route matched, and Content-Range in both
// its satisfied and unsatisfied forms. Each writes straight into the response's
// own header storage, sized before it is filled.

namespace ruvia {
namespace {

inline constexpr std::size_t allow_header_method_slots = static_cast<std::size_t>(http_known_method::unknown);

void append_header_value_literal(char*& cursor_value, std::string_view value) noexcept {
    std::memcpy(cursor_value, value.data(), value.size());
    cursor_value += value.size();
}

void append_header_value_unsigned(char*& cursor_value, char* end, std::uint64_t value) {
    const auto [ptr, ec] = std::to_chars(cursor_value, end, value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format HTTP response header value");
    }
    cursor_value = ptr;
}

void write_content_range_header_value(
    http_response_header& header_value, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }

    auto* cursor_value = detail::response_header_value_begin(header_value);
    auto* const end = detail::response_header_value_end(header_value);
    append_header_value_literal(cursor_value, "bytes ");
    append_header_value_unsigned(cursor_value, end, offset);
    *cursor_value++ = '-';
    append_header_value_unsigned(cursor_value, end, offset + length - 1);
    *cursor_value++ = '/';
    append_header_value_unsigned(cursor_value, end, size);
    if (cursor_value != end) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

void write_content_range_unsatisfied_header_value(http_response_header& header_value, std::uint64_t size) {
    auto* cursor_value = detail::response_header_value_begin(header_value);
    auto* const end = detail::response_header_value_end(header_value);
    append_header_value_literal(cursor_value, "bytes */");
    append_header_value_unsigned(cursor_value, end, size);
    if (cursor_value != end) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

// The mask names the methods this library classifies; extension_methods carries
// the exact wire tokens of any others the resource supports. RFC 9110 10.2.1
// requires Allow to list every supported method, and an extension method has no
// bit to occupy, so it has to travel alongside the mask rather than inside it.
[[nodiscard]] std::size_t allow_header_value_size(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    std::size_t size = 0;
    std::size_t count = 0;
    const auto add_method = [&](std::size_t token_size) {
        const auto separator_size = count == 0 ? std::size_t{0} : std::size_t{2};
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, separator_size);
        size += separator_size;
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, token_size);
        size += token_size;
        ++count;
    };
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        add_method(known_http_method_token(static_cast<http_known_method>(i)).size());
    }
    for (const auto token : extension_methods) {
        add_method(token.size());
    }
    return size;
}

void write_allow_header_value(http_response_header& header_value, std::uint32_t method_mask,
    std::span<const std::string_view> extension_methods) {
    auto* cursor_value = detail::response_header_value_begin(header_value);
    auto* const end = detail::response_header_value_end(header_value);
    bool first = true;
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            append_header_value_literal(cursor_value, ", ");
        }
        first = false;
        append_header_value_literal(cursor_value, known_http_method_token(static_cast<http_known_method>(i)));
    }
    for (const auto token : extension_methods) {
        if (!first) {
            append_header_value_literal(cursor_value, ", ");
        }
        first = false;
        append_header_value_literal(cursor_value, token);
    }
    if (cursor_value != end) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

}  // namespace

void http_response::allow_methods(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    const auto value_size = allow_header_value_size(method_mask, extension_methods);
    for (const auto token : extension_methods) {
        if (!is_valid_http_method_token(token)) {
            throw std::invalid_argument("invalid HTTP Allow method");
        }
    }
    if (!extension_methods.empty()) {
        if (auto* const retained = find_header_for_update("Allow", detail::response_header_allow)) {
            bool borrows_allow = false;
            for (const auto token : extension_methods) {
                if (detail::response_header_storage_overlaps(*retained, token)) {
                    borrows_allow = true;
                    break;
                }
            }
            if (borrows_allow) {
                // Replacing or collapsing Allow can retire any of its input views.
                // Format the replacement before publishing it or releasing old bytes.
                auto prepared = headers_.make_uninitialized_header(
                    "Allow", value_size, detail::response_header_allow);
                try {
                    write_allow_header_value(prepared, method_mask, extension_methods);
                } catch (...) {
                    headers_.release_header(prepared);
                    throw;
                }
                const bool was_appended = detail::response_header_append(*retained);
                headers_.release_header(*retained);
                *retained = prepared;
                if (was_appended) {
                    (void)collapse_response_headers(*retained, detail::response_header_allow);
                }
                return;
            }
        }
    }
    auto& header_value = prepare_header_value_storage(
        "Allow", value_size, detail::response_header_allow);
    write_allow_header_value(header_value, method_mask, extension_methods);
}

void http_response::set_content_range(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto end_offset = offset + length - 1;
    const auto value_size =
        std::string_view("bytes ").size() + detail::http_unsigned_decimal_size(offset) + 1 +
        detail::http_unsigned_decimal_size(end_offset) + 1 + detail::http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_header_value(header_value, offset, length, size);
}

void http_response::set_content_range_unsatisfied(std::uint64_t size) {
    const auto value_size =
        std::string_view("bytes */").size() + detail::http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_unsatisfied_header_value(header_value, size);
}

}  // namespace ruvia
