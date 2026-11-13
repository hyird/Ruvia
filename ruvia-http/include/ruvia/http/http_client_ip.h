#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/ip_address.h"

namespace ruvia {

enum class http_forwarded_header_kind : std::uint8_t {
    auto_detect,
    rfc7239_forwarded,
    comma_separated_list,
    single_ip,
};

struct http_forwarded_header_spec final {
    http_forwarded_header_spec() noexcept = default;

    http_forwarded_header_spec(std::string_view name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : name_(name),
          kind_(kind) {}

    http_forwarded_header_spec(const char* name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : http_forwarded_header_spec(std::string_view(name), kind) {}

    http_forwarded_header_spec(const std::string& name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : name_(name),
          kind_(kind) {}

    std::string name_;
    http_forwarded_header_kind kind_{http_forwarded_header_kind::auto_detect};
};

struct http_client_ip_config final {
    http_client_ip_config() noexcept = default;

    bool enabled_{true};
    // Trusted proxy IP literals or CIDR ranges (e.g. "127.0.0.1", "10.0.0.0/8", "::1").
    std::vector<std::string> trusted_proxies_;
    // Priority-ordered forwarding headers. When empty, defaults to
    // {"X-Forwarded-For", "Forwarded"}.
    std::vector<http_forwarded_header_spec> forwarding_headers_;
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto_{false};
};

struct http_client_ip_result final {
    ip_address address_{};
    bool from_forwarded_header_{false};
    std::optional<http_scheme> scheme_{};

    [[nodiscard]] constexpr ip_address address() const noexcept {
        return address_;
    }
    [[nodiscard]] constexpr bool from_forwarded_header() const noexcept {
        return from_forwarded_header_;
    }
    [[nodiscard]] constexpr std::optional<http_scheme> scheme() const noexcept {
        return scheme_;
    }

    friend constexpr bool operator==(const http_client_ip_result&, const http_client_ip_result&) noexcept = default;
};

// Stateless client IP extractor for deployments behind trusted reverse proxies.
class http_client_ip_extractor final {
public:
    http_client_ip_extractor() noexcept
        : enabled_(false) {}

    explicit http_client_ip_extractor(const http_client_ip_config& config);

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return enabled_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !enabled_ || trusted_cidrs_.empty();
    }
    [[nodiscard]] constexpr bool trust_x_forwarded_proto() const noexcept {
        return trust_x_forwarded_proto_;
    }
    [[nodiscard]] bool is_trusted_proxy(const ip_address& address) const noexcept;
    [[nodiscard]] bool is_trusted_proxy(std::string_view address_text) const noexcept;

    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, std::span<const http_header> headers) const noexcept;
    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept;
    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, const http_request& request) const noexcept;

    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, std::span<const http_header> headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, const http_request& request) const noexcept;

private:
    struct compiled_header final {
        std::string name_;
        http_forwarded_header_kind kind_{http_forwarded_header_kind::comma_separated_list};
    };

    std::vector<ip_cidr> trusted_cidrs_;
    std::vector<compiled_header> headers_;
    bool enabled_{true};
    bool trust_x_forwarded_proto_{false};
};

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    std::span<const http_header> headers) noexcept {
    return extractor.resolve(peer_ip, headers);
}

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    std::span<const http_header_view> headers) noexcept {
    return extractor.resolve(peer_ip, headers);
}

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    const http_request& request) noexcept {
    return extractor.resolve(peer_ip, request);
}

}  // namespace ruvia
