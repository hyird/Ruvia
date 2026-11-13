#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"

#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"

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

    [[nodiscard]] static std::string_view known_header(
        const http_request& request, http_field_kind name) noexcept {
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
        const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.cached_headers_[slot] != 0 &&
               request.cached_headers_[slot] <= request.headers_.size();
    }

    [[nodiscard]] static bool has_repeated_known_header(
        const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.headers_.has_repeated_known_slot(slot);
    }

    // Visits the value of every field line of known kind `name` in wire order and
    // returns false as soon as the visitor does. A non-repeated field is served
    // from the cache; repeated fields scan only up to the cached last line.
    template <typename visitor_type>
    static bool visit_known_headers(
        const http_request& request, http_field_kind name, visitor_type&& visitor) {
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

    static void set_protocol_version(
        http_request& request, http_protocol_version protocol_version) noexcept {
        request.protocol_version_ = protocol_version;
    }

    // Call once after syntax validation, with the exact semantic field count.
    static void reserve_headers(http_request& request, std::size_t count) {
        if (count > (std::numeric_limits<std::uint16_t>::max)() || !request.headers_.empty()) {
            throw std::logic_error("invalid request header block reservation");
        }
        request.headers_.reserve(count);
    }

    static bool add_header(
        http_request& request, http_header_view header_value, http_field_kind kind) {
        if (request.headers_.size() == (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        const auto known_slot = known_header_slot(kind);
        request.headers_.append(
            header_value, known_slot < cached_header_slots ? kind : http_field_kind::other);
        if (known_slot < cached_header_slots) {
            if (request.cached_headers_[known_slot] != 0) {
                request.headers_.mark_repeated_known_slot(known_slot);
            }
            request.cached_headers_[known_slot] = static_cast<std::uint16_t>(request.headers_.size());
        }
        return true;
    }

    [[nodiscard]] static http_field_kind header_kind(
        const http_request& request, std::size_t index) noexcept {
        return request.headers_.kind_at(index);
    }

    static void set_body(http_request& request, std::span<const std::byte> body) noexcept {
        request.body_ = body;
    }
};

struct binary_request_materialization_spec final {
    http_protocol_version version_{http_protocol_version::http2};
    ::ruvia::http_request_target_form target_form_{::ruvia::http_request_target_form::http2};
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view host_{};
    std::string_view path_{};
    std::string_view protocol_{};
    std::string_view cookie_{};
    std::string_view body_{};
    std::size_t regular_header_count_{0};
    bool has_authority_{false};
    bool has_host_{false};
    bool has_path_{false};
    bool has_cookie_{false};
    bool standard_connect_{false};
    const char* failure_message_{"validated binary HTTP request cannot be materialized"};
};

template <typename header_accessor_type>
[[nodiscard]] inline http_request materialize_binary_http_request(
    const binary_request_materialization_spec& spec,
    header_accessor_type&& header_at) {
    auto request = http_request_access::make();
    if (spec.method_.empty()) {
        throw std::logic_error(spec.failure_message_);
    }
    const auto target = spec.standard_connect_ ? spec.authority_ : spec.path_;
    if (target.empty() && (spec.standard_connect_ || !spec.has_path_)) {
        throw std::logic_error(spec.failure_message_);
    }
    std::string_view request_path = target;
    std::string_view query_string;
    if (!spec.standard_connect_ && target != "*") {
        if (const auto query = target.find('?'); query != std::string_view::npos) {
            request_path = target.substr(0, query);
            query_string = target.substr(query + 1);
        }
    }
    const auto effective_authority = spec.has_authority_ ? spec.authority_ : spec.host_;

    http_request_access::set_method(request, spec.method_);
    http_request_access::set_protocol_version(request, spec.version_);
    http_request_access::set_target(request, target);
    http_request_access::set_scheme(request, spec.scheme_);
    http_request_access::set_authority(request, effective_authority);
    http_request_access::set_protocol(request, spec.protocol_);
    http_request_access::set_target_form(request, spec.target_form_);
    http_request_access::set_path(request, request_path);
    http_request_access::set_query_string(request, query_string);
    http_request_access::set_body(request, std::as_bytes(std::span(spec.body_.data(), spec.body_.size())));

    // A non-HTTP target can carry an authority that is not legal Host syntax;
    // never manufacture an invalid regular field from that distinct grammar.
    const bool synthesize_host = binary_request_synthesizes_host(
        spec.has_host_, spec.has_authority_, spec.scheme_, spec.authority_);
    const auto header_count = spec.regular_header_count_ + static_cast<std::size_t>(synthesize_host) +
                              static_cast<std::size_t>(spec.has_cookie_);
    if (header_count > (std::numeric_limits<std::uint16_t>::max)()) {
        throw std::logic_error(spec.failure_message_);
    }
    http_request_access::reserve_headers(request, header_count);
    for (std::size_t index = 0; index < spec.regular_header_count_; ++index) {
        const auto [field, kind] = header_at(index);
        if (!http_request_access::add_header(request, field, kind)) {
            throw std::logic_error(spec.failure_message_);
        }
    }
    if (synthesize_host &&
        !http_request_access::add_header(
            request, http_header_view{"host", spec.authority_}, http_field_kind::host)) {
        throw std::logic_error(spec.failure_message_);
    }
    if (spec.has_cookie_ &&
        !http_request_access::add_header(
            request, http_header_view{"cookie", spec.cookie_}, http_field_kind::cookie)) {
        throw std::logic_error(spec.failure_message_);
    }
    return request;
}

}  // namespace ruvia::detail
