#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "http_client_response_head.h"
#include "http_field_kind.h"
#include "http_field_text.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_body.h"
#include "http_response_headers.h"
#include "http_status.h"

namespace ruvia::detail {

struct http_request_access final {
    static constexpr std::size_t cached_header_slots = http_request::cached_header_slots;
    static_assert(cached_header_slots == http_field_kind_count - 1);

    [[nodiscard]] static http_request make() noexcept {
        return http_request();
    }

    [[nodiscard]] static constexpr std::size_t known_header_slot(http_field_kind name) noexcept {
        const auto slot = http_field_kind_known_slot(name);
        return slot < cached_header_slots ? slot : cached_header_slots;
    }

    [[nodiscard]] static std::string_view known_header(const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        if (slot >= cached_header_slots) {
            return {};
        }
        const auto index = request.cached_headers_[slot];
        return index != 0 && index <= request.headers_.size() ? request.headers_[index - 1].value() : std::string_view{};
    }

    [[nodiscard]] static bool has_known_header(const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.cached_headers_[slot] != 0 && request.cached_headers_[slot] <= request.headers_.size();
    }

    [[nodiscard]] static bool has_repeated_known_header(const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.headers_.has_repeated_known_slot(slot);
    }

    // Visits the value of every field line of known kind `name` in wire order and
    // returns false as soon as the visitor does. A non-repeated field is served
    // from the cache; repeated fields scan only up to the cached last line.
    template <typename visitor_type>
    static bool visit_known_headers(const http_request& request, http_field_kind name, visitor_type&& visitor) {
        const auto slot = known_header_slot(name);
        if (slot >= cached_header_slots) {
            return true;
        }
        const auto headers = request.headers_.fields();
        const std::size_t last_index = request.cached_headers_[slot];
        if (last_index == 0 || last_index > headers.size()) {
            return true;
        }
        if (!request.headers_.has_repeated_known_slot(slot)) {
            return visitor(headers[last_index - 1].value());
        }
        for (std::size_t i = 0; i < last_index; ++i) {
            if (request.headers_.kind_at(i) == name && !visitor(headers[i].value())) {
                return false;
            }
        }
        return true;
    }

    static void set_method(http_request& request, std::string_view method) noexcept {
        request.method_ = method;
        request.known_method_ = classify_http_method(method);
    }

    static void set_target(http_request& request, std::string_view target) noexcept {
        request.target_ = target;
    }

    static void set_scheme(http_request& request, std::string_view scheme) noexcept {
        request.scheme_ = scheme;
    }

    static void set_authority(http_request& request, std::string_view authority) noexcept {
        request.authority_ = authority;
    }

    static void set_protocol(http_request& request, std::string_view protocol) noexcept {
        request.protocol_ = protocol;
    }

    static void set_target_form(http_request& request, ::ruvia::http_request_target_form form) noexcept {
        request.target_form_ = form;
    }

    static void set_path(http_request& request, std::string_view path) noexcept {
        request.path_ = path;
    }

    static void set_query_string(http_request& request, std::string_view query_string) noexcept {
        request.query_string_ = query_string;
    }

    static void set_protocol_version(http_request& request, http_protocol_version protocol_version) noexcept {
        request.protocol_version_ = protocol_version;
    }

    // Call once after syntax validation, with the exact semantic field count.
    static void reserve_headers(http_request& request, std::size_t count) {
        if (count > (std::numeric_limits<std::uint16_t>::max)() || !request.headers_.empty()) {
            throw std::logic_error("invalid request header block reservation");
        }
        request.headers_.reserve(count);
    }

    static bool add_header(http_request& request, http_header_view header_value, http_field_kind kind) {
        if (request.headers_.size() == (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        const auto known_slot = known_header_slot(kind);
        request.headers_.append(header_value, known_slot < cached_header_slots ? kind : http_field_kind::other);
        if (known_slot < cached_header_slots) {
            if (request.cached_headers_[known_slot] != 0) {
                request.headers_.mark_repeated_known_slot(known_slot);
            }
            request.cached_headers_[known_slot] = static_cast<std::uint16_t>(request.headers_.size());
        }
        return true;
    }

    [[nodiscard]] static http_field_kind header_kind(const http_request& request, std::size_t index) noexcept {
        return request.headers_.kind_at(index);
    }

    static void set_body(http_request& request, std::span<const std::byte> body) noexcept {
        request.body_ = body;
    }
};

[[nodiscard]] constexpr http_response_content_semantics classify_http_response_content_semantics(http_known_method request_method, http_status_code status_code) noexcept {
    if (status_code == http_status::switching_protocols) {
        return http_response_content_semantics::protocol_switch;
    }
    if (status_code.is_informational()) {
        return http_response_content_semantics::informational;
    }
    if (request_method == http_known_method::connect && status_code.is_successful()) {
        return http_response_content_semantics::connect_tunnel;
    }
    if (request_method == http_known_method::head || status_code == http_status::no_content || status_code == http_status::not_modified) {
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

    [[nodiscard]] static const http_response_header* known_header(const http_response_headers& headers, std::uint32_t bit) noexcept {
        return headers.find_header_for_read({}, bit);
    }

    [[nodiscard]] static std::string_view known_header_value(const http_response_headers& headers, std::uint32_t bit) noexcept {
        const auto* const header_value = known_header(headers, bit);
        return header_value == nullptr ? std::string_view{} : header_value->value();
    }
};

struct http_response_header_access final {
    [[nodiscard]] static constexpr http_response_header make(const char* bytes_value, std::uint32_t name_size, std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
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

[[nodiscard]] inline bool response_has_known_header(const http_response_headers& headers, std::uint32_t bit) noexcept {
    return (http_response_header_state_access::known_bits(headers) & bit) != 0;
}

[[nodiscard]] inline bool response_has_known_header(const http_response& response, std::uint32_t bit) noexcept {
    return response_has_known_header(response.headers(), bit);
}

// http_response_header stores both lengths in uint32_t and releases the one
// name/value allocation using their sum. Never let a public or runtime helper
// construct a descriptor whose wire/storage lengths cannot be represented;
// truncating either field would make the response header view disagree with
// the allocation and could turn a large application value into malformed wire
// bytes or an out-of-bounds read during emission.
[[nodiscard]] inline constexpr bool response_header_storage_size_fits(std::size_t name_size, std::size_t value_size) noexcept {
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
[[nodiscard]] inline bool response_header_matches(const http_response_header& header_value, std::string_view key, std::uint32_t known_bit) noexcept {
    const auto header_known_bit = http_response_header_access::known_bit(header_value);
    return known_bit != 0 ? header_known_bit == known_bit : header_known_bit == 0 && http_ascii_equals_ignore_case(header_value.name(), key);
}

[[nodiscard]] inline bool response_header_storage_overlaps(const http_response_header& header_value, std::string_view value) noexcept {
    const auto name = header_value.name();
    if (name.data() == nullptr) {
        return false;
    }
    return http_text_overlaps({name.data(), name.size() + header_value.value().size()}, value);
}

[[nodiscard]] inline bool response_header_views_contiguous(std::string_view name, std::string_view value) noexcept {
    if (name.data() == nullptr) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    if (value.data() == nullptr) {
        return false;
    }
    return reinterpret_cast<std::uintptr_t>(value.data()) == reinterpret_cast<std::uintptr_t>(name.data() + name.size());
}

struct http_client_response_head_access final {
    [[nodiscard]] static http_client_response_head make(http_received_status status, http_protocol_version protocol_version) noexcept {
        return http_client_response_head(status, protocol_version);
    }

    [[nodiscard]] static std::vector<http_header>& headers(http_client_response_head& head) noexcept {
        return head.headers_;
    }
    static void content_length(http_client_response_head& head, std::optional<std::uint64_t> value) noexcept {
        head.content_length_ = value;
    }
};

}  // namespace ruvia::detail
