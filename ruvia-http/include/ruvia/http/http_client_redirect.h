#pragma once

// Outbound HTTP redirect protocol helpers.
//
// These values contain no transport policy: redirect limits, cross-origin
// authorization, connection selection, and retries remain owned by the external
// I/O runtime. The helpers only classify HTTP response fields, apply the RFC
// method/content rewrite rules, and resolve one same-origin URI-reference.

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_client.h"

namespace ruvia {

[[nodiscard]] bool is_http_client_redirect_status(http_status_code status) noexcept;

class http_client_response_header_lookup_result;

class http_client_response_header_absent final {
private:
    friend class http_client_response_header_lookup_result;
    constexpr http_client_response_header_absent() noexcept = default;
};

class http_client_response_header_found final {
public:
    // Borrowed from the owning http_client_response_head passed to the lookup.
    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    friend class http_client_response_header_lookup_result;

    explicit constexpr http_client_response_header_found(std::string_view value) noexcept
        : value_(value) {}

    std::string_view value_;
};

class http_client_response_header_repeated final {
private:
    friend class http_client_response_header_lookup_result;
    constexpr http_client_response_header_repeated() noexcept = default;
};

// A non-list response field has three mutually exclusive lookup outcomes. Only
// the found alternative exposes a value, so an absent/repeated field cannot be
// mistaken for a present field whose value is empty.
class http_client_response_header_lookup_result final {
public:
    [[nodiscard]] constexpr const http_client_response_header_absent* absent() const& noexcept {
        return std::get_if<http_client_response_header_absent>(&value_);
    }
    const http_client_response_header_absent* absent() const&& = delete;

    [[nodiscard]] constexpr const http_client_response_header_found* found() const& noexcept {
        return std::get_if<http_client_response_header_found>(&value_);
    }
    const http_client_response_header_found* found() const&& = delete;

    [[nodiscard]] constexpr const http_client_response_header_repeated* repeated() const& noexcept {
        return std::get_if<http_client_response_header_repeated>(&value_);
    }
    const http_client_response_header_repeated* repeated() const&& = delete;

private:
    friend http_client_response_header_lookup_result lookup_unique_http_client_response_header(
        const http_client_response_head&, std::string_view) noexcept;

    using value_type = std::variant<http_client_response_header_absent, http_client_response_header_found,
        http_client_response_header_repeated>;

    template <typename result_type>
    explicit constexpr http_client_response_header_lookup_result(result_type result_value) noexcept
        : value_(result_value) {}

    [[nodiscard]] static constexpr http_client_response_header_lookup_result make_absent() noexcept {
        return http_client_response_header_lookup_result(http_client_response_header_absent());
    }

    [[nodiscard]] static constexpr http_client_response_header_lookup_result make_found(
        std::string_view value) noexcept {
        return http_client_response_header_lookup_result(http_client_response_header_found(value));
    }

    [[nodiscard]] static constexpr http_client_response_header_lookup_result make_repeated() noexcept {
        return http_client_response_header_lookup_result(http_client_response_header_repeated());
    }

    value_type value_;
};

[[nodiscard]] http_client_response_header_lookup_result lookup_unique_http_client_response_header(
    const http_client_response_head& head, std::string_view name) noexcept;
[[nodiscard]] http_client_response_header_lookup_result lookup_unique_http_client_response_header(
    const http_client_response_head&& head, std::string_view name) = delete;

enum class http_client_redirect_content_disposition : std::uint8_t {
    preserve,
    // The I/O owner must omit both the representation and content-specific
    // fields when constructing the redirected request (RFC 9110 Section 15.4).
    drop,
};

struct http_client_redirect_request_plan_options final {
    http_status_code status_;
    std::pmr::memory_resource* resource_{nullptr};
};

// The method is copied into caller-selected PMR storage so the plan does not
// inherit the request or request-method backing storage lifetime.
class http_client_redirect_request_plan final {
public:
    http_client_redirect_request_plan(const http_client_redirect_request_plan&) = delete;
    http_client_redirect_request_plan& operator=(const http_client_redirect_request_plan&) = delete;
    http_client_redirect_request_plan(http_client_redirect_request_plan&&) noexcept = default;
    http_client_redirect_request_plan& operator=(http_client_redirect_request_plan&&) = delete;

    [[nodiscard]] std::string_view method() const& noexcept {
        return method_;
    }
    std::string_view method() const&& = delete;

    [[nodiscard]] constexpr http_client_redirect_content_disposition content_disposition()
        const noexcept {
        return content_disposition_;
    }

private:
    friend http_client_redirect_request_plan plan_http_client_redirect_request(
        const http_client_request_view&, http_client_redirect_request_plan_options);

    http_client_redirect_request_plan(std::string_view method,
        http_client_redirect_content_disposition content_disposition,
        std::pmr::memory_resource* resource);

    std::pmr::string method_;
    http_client_redirect_content_disposition content_disposition_;
};

[[nodiscard]] http_client_redirect_request_plan plan_http_client_redirect_request(
    const http_client_request_view& request, http_client_redirect_request_plan_options options);

// This classification has no alternative-specific payload, so an enum is the
// complete result rather than a status coupled to unrelated fields.
enum class http_client_origin_authority_status : std::uint8_t {
    same_origin,
    different_origin,
    invalid_authority,
};

[[nodiscard]] http_client_origin_authority_status classify_http_client_origin_authority(
    const http_origin_view& origin, std::string_view authority) noexcept;

enum class http_client_redirect_resolution_error : std::uint8_t {
    invalid_current_target,
    invalid_location,
    // The Location names a scheme this client cannot follow (anything other
    // than http/https). The response itself is well-formed; the I/O owner
    // decides whether to surface the response or fail the exchange.
    unsupported_scheme,
};

struct http_client_redirect_target_options final {
    std::string_view current_target_{};
    std::string_view location_{};
    std::pmr::memory_resource* resource_{nullptr};
};

class http_client_redirect_resolution_result;

// A followable redirect destination: the resolved origin plus the origin-form
// target on that origin. `cross_origin()` is true whenever the destination
// scheme/host/port triple differs from the request origin; RFC 9110 Section
// 15.4 and the fetch specification require the I/O owner to drop credentials
// (Authorization, Proxy-Authorization, cookie material not scoped to the new
// origin) before following a cross-origin redirect.
class http_client_resolved_redirect final {
public:
    http_client_resolved_redirect(const http_client_resolved_redirect&) = delete;
    http_client_resolved_redirect& operator=(const http_client_resolved_redirect&) = delete;
    http_client_resolved_redirect(http_client_resolved_redirect&&) noexcept = default;
    http_client_resolved_redirect& operator=(http_client_resolved_redirect&&) = delete;

    [[nodiscard]] std::string_view target() const& noexcept {
        return target_;
    }
    std::string_view target() const&& = delete;

    [[nodiscard]] http_scheme scheme() const noexcept {
        return scheme_;
    }

    // RFC 3986 uri-host of the destination; IP literals keep their brackets,
    // matching the http_origin_view factory contract.
    [[nodiscard]] std::string_view host() const& noexcept {
        return host_;
    }
    std::string_view host() const&& = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] bool cross_origin() const noexcept {
        return cross_origin_;
    }

    // Borrows host() storage: the returned origin is valid only while this
    // resolved redirect is alive.
    [[nodiscard]] http_origin_view origin() const&;
    http_origin_view origin() const&& = delete;

private:
    friend class http_client_redirect_resolution_result;

    http_client_resolved_redirect(http_scheme scheme, std::pmr::string host, std::uint16_t port,
        std::pmr::string target, bool cross_origin) noexcept
        : scheme_(scheme),
          host_(std::move(host)),
          port_(port),
          target_(std::move(target)),
          cross_origin_(cross_origin) {}

    http_scheme scheme_;
    std::pmr::string host_;
    std::uint16_t port_;
    std::pmr::string target_;
    bool cross_origin_;
};

class http_client_redirect_resolution_failure final {
public:
    [[nodiscard]] constexpr http_client_redirect_resolution_error error() const noexcept {
        return error_;
    }

private:
    friend class http_client_redirect_resolution_result;

    explicit constexpr http_client_redirect_resolution_failure(
        http_client_redirect_resolution_error error) noexcept
        : error_(error) {}

    http_client_redirect_resolution_error error_;
};

// Cross-origin-capable resolution: either one owned followable destination or
// one typed failure. A different http/https origin is a success alternative
// classified by cross_origin(); the I/O owner applies its own cross-origin
// policy (credential strip, TLS-downgrade refusal) on top of the classified
// destination.
class http_client_redirect_resolution_result final {
public:
    http_client_redirect_resolution_result(const http_client_redirect_resolution_result&) = delete;
    http_client_redirect_resolution_result& operator=(
        const http_client_redirect_resolution_result&) = delete;
    http_client_redirect_resolution_result(http_client_redirect_resolution_result&&) noexcept = default;
    http_client_redirect_resolution_result& operator=(http_client_redirect_resolution_result&&) = delete;

    [[nodiscard]] const http_client_resolved_redirect* resolved() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http_client_resolved_redirect* resolved() const&& = delete;

    [[nodiscard]] constexpr const http_client_redirect_resolution_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http_client_redirect_resolution_failure* failure() const&& = delete;

private:
    friend http_client_redirect_resolution_result resolve_http_client_redirect_target(
        const http_origin_view&, http_client_redirect_target_options);

    using value_type = std::variant<http_client_resolved_redirect, http_client_redirect_resolution_failure>;

    explicit http_client_redirect_resolution_result(http_client_resolved_redirect resolved) noexcept
        : value_(std::move(resolved)) {}

    explicit constexpr http_client_redirect_resolution_result(
        http_client_redirect_resolution_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static http_client_redirect_resolution_result make_resolved(http_scheme scheme,
        std::pmr::string host, std::uint16_t port, std::pmr::string target,
        bool cross_origin) noexcept {
        return http_client_redirect_resolution_result(http_client_resolved_redirect(
            scheme, std::move(host), port, std::move(target), cross_origin));
    }

    [[nodiscard]] static constexpr http_client_redirect_resolution_result make_failure(
        http_client_redirect_resolution_error error) noexcept {
        return http_client_redirect_resolution_result(http_client_redirect_resolution_failure(error));
    }

    value_type value_;
};

[[nodiscard]] http_client_redirect_resolution_result resolve_http_client_redirect_target(
    const http_origin_view& origin, http_client_redirect_target_options options);

}  // namespace ruvia
