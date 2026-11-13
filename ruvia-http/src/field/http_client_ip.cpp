#include "ruvia/http/http_client_ip.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_values.h"

namespace ruvia {
namespace {

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
[[nodiscard]] std::string_view extract_forwarded_node_address(std::string_view node_value) noexcept {
    node_value = http_trim_quoted_field_value(http_trim_ows(node_value));
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        if (close == std::string_view::npos) {
            return {};
        }
        const auto rest = node_value.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') {
            return {};
        }
        return node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos &&
        node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::string_view extract_rfc7239_for_node(std::string_view element) noexcept {
    std::string_view address{};
    bool saw_for = false;
    bool valid = true;
    http_visit_semicolon_parameters_quoted_field(
        element, [&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "for")) {
                if (saw_for) {
                    valid = false;
                    address = {};
                    return false;
                }
                saw_for = true;
                address = extract_forwarded_node_address(value);
            }
            return true;
        });
    return (valid && saw_for) ? address : std::string_view{};
}

[[nodiscard]] http_forwarded_header_kind resolve_header_kind(
    std::string_view name, http_forwarded_header_kind configured) noexcept {
    if (configured != http_forwarded_header_kind::auto_detect) {
        return configured;
    }
    if (http_ascii_equals_ignore_case(name, "Forwarded")) {
        return http_forwarded_header_kind::rfc7239_forwarded;
    }
    if (http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
        return http_forwarded_header_kind::comma_separated_list;
    }
    return http_forwarded_header_kind::single_ip;
}

template <typename header_span_type>
[[nodiscard]] std::optional<ip_address> resolve_from_header(
    const http_client_ip_extractor& extractor, std::string_view header_name,
    http_forwarded_header_kind kind, header_span_type headers) noexcept {
    if (kind == http_forwarded_header_kind::single_ip) {
        std::string_view single_value{};
        std::size_t line_count = 0;
        for (const auto& field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), header_name)) {
                single_value = field.value();
                ++line_count;
            }
        }
        if (line_count != 1) {
            return std::nullopt;
        }
        const auto trimmed = http_trim_ows(single_value);
        if (trimmed.empty() || trimmed.find(',') != std::string_view::npos) {
            return std::nullopt;
        }
        const auto node = extract_forwarded_node_address(trimmed);
        const auto parsed = ip_address::parse(node);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        const auto normalized = parsed->normalized_ipv4();
        if (extractor.is_trusted_proxy(normalized)) {
            return std::nullopt;
        }
        return normalized;
    }

    std::array<std::string_view, 64> hops{};
    std::size_t hop_count = 0;
    bool overflow = false;
    for (const auto& field : headers) {
        if (!http_ascii_equals_ignore_case(field.name(), header_name)) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(
            field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                if (hop_count == hops.size()) {
                    overflow = true;
                    return false;
                }
                hops[hop_count++] =
                    kind == http_forwarded_header_kind::rfc7239_forwarded
                        ? extract_rfc7239_for_node(item)
                        : extract_forwarded_node_address(item);
                return true;
            });
        if (overflow) {
            return std::nullopt;
        }
    }
    if (hop_count == 0) {
        return std::nullopt;
    }

    for (std::size_t index = hop_count; index > 0; --index) {
        const auto candidate = hops[index - 1];
        if (candidate.empty()) {
            return std::nullopt;
        }
        const auto parsed = ip_address::parse(candidate);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        const auto normalized = parsed->normalized_ipv4();
        if (!extractor.is_trusted_proxy(normalized)) {
            return normalized;
        }
    }
    return std::nullopt;
}

}  // namespace

http_client_ip_extractor::http_client_ip_extractor(
    const http_client_ip_config& config, std::pmr::memory_resource* resource)
    : trusted_cidrs_(detail::http_pmr_resource_or_default(resource)),
      headers_(detail::http_pmr_resource_or_default(resource)),
      enabled_(config.enabled_) {
    auto* const target_resource = trusted_cidrs_.get_allocator().resource();
    trusted_cidrs_.reserve(config.trusted_proxies_.size());
    for (const auto& cidr_text : config.trusted_proxies_) {
        const auto parsed = ip_cidr::parse(cidr_text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("trusted proxy entry must be a valid IP address or CIDR");
        }
        trusted_cidrs_.push_back(*parsed);
    }

    if (config.forwarding_headers_.empty()) {
        compiled_header xff(target_resource);
        xff.name_.assign("X-Forwarded-For");
        xff.kind_ = http_forwarded_header_kind::comma_separated_list;
        headers_.push_back(std::move(xff));

        compiled_header fwd(target_resource);
        fwd.name_.assign("Forwarded");
        fwd.kind_ = http_forwarded_header_kind::rfc7239_forwarded;
        headers_.push_back(std::move(fwd));
        return;
    }

    headers_.reserve(config.forwarding_headers_.size());
    for (const auto& spec : config.forwarding_headers_) {
        if (!is_valid_http_header_name(spec.name_)) {
            throw std::invalid_argument("forwarding header name must be a valid HTTP field name");
        }
        compiled_header entry(target_resource);
        entry.name_.assign(spec.name_);
        entry.kind_ = resolve_header_kind(spec.name_, spec.kind_);
        headers_.push_back(std::move(entry));
    }
}

bool http_client_ip_extractor::is_trusted_proxy(const ip_address& address) const noexcept {
    return std::ranges::any_of(trusted_cidrs_, [&address](const ip_cidr& cidr) noexcept {
        return cidr.contains(address);
    });
}

bool http_client_ip_extractor::is_trusted_proxy(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && is_trusted_proxy(*parsed);
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted =
                resolve_from_header(*this, header_spec.name_, header_spec.kind_, headers)) {
            return {.address_ = *extracted, .from_forwarded_header_ = true};
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present =
            std::ranges::any_of(headers, [&](const http_header_view& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
            });
        if (!present) {
            continue;
        }
        if (const auto extracted =
                resolve_from_header(*this, header_spec.name_, header_spec.kind_, headers)) {
            return {.address_ = *extracted, .from_forwarded_header_ = true};
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

}  // namespace ruvia
