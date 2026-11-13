#include "ruvia/http/http_request.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_target_origin.h"
#include "ruvia/http/url_encoding.h"

#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"
#include "parser/http_uri_grammar.h"
#include "request/http_request_access.h"

namespace ruvia {
http_request& http_request::operator=(http_request&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Publish the whole request before an old descriptor's allocator can
    // reenter. The return reference is fixed before retired storage is freed.
    http_request retired(std::move(*this));
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

std::optional<std::string_view> http_request::header(std::string_view name) const noexcept {
    const auto kind = detail::classify_http_field(name);
    if (kind != detail::http_field_kind::other) {
        const auto known_slot = detail::http_field_kind_known_slot(kind);
        const auto index = cached_headers_[known_slot];
        if (index == 0 || index > headers_.size()) {
            return std::nullopt;
        }
        return headers_[index - 1].value();
    }

    for (std::size_t i = headers_.size(); i > 0; --i) {
        const auto index = i - 1;
        if (headers_.kind_at(index) == detail::http_field_kind::other &&
            http_ascii_equals_ignore_case(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return std::nullopt;
}

std::optional<std::string_view> http_request::last_raw_query_value(
    std::string_view raw_name) const noexcept {
    std::optional<std::string_view> result;
    (void)visit_url_encoded_pairs(
        query_string_, [&](std::string_view name, std::string_view value) noexcept {
            if (name == raw_name) {
                result = value;
            }
            return true;
        });
    return result;
}

std::optional<std::string_view> http_request::cookie(std::string_view name) const noexcept {
    if (!detail::http_request_access::has_known_header(*this, detail::http_field_kind::cookie)) {
        return std::nullopt;
    }
    // The last matching pair within one field line wins.
    const auto find_in = [name](std::string_view field_value) noexcept {
        std::optional<std::string_view> result;
        http_visit_cookie_pairs(field_value, [name, &result](std::string_view key, std::string_view value) noexcept {
            if (key == name) {
                result = value;
            }
            return true;
        });
        return result;
    };
    const auto last_cookie =
        detail::http_request_access::known_header(*this, detail::http_field_kind::cookie);
    if (auto value = find_in(last_cookie)) {
        return value;
    }
    if (!detail::http_request_access::has_repeated_known_header(
            *this, detail::http_field_kind::cookie)) {
        return std::nullopt;
    }
    const auto last_cookie_index =
        cached_headers_[detail::http_field_kind_known_slot(detail::http_field_kind::cookie)];
    // The one-based cache points to the last Cookie field; only earlier fields can contribute.
    for (std::size_t i = last_cookie_index - 1; i > 0; --i) {
        const auto index = i - 1;
        if (headers_.kind_at(index) != detail::http_field_kind::cookie) {
            continue;
        }
        if (auto value = find_in(headers_[index].value())) {
            return value;
        }
    }
    return std::nullopt;
}

http_response_coding_selection http_response_coding_selection_for(const http_request& request) noexcept {
    http_response_coding_qualities qualities;
    // Every field line contributes: RFC 9110 section 5.3 combines repeated list fields.
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::accept_encoding, [&qualities](std::string_view value) noexcept {
            qualities.update(value);
            return true;
        });
    return http_response_coding_selection::select(qualities);
}
}  // namespace ruvia

namespace ruvia {
namespace {

template <typename field_type>
[[nodiscard]] std::optional<http_request_trailer_error> append_trailer_fields(
    std::vector<http_header>& retained, http_field_section_budget& retained_budget,
    std::span<const field_type> fields) {
    auto budget = retained_budget;
    for (const auto& field : fields) {
        if (const auto error = detail::charge_http_request_trailer_field(budget, field.name(), field.value())) {
            return error;
        }
    }
    if (fields.empty()) {
        return std::nullopt;
    }
    const auto field_count = retained.size();
    std::vector<http_header> staged;
    staged.reserve(fields.size());
    for (const auto& field : fields) {
        staged.push_back(http_header::copy_with_lowercase_name(field.name(), field.value()));
    }
    retained.reserve(field_count + staged.size());
    for (auto& field : staged) {
        retained.push_back(std::move(field));
    }
    retained_budget = budget;
    return std::nullopt;
}

}  // namespace

std::optional<http_request_trailer_error> http_request_trailers::append(std::string_view name, std::string_view value) {
    const http_header_view field{name, value};
    return append(std::span<const http_header_view>(&field, 1));
}

std::optional<http_request_trailer_error> http_request_trailers::append(std::span<const http_header_view> fields) {
    return append_trailer_fields(fields_, budget_, fields);
}

std::optional<http_request_trailer_error> http_request_trailers::append(std::span<const http_header> fields) {
    return append_trailer_fields(fields_, budget_, fields);
}

std::optional<http_request_trailer_error> http_request_trailers::append_received(std::span<const http_header> fields) {
    auto budget = budget_;
    std::size_t retained_count = 0;
    for (const auto& field : fields) {
        const auto name = field.name();
        const auto value = field.value();
        if (name.empty() || name.front() == ':' || !detail::is_valid_binary_field_name(name) ||
            !is_valid_http_header_value(value)) {
            return http_request_trailer_error::invalid_field;
        }
        if (!budget.add_field(name, value)) {
            return http_request_trailer_error::section_too_large;
        }
        if (!detail::is_forbidden_http_request_trailer_name(name)) {
            ++retained_count;
        }
    }
    if (retained_count == 0) {
        budget_ = budget;
        return std::nullopt;
    }
    std::vector<http_header> staged;
    staged.reserve(retained_count);
    for (const auto& field : fields) {
        if (!detail::is_forbidden_http_request_trailer_name(field.name())) {
            staged.emplace_back(field.name(), field.value());
        }
    }
    fields_.reserve(fields_.size() + staged.size());
    for (auto& field : staged) {
        fields_.push_back(std::move(field));
    }
    budget_ = budget;
    return std::nullopt;
}

std::optional<std::string_view> http_request_trailers::field(std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(fields_, name);
}
}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] http_target_origin classify_reg_name(std::string_view host, bool decoded) noexcept {
    if (const auto address = detail::parse_ipv4_address(host)) {
        return http_target_origin::ip_address(http_target_host_kind::ipv4, *address);
    }
    // An absolute domain name names the same host as its relative spelling.
    const bool trailing_dot = host.size() > 1 && host.back() == '.';
    const auto name = trailing_dot ? host.substr(0, host.size() - 1) : host;
    if (!detail::is_valid_dns_domain_name(name) ||
        (trailing_dot && detail::parse_ipv4_address(name).has_value())) {
        return http_target_origin::misdirected();
    }
    return decoded ? http_target_origin::decoded_dns_name(name) : http_target_origin::dns_name(name);
}

[[nodiscard]] http_target_origin certificate_check(std::string_view authority) noexcept {
    // The parsers admitted only valid Host syntax; anything else here is a
    // caller-built request, which fails closed.
    const auto parsed = parse_http_authority(authority);
    if (!parsed.has_value()) {
        return http_target_origin::misdirected();
    }
    const auto host = parsed->host();
    if (host.front() == '[') {
        const auto groups = detail::parse_ipv6_address(host.substr(1, host.size() - 2));
        if (!groups.has_value()) {
            // IPvFuture: no certificate identifies such a host.
            return http_target_origin::misdirected();
        }
        std::array<std::uint8_t, 16> address{};
        for (std::size_t i = 0; i < groups->size(); ++i) {
            address[2 * i] = static_cast<std::uint8_t>((*groups)[i] >> 8);
            address[2 * i + 1] = static_cast<std::uint8_t>((*groups)[i] & 0xFFU);
        }
        return http_target_origin::ip_address(http_target_host_kind::ipv6, address);
    }
    if (host.find('%') == std::string_view::npos) {
        return classify_reg_name(host, false);
    }
    // RFC 3986 §3.2.2 / §6.2.2.2 and RFC 9110 §4.2.3: percent-encoded
    // unreserved octets in reg-name are equivalent to their decoded characters.
    std::array<char, http_target_origin::max_dns_name_size + 1> decoded;
    std::size_t size = 0;
    bool decoded_any = false;
    for (std::size_t i = 0; i < host.size();) {
        if (host[i] == '%') {
            if (const auto byte = http_try_decode_percent_byte(host, i);
                byte.has_value() && detail::is_unreserved_byte(*byte)) {
                if (size == decoded.size()) {
                    return http_target_origin::misdirected();
                }
                decoded[size++] = static_cast<char>(*byte);
                i += 3;
                decoded_any = true;
                continue;
            }
        }
        if (size == decoded.size()) {
            return http_target_origin::misdirected();
        }
        decoded[size++] = host[i++];
    }
    if (!decoded_any) {
        return classify_reg_name(host, false);
    }
    return classify_reg_name(std::string_view(decoded.data(), size), true);
}

}  // namespace

http_target_origin decide_http_target_origin(
    const http_request& request, http_target_origin_connection connection) noexcept {
    if (connection.trusted_gateway_) {
        return http_target_origin::accepted();
    }
    const auto form = request.target_form();
    const auto scheme = request.scheme();
    if (form == http_request_target_form::authority ||
        ((form == http_request_target_form::http2 || form == http_request_target_form::http3) &&
            scheme.empty())) {
        // A CONNECT tunnel target names a destination, not an "https" resource.
        return http_target_origin::accepted();
    }
    const bool https = scheme.empty() ? connection.secured_ : http_ascii_equals_ignore_case(scheme, "https");
    if (!https) {
        return http_target_origin::accepted();
    }
    if (!connection.secured_) {
        return http_target_origin::misdirected();
    }
    const auto authority = !request.authority().empty()
                               ? request.authority()
                               : detail::http_request_access::known_header(request, detail::http_field_kind::host);
    if (authority.empty()) {
        return http_target_origin::accepted();
    }
    return certificate_check(authority);
}

}  // namespace ruvia
