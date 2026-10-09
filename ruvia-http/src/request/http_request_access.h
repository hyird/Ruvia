#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"

namespace ruvia::detail {

struct http_request_access final {
    static constexpr std::size_t cached_header_slots = http_request::cached_header_slots;

    [[nodiscard]] static http_request make() noexcept {
        return http_request();
    }

    [[nodiscard]] static constexpr std::size_t known_header_slot(request_header_kind name) noexcept {
        const auto slot = request_header_kind_known_slot(name);
        return slot < cached_header_slots ? slot : cached_header_slots;
    }

    [[nodiscard]] static std::string_view known_header(
        const http_request& request, request_header_kind name) noexcept {
        const auto slot = known_header_slot(name);
        if (slot >= cached_header_slots) {
            return {};
        }
        const auto index = request.cached_headers_[slot];
        return index != 0 && index <= request.headers_.size()
                   ? request.headers_[index - 1].value()
                   : std::string_view{};
    }

    [[nodiscard]] static bool has_known_header(
        const http_request& request, request_header_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.cached_headers_[slot] != 0 &&
               request.cached_headers_[slot] <= request.headers_.size();
    }

    [[nodiscard]] static std::span<const std::byte> body_bytes(const http_request& request) noexcept {
        return request.body_;
    }

    static void reset(http_request& request) noexcept {
        request = make();
    }

    static void set_resource(http_request& request, std::pmr::memory_resource* resource) noexcept {
        request.resource_ = resource;
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

    static void set_target_form(http_request& request, ::ruvia::http_request_target_form form) noexcept {
        request.target_form_ = form;
    }

    static void set_path(http_request& request, std::string_view path) noexcept {
        request.path_ = path;
    }

    static void set_query_string(http_request& request, std::string_view query_string) noexcept {
        request.query_string_ = query_string;
    }

    static void set_protocol_version(
        http_request& request, http_protocol_version protocol_version) noexcept {
        request.protocol_version_ = protocol_version;
    }

    // Call once after syntax validation, with the exact semantic field count.
    // Binding happens before allocation; changing the auxiliary request resource
    // later does not change ownership of an existing descriptor block.
    static void reserve_headers(http_request& request, std::size_t count) {
        if (count > max_http_header_fields || !request.headers_.empty()) {
            throw std::logic_error("invalid request header block reservation");
        }
        request.headers_.reserve(count, request.resource());
    }

    static bool add_header(http_request& request, http_header_view header_value) {
        return add_header(
            request, header_value, request_header_kind_known_slot(classify_request_header(header_value.name())));
    }

    static bool add_header(
        http_request& request, http_header_view header_value, std::size_t known_slot) {
        if (request.headers_.size() == max_http_header_fields) {
            return false;
        }
        const auto kind = known_slot < cached_header_slots
                              ? static_cast<std::uint8_t>(known_slot + 1)
                              : std::uint8_t{0};
        request.headers_.append(header_value, kind);
        if (known_slot < cached_header_slots) {
            request.cached_headers_[known_slot] = static_cast<std::uint8_t>(request.headers_.size());
        }
        return true;
    }

    [[nodiscard]] static std::uint8_t header_kind(
        const http_request& request, std::size_t index) noexcept {
        return request.headers_.kind_at(index);
    }

    static void set_body(http_request& request, std::span<const std::byte> body) noexcept {
        request.body_ = body;
    }
    static void set_body(http_request& request, std::string_view body) noexcept {
        set_body(request, std::as_bytes(std::span<const char>(body.data(), body.size())));
    }
};

[[nodiscard]] inline std::string_view request_known_header(
    const http_request& request, request_header_kind name) noexcept {
    return http_request_access::known_header(request, name);
}

[[nodiscard]] inline bool request_has_known_header(
    const http_request& request, request_header_kind name) noexcept {
    return http_request_access::has_known_header(request, name);
}

[[nodiscard]] inline std::span<const std::byte> request_body_bytes(const http_request& request) noexcept {
    return http_request_access::body_bytes(request);
}

}  // namespace ruvia::detail
