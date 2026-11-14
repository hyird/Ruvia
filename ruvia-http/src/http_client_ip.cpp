#include "http_client_ip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_connection_fields.h"
#include "http_request.h"

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
    if (colon != std::string_view::npos && node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::optional<http_scheme> parse_forwarded_http_scheme(std::string_view token) noexcept {
    token = http_trim_quoted_field_value(http_trim_ows(token));
    if (http_ascii_equals_ignore_case(token, "http")) {
        return http_scheme::http;
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return http_scheme::https;
    }
    return std::nullopt;
}

struct forwarded_hop final {
    std::optional<ip_address> address_{};
    std::optional<http_scheme> scheme_{};
};

[[nodiscard]] std::optional<ip_address> parse_hop_address(std::string_view node_value) noexcept {
    const auto address_text = extract_forwarded_node_address(node_value);
    if (address_text.empty()) {
        return std::nullopt;
    }
    const auto parsed = ip_address::parse(address_text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->normalized_ipv4();
}

[[nodiscard]] forwarded_hop parse_rfc7239_element(std::string_view element) noexcept {
    forwarded_hop hop{};
    bool saw_for = false;
    bool saw_proto = false;
    http_visit_semicolon_parameters_quoted_field(element, [&](std::string_view name, std::string_view value) noexcept {
        if (http_ascii_equals_ignore_case(name, "for")) {
            if (saw_for) {
                hop.address_.reset();
            } else {
                saw_for = true;
                hop.address_ = parse_hop_address(value);
            }
        } else if (http_ascii_equals_ignore_case(name, "proto")) {
            if (saw_proto) {
                hop.scheme_.reset();
            } else {
                saw_proto = true;
                hop.scheme_ = parse_forwarded_http_scheme(value);
            }
        }
        return true;
    });
    return hop;
}

class forwarding_chain final {
public:
    void append(const http_client_ip_extractor& extractor, forwarded_hop hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const bool trusted = hop.address_.has_value() && extractor.is_trusted_proxy(*hop.address_);
        if (trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = hop.address_.has_value();
            has_client_hop_ = true;
            nearest_trusted_.reset();
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    [[nodiscard]] forwarded_hop client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return has_client_hop_ ? client_index_ : 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_hop leftmost_{};
    forwarded_hop client_hop_{};
    std::optional<ip_address> nearest_trusted_{};
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

template <typename header_span_type>
[[nodiscard]] std::optional<http_client_ip_result> resolve_from_header(const http_client_ip_extractor& extractor, const ip_address& normalized_peer, std::string_view header_name, http_forwarded_header_kind kind, header_span_type headers) noexcept {
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
        const auto parsed = parse_hop_address(trimmed);
        if (!parsed.has_value() || extractor.is_trusted_proxy(*parsed)) {
            return std::nullopt;
        }
        return http_client_ip_result{.address_ = *parsed, .from_forwarded_header_ = true, .scheme_ = std::nullopt};
    }

    forwarding_chain chain;
    for (const auto& field : headers) {
        if (!http_ascii_equals_ignore_case(field.name(), header_name)) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(field.value(), [&](std::string_view item) noexcept {
            if (item.empty()) {
                return true;
            }
            if (kind == http_forwarded_header_kind::rfc7239_forwarded) {
                chain.append(extractor, parse_rfc7239_element(item));
            } else {
                chain.append(extractor, {.address_ = parse_hop_address(item), .scheme_ = std::nullopt});
            }
            return true;
        });
    }
    if (chain.size() == 0) {
        return std::nullopt;
    }
    auto resolved = chain.client();
    if (kind == http_forwarded_header_kind::comma_separated_list && extractor.trust_x_forwarded_proto() && http_ascii_equals_ignore_case(header_name, "X-Forwarded-For")) {
        std::size_t proto_count = 0;
        std::optional<http_scheme> matched_scheme{};
        const auto target_index = chain.client_index();
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "X-Forwarded-Proto")) {
                continue;
            }
            http_visit_comma_separated_quoted_field_items(field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                if (proto_count == target_index) {
                    matched_scheme = parse_forwarded_http_scheme(item);
                }
                ++proto_count;
                return true;
            });
        }
        if (proto_count == chain.size()) {
            resolved.scheme_ = matched_scheme;
        }
    }
    if (!resolved.address_.has_value() && !resolved.scheme_.has_value()) {
        return std::nullopt;
    }
    return http_client_ip_result{
        .address_ = resolved.address_.value_or(normalized_peer),
        .from_forwarded_header_ = true,
        .scheme_ = resolved.scheme_,
    };
}

[[nodiscard]] http_forwarded_header_kind resolve_header_kind(std::string_view name, http_forwarded_header_kind configured) noexcept {
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
}  // namespace

http_client_ip_extractor::http_client_ip_extractor(const http_client_ip_config& config) :
    enabled_(config.enabled_),
    trust_x_forwarded_proto_(config.trust_x_forwarded_proto_) {
    trusted_cidrs_.reserve(config.trusted_proxies_.size());
    for (const auto& cidr_text : config.trusted_proxies_) {
        const auto parsed = ip_cidr::parse(cidr_text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("trusted proxy entry must be a valid IP address or CIDR");
        }
        trusted_cidrs_.push_back(*parsed);
    }

    if (config.forwarding_headers_.empty()) {
        compiled_header xff;
        xff.name_.assign("X-Forwarded-For");
        xff.kind_ = http_forwarded_header_kind::comma_separated_list;
        headers_.push_back(std::move(xff));

        compiled_header fwd;
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
        compiled_header entry;
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

http_client_ip_result http_client_ip_extractor::resolve(const ip_address& peer_ip, std::span<const http_header> headers) const noexcept {
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
        if (const auto extracted = resolve_from_header(*this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header_view& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(*this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(const ip_address& peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(std::string_view peer_ip, std::span<const http_header> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(std::string_view peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

}  // namespace ruvia
