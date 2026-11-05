#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>

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

    [[nodiscard]] static bool has_repeated_known_header(
        const http_request& request, request_header_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.headers_.has_repeated_known_slot(slot);
    }

    // Visits the value of every field line of known kind `name` in wire order and
    // returns false as soon as the visitor does. A non-repeated field is served
    // from the cache; repeated fields scan only up to the cached last line.
    template <typename visitor_type>
    static bool visit_known_headers(
        const http_request& request, request_header_kind name, visitor_type&& visitor) {
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

    static void set_protocol_version(
        http_request& request, http_protocol_version protocol_version) noexcept {
        request.protocol_version_ = protocol_version;
    }

    // Call once after syntax validation, with the exact semantic field count.
    // Binding happens before allocation; changing the auxiliary request resource
    // later does not change ownership of an existing descriptor block.
    static void reserve_headers(http_request& request, std::size_t count) {
        if (count > UINT16_MAX || !request.headers_.empty()) {
            throw std::logic_error("invalid request header block reservation");
        }
        request.headers_.reserve(count, request.resource());
    }

    static bool add_header(
        http_request& request, http_header_view header_value, request_header_kind kind) {
        if (request.headers_.size() == UINT16_MAX) {
            return false;
        }
        const auto known_slot = known_header_slot(kind);
        request.headers_.append(
            header_value, known_slot < cached_header_slots ? kind : request_header_kind::other);
        if (known_slot < cached_header_slots) {
            if (request.cached_headers_[known_slot] != 0) {
                request.headers_.mark_repeated_known_slot(known_slot);
            }
            request.cached_headers_[known_slot] = static_cast<std::uint16_t>(request.headers_.size());
        }
        return true;
    }

    [[nodiscard]] static request_header_kind header_kind(
        const http_request& request, std::size_t index) noexcept {
        return request.headers_.kind_at(index);
    }

    static void set_body(http_request& request, std::span<const std::byte> body) noexcept {
        request.body_ = body;
    }
};

}  // namespace ruvia::detail
