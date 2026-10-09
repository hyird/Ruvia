#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"

#include "http2/http2_request_headers.h"
#include "http2/http2_stream_state.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia::detail {

class http2_request_builder;

class http2_request_built final {
private:
    friend class http2_request_build_result;

    constexpr http2_request_built() noexcept = default;
};

class http2_request_build_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        switch (kind_) {
            case kind_type::missing_method:
                return http_protocol_error(http_status::bad_request, "missing HTTP/2 :method");
            case kind_type::missing_target:
                return http_protocol_error(http_status::bad_request, "missing HTTP/2 request target");
            case kind_type::invalid_target:
                return http_protocol_error(http_status::bad_request, "invalid HTTP/2 request target");
            case kind_type::too_many_headers:
                return http_protocol_error(
                    http_status::request_header_fields_too_large, "too many HTTP/2 request headers");
        }
        return http_protocol_error(http_status::bad_request, "invalid HTTP/2 request");
    }

private:
    friend class http2_request_build_result;
    friend class http2_request_builder;

    enum class kind_type : std::uint8_t {
        missing_method,
        missing_target,
        invalid_target,
        too_many_headers
    };

    explicit constexpr http2_request_build_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

// Building the runtime-facing http_request either completes the whole borrowed
// message view or returns one HTTP-owned protocol failure. The private failure
// kind prevents runtimes from reconstructing status/message mappings.
class http2_request_build_result final {
public:
    [[nodiscard]] constexpr const http2_request_built* built() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http2_request_built* built() const&& = delete;

    [[nodiscard]] constexpr const http2_request_build_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http2_request_build_failure* failure() const&& = delete;

private:
    friend class http2_request_builder;

    using value_type = std::variant<http2_request_built, http2_request_build_failure>;

    explicit constexpr http2_request_build_result(http2_request_built built) noexcept
        : value_(built) {}

    explicit constexpr http2_request_build_result(http2_request_build_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr http2_request_build_result make_built() noexcept {
        return http2_request_build_result(http2_request_built());
    }

    [[nodiscard]] static constexpr http2_request_build_result make_failure(
        http2_request_build_failure::kind_type kind) noexcept {
        return http2_request_build_result(http2_request_build_failure(kind));
    }

    value_type value_;
};

class http2_request_builder final {
public:
    [[nodiscard]] static std::string_view request_target(const http2_stream_state& stream) noexcept {
        const auto* pending = stream.tunnel().pending();
        const bool standard_connect =
            pending != nullptr && pending->form() == http2_connect_form::standard;
        return standard_connect ? stream.request_authority() : stream.request_path();
    }

    [[nodiscard]] static std::string_view request_path(const http2_stream_state& stream) noexcept {
        return split_request_target(request_target(stream)).path_;
    }

    [[nodiscard]] static http2_request_build_result build(http2_stream_state& stream,
        http_request& request, std::pmr::memory_resource* resource, std::string_view body) {
        http_request_access::reset(request);
        http_request_access::set_resource(request, resource);
        const auto method = stream.request_method();
        if (method.empty()) {
            return http2_request_build_result::make_failure(
                http2_request_build_failure::kind_type::missing_method);
        }
        const auto* pending = stream.tunnel().pending();
        const bool standard_connect =
            pending != nullptr && pending->form() == http2_connect_form::standard;
        const bool extended_connect =
            pending != nullptr && pending->form() == http2_connect_form::extended;
        const auto target = request_target(stream);
        if (target.empty() && (standard_connect || !stream.has_path())) {
            return http2_request_build_result::make_failure(
                http2_request_build_failure::kind_type::missing_target);
        }
        request_target_parts_type target_parts;
        if (standard_connect) {
            target_parts = split_request_target(target);
        } else if (target.empty()) {
            const bool valid_empty_target =
                stream.has_scheme() &&
                (extended_connect ? http2_is_valid_extended_connect_path(stream.request_scheme(), target)
                                  : http2_is_valid_regular_request_path(stream.request_known_method(),
                                        stream.request_scheme(), target));
            if (!valid_empty_target) {
                return http2_request_build_result::make_failure(
                    http2_request_build_failure::kind_type::invalid_target);
            }
            target_parts = request_target_parts_type{};
        } else {
            request_target_view target_view;
            // Extended CONNECT retains normal :scheme/:path target components.
            // Parse its target using the origin-form grammar without changing the
            // wire method stored on http_request.
            const auto target_method =
                extended_connect ? http_known_method::get : stream.request_known_method();
            if (!parse_request_target(target_method, target, target_view)) {
                return http2_request_build_result::make_failure(
                    http2_request_build_failure::kind_type::invalid_target);
            }
            target_parts =
                request_target_parts_type{.path_ = target_view.path_, .query_string_ = target_view.query_};
        }

        http_request_access::set_method(request, method);
        http_request_access::set_protocol_version(request, http_protocol_version::http2);
        http_request_access::set_target(request, target);
        http_request_access::set_scheme(request, stream.request_scheme());
        http_request_access::set_authority(request, http2_effective_request_authority(stream));
        http_request_access::set_target_form(request, ::ruvia::http_request_target_form::http2);
        http_request_access::set_path(request, target_parts.path_);
        http_request_access::set_query_string(request, target_parts.query_string_);
        http_request_access::set_body(request, body);

        const auto authority = stream.request_authority();
        const bool synthesize_host = !stream.has_host() && stream.has_authority() && is_valid_host_header(authority);
        const auto header_count = stream.remote_header_count() + static_cast<std::size_t>(synthesize_host) +
                                  static_cast<std::size_t>(stream.has_cookie());
        if (header_count > max_http_header_fields) {
            return http2_request_build_result::make_failure(http2_request_build_failure::kind_type::too_many_headers);
        }
        http_request_access::reserve_headers(request, header_count);
        for (std::size_t i = 0; i < stream.remote_header_count(); ++i) {
            const auto header_value = stream.remote_header_at(i);
            if (!add_header(request, header_value.name_, header_value.value_, header_value.kind_)) {
                return http2_request_build_result::make_failure(
                    http2_request_build_failure::kind_type::too_many_headers);
            }
        }
        // A non-HTTP target can carry RFC 3986 userinfo or another authority
        // value that is not legal Host syntax. Never manufacture an invalid
        // regular field from that distinct pseudo-header grammar.
        if (synthesize_host) {
            if (!add_header(request, "host", authority, request_header_kind::host)) {
                return http2_request_build_result::make_failure(
                    http2_request_build_failure::kind_type::too_many_headers);
            }
        }
        if (stream.has_cookie()) {
            if (!add_header(request, "cookie", stream.request_cookie(), request_header_kind::cookie)) {
                return http2_request_build_result::make_failure(
                    http2_request_build_failure::kind_type::too_many_headers);
            }
        }
        return http2_request_build_result::make_built();
    }

private:
    struct request_target_parts_type final {
        std::string_view path_;
        std::string_view query_string_;
    };

    [[nodiscard]] static request_target_parts_type split_request_target(std::string_view target) noexcept {
        if (target == "*") {
            return request_target_parts_type{.path_ = "*", .query_string_ = {}};
        }
        const auto query = target.find('?');
        if (query == std::string_view::npos) {
            return request_target_parts_type{.path_ = target, .query_string_ = {}};
        }
        return request_target_parts_type{
            .path_ = target.substr(0, query), .query_string_ = target.substr(query + 1)};
    }

    static bool add_header(http_request& request, std::string_view name, std::string_view value,
        request_header_kind kind) {
        return http_request_access::add_header(
            request, http_header_view{name, value}, request_header_kind_known_slot(kind));
    }
};

}  // namespace ruvia::detail
