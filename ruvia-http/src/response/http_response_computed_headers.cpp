#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"
#include "util/http_wire_sink.h"
// The two response fields Ruvia formats itself rather than taking as a value:
// Allow, built from the method mask a route matched, and Content-Range in both
// its satisfied and unsatisfied forms. Each writes straight into the response's
// own header storage, sized before it is filled.

namespace ruvia {
namespace {

inline constexpr std::size_t allow_header_method_slots = static_cast<std::size_t>(http_known_method::unknown);

void write_content_range_header_value(
    http_response_header& header_value, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }

    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes ");
    sink.append_unsigned(offset);
    sink.append('-');
    sink.append_unsigned(offset + length - 1);
    sink.append('/');
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

void write_content_range_unsatisfied_header_value(http_response_header& header_value, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes */");
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
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
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    bool first = true;
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(known_http_method_token(static_cast<http_known_method>(i)));
    }
    for (const auto token : extension_methods) {
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(token);
    }
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

}  // namespace

void http_response_headers::allow_methods(
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
                auto prepared = make_uninitialized_header(
                    "Allow", value_size, detail::response_header_allow);
                try {
                    write_allow_header_value(prepared, method_mask, extension_methods);
                } catch (...) {
                    release_header(prepared);
                    throw;
                }
                const bool was_appended = detail::http_response_header_access::append(*retained);
                release_header(*retained);
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

void http_response_headers::set_content_range(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto end_offset = offset + length - 1;
    const auto value_size =
        std::string_view("bytes ").size() + http_unsigned_decimal_size(offset) + 1 +
        http_unsigned_decimal_size(end_offset) + 1 + http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_header_value(header_value, offset, length, size);
}

void http_response_headers::set_content_range_unsatisfied(std::uint64_t size) {
    const auto value_size =
        std::string_view("bytes */").size() + http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_unsatisfied_header_value(header_value, size);
}

}  // namespace ruvia
