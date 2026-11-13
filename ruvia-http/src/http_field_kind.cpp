#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

#include "http_websocket_permessage_deflate.h"

// Media-type syntax (RFC 9110 section 8.3.1): splitting a field value into
// type/subtype and parameters, comparing parameter values across token and
// quoted-string forms, and validating a Content-Type field. Matching a media
// type against an Accept media-range lives in http_accept_match.cpp.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <vector>

#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/url_encoding.h"

#include "http_connection_fields.h"
namespace ruvia::detail {

struct http_media_type_parts final {
    std::string_view type_;
    std::string_view subtype_;
};

// Both values have already passed parameter syntax validation. Compare their
// decoded bytes without materializing quoted strings.
[[nodiscard]] inline bool http_media_parameter_value_equals(
    std::string_view left, std::string_view right, bool ascii_case_insensitive = false) noexcept {
    if (left.front() != '"' && right.front() != '"') {
        return ascii_case_insensitive ? http_ascii_equals_ignore_case(left, right) : left == right;
    }
    struct cursor final {
        std::string_view value_;
        http_quoted_string_cursor quoted_cursor_;
        std::size_t position_{0};

        explicit cursor(std::string_view value) noexcept
            : value_(value),
              quoted_cursor_(value) {}

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (value_.front() == '"') {
                return quoted_cursor_.next(out);
            }
            if (position_ == value_.size()) {
                return false;
            }
            out = static_cast<unsigned char>(value_[position_++]);
            return true;
        }
    };
    cursor lhs(left);
    cursor rhs(right);
    while (true) {
        unsigned char lhs_char = 0;
        unsigned char rhs_char = 0;
        const bool has_left = lhs.next(lhs_char);
        const bool has_right = rhs.next(rhs_char);
        if (has_left != has_right) {
            return false;
        }
        if (!has_left) {
            return true;
        }
        if (ascii_case_insensitive) {
            lhs_char = http_ascii_to_lower(lhs_char);
            rhs_char = http_ascii_to_lower(rhs_char);
        }
        if (lhs_char != rhs_char) {
            return false;
        }
    }
}

// Parameters are cached on the stack. A media type or media range with more
// than max_parameters parameters is invalid, which bounds the per-member
// duplicate-name check. Views stay valid only while the parsed field is
// unchanged; parsing and matching never allocate.
struct http_media_type_parameters final {
    static constexpr std::size_t max_parameters = 64;

    struct parameter final {
        const char* name_data_;
        std::size_t name_size_;
        const char* value_data_;
        std::size_t value_size_;

        [[nodiscard]] std::string_view name() const noexcept {
            return {name_data_, name_size_};
        }

        [[nodiscard]] std::string_view value() const noexcept {
            return {value_data_, value_size_};
        }
    };
    // Only the first size_ slots are initialized and read. Plain pointer/length
    // slots avoid constructing string_views for unused slots.
    std::array<parameter, max_parameters> values_;
    std::size_t size_{0};

    template <typename visitor_type>
    [[nodiscard]] bool visit(visitor_type&& visitor) const {
        for (std::size_t i = 0; i < size_; ++i) {
            if (!visitor(values_[i].name(), values_[i].value())) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (http_ascii_equals_ignore_case(values_[i].name(), name)) {
                return values_[i].value();
            }
        }
        return std::nullopt;
    }

    // A quality output selects Accept semantics: q is parsed as a weight,
    // including an invalid weight's zero quality, rather than a media parameter.
    [[nodiscard]] bool parse(std::string_view value, int* quality = nullptr) noexcept {
        size_ = 0;
        if (quality != nullptr) {
            *quality = 1000;
        }
        return http_all_parameters(value, [&](std::string_view part) noexcept {
            if (part.empty()) {
                return true;
            }
            const auto equals = part.find('=');
            if (equals == std::string_view::npos || size_ == values_.size()) {
                return false;
            }
            const auto name = part.substr(0, equals);
            const auto parameter_value = part.substr(equals + 1);
            if (!is_valid_http_header_name(name) || parameter_value.empty() || parameter_value != http_trim_ows(parameter_value)) {
                return false;
            }
            if (find(name).has_value()) {
                return false;
            }
            if (quality != nullptr && http_ascii_equals_ignore_case(name, "q")) {
                const auto parsed = http_parse_quality_value(parameter_value);
                *quality = parsed < 0 ? 0 : parsed;
            } else if (!is_http_token_or_quoted_string(parameter_value)) {
                return false;
            }
            values_[size_++] = {name.data(), name.size(), parameter_value.data(), parameter_value.size()};
            return true;
        });
    }

    [[nodiscard]] bool contains(std::string_view name, std::string_view value,
        const http_media_type_parts& parts) const noexcept {
        const auto parameter_value = find(name);
        if (!parameter_value.has_value()) {
            return false;
        }
        // RFC 3676 section 4 defines these case-insensitive values only for text/plain.
        const bool ascii_case_insensitive = http_ascii_equals_ignore_case(name, "charset") ||
                                            ((http_ascii_equals_ignore_case(name, "format") ||
                                                 http_ascii_equals_ignore_case(name, "delsp")) &&
                                                http_ascii_equals_ignore_case(parts.type_, "text") &&
                                                http_ascii_equals_ignore_case(parts.subtype_, "plain"));
        return http_media_parameter_value_equals(*parameter_value, value, ascii_case_insensitive);
    }
};

[[nodiscard]] inline bool http_parse_media_type_parts(
    std::string_view value, bool allow_wildcard, http_media_type_parts& parts) noexcept {
    value = http_header_token_before_parameters(value);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }

    parts.type_ = value.substr(0, slash);
    parts.subtype_ = value.substr(slash + 1);
    const bool type_wildcard = parts.type_ == "*";
    const bool subtype_wildcard = parts.subtype_ == "*";
    if ((type_wildcard || subtype_wildcard) && !allow_wildcard) {
        return false;
    }
    if (type_wildcard && !subtype_wildcard) {
        return false;
    }
    return (type_wildcard || is_valid_http_header_name(parts.type_)) &&
           (subtype_wildcard || is_valid_http_header_name(parts.subtype_));
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type_parts(value_type&&, bool, http_media_type_parts&) = delete;

[[nodiscard]] inline bool http_parse_media_type(std::string_view value, http_media_type_parts& parts,
    http_media_type_parameters& parameters) noexcept {
    return http_parse_media_type_parts(value, false, parts) && parameters.parse(value);
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type(value_type&&, http_media_type_parts&, http_media_type_parameters&) = delete;

[[nodiscard]] inline bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    http_media_type_parts parts;
    http_media_type_parameters parameters;
    return http_parse_media_type(value, parts, parameters);
}

template <http_temporary_owning_char_string value_type>
bool is_valid_http_content_type_field_value(value_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

struct http_field_definition final {
    std::string_view name_;
    http_field_kind kind_;
    std::uint32_t response_bit_;
};

// One spelling authority for both directions. Length-indexed ranges keep
// classification bounded to the names of the same size without allocation.
constexpr http_field_definition http_field_definitions[] = {
    {"te", http_field_kind::te, 0},
    {"date", http_field_kind::other, response_header_date},
    {"etag", http_field_kind::other, response_header_etag},
    {"host", http_field_kind::host, 0},
    {"vary", http_field_kind::other, response_header_vary},
    {"allow", http_field_kind::other, response_header_allow},
    {"range", http_field_kind::range, 0},
    {"accept", http_field_kind::accept, 0},
    {"cookie", http_field_kind::cookie, 0},
    {"expect", http_field_kind::expect, 0},
    {"origin", http_field_kind::origin, 0},
    {"server", http_field_kind::other, response_header_server},
    {"trailer", http_field_kind::trailer, response_header_trailer},
    {"upgrade", http_field_kind::upgrade, response_header_upgrade},
    {"if-match", http_field_kind::if_match, 0},
    {"if-range", http_field_kind::if_range, 0},
    {"location", http_field_kind::other, response_header_location},
    {"forwarded", http_field_kind::forwarded, 0},
    {"connection", http_field_kind::connection, response_header_connection},
    {"keep-alive", http_field_kind::other, response_header_keep_alive},
    {"set-cookie", http_field_kind::other, response_header_set_cookie},
    {"user-agent", http_field_kind::user_agent, 0},
    {"content-type", http_field_kind::content_type, response_header_content_type},
    {"accept-ranges", http_field_kind::other, response_header_accept_ranges},
    {"authorization", http_field_kind::authorization, 0},
    {"cache-control", http_field_kind::other, response_header_cache_control},
    {"content-range", http_field_kind::other, response_header_content_range},
    {"if-none-match", http_field_kind::if_none_match, 0},
    {"last-modified", http_field_kind::other, response_header_last_modified},
    {"content-length", http_field_kind::content_length, response_header_content_length},
    {"accept-encoding", http_field_kind::accept_encoding, 0},
    {"x-forwarded-for", http_field_kind::x_forwarded_for, 0},
    {"content-encoding", http_field_kind::content_encoding, response_header_content_encoding},
    {"proxy-connection", http_field_kind::other, response_header_proxy_connection},
    {"if-modified-since", http_field_kind::if_modified_since, 0},
    {"sec-websocket-key", http_field_kind::sec_websocket_key, 0},
    {"transfer-encoding", http_field_kind::transfer_encoding, response_header_transfer_encoding},
    {"x-forwarded-proto", http_field_kind::x_forwarded_proto, 0},
    {"if-unmodified-since", http_field_kind::if_unmodified_since, 0},
    {"proxy-authorization", http_field_kind::proxy_authorization, 0},
    {"sec-websocket-version", http_field_kind::sec_websocket_version, 0},
    {"access-control-max-age", http_field_kind::other, response_header_access_control_max_age},
    {"sec-websocket-protocol", http_field_kind::sec_websocket_protocol, 0},
    {"sec-websocket-extensions", http_field_kind::sec_websocket_extensions, 0},
    {"access-control-allow-origin", http_field_kind::other, response_header_access_control_allow_origin},
    {"access-control-allow-headers", http_field_kind::other, response_header_access_control_allow_headers},
    {"access-control-allow-methods", http_field_kind::other, response_header_access_control_allow_methods},
    {"access-control-expose-headers", http_field_kind::other, response_header_access_control_expose_headers},
    {"access-control-request-method", http_field_kind::access_control_request_method, 0},
    {"access-control-request-headers", http_field_kind::access_control_request_headers, 0},
    {"access-control-allow-credentials", http_field_kind::other, response_header_access_control_allow_credentials},
};

static_assert([] {
    for (std::size_t i = 1; i < std::size(http_field_definitions); ++i) {
        if (http_field_definitions[i - 1].name_.size() > http_field_definitions[i].name_.size()) {
            return false;
        }
    }
    return true;
}());

constexpr auto http_field_length_offsets = [] {
    std::array<std::size_t, http_field_definitions[std::size(http_field_definitions) - 1].name_.size() + 2> offsets{};
    std::size_t index = 0;
    for (std::size_t length = 0; length < offsets.size(); ++length) {
        while (index < std::size(http_field_definitions) && http_field_definitions[index].name_.size() < length) {
            ++index;
        }
        offsets[length] = index;
    }
    return offsets;
}();

constexpr auto response_field_names = [] {
    std::array<std::string_view, std::bit_width(response_header_proxy_connection)> names{};
    for (const auto& field : http_field_definitions) {
        if (field.response_bit_ != 0) {
            names[std::countr_zero(field.response_bit_)] = field.name_;
        }
    }
    return names;
}();

}  // namespace

http_field_classification classify_http_field_name(std::string_view name) noexcept {
    if (name.empty() || name.size() >= http_field_length_offsets.size() - 1) {
        return {};
    }
    const auto first = http_ascii_to_lower(static_cast<unsigned char>(name.front()));
    const auto end = http_field_length_offsets[name.size() + 1];
    for (auto index = http_field_length_offsets[name.size()]; index < end; ++index) {
        const auto& field = http_field_definitions[index];
        if (first == static_cast<unsigned char>(field.name_.front()) && http_ascii_equals_ignore_case(name, field.name_)) {
            return {field.kind_, field.response_bit_};
        }
    }
    return {};
}

http_field_kind classify_http_field(std::string_view name) noexcept {
    return classify_http_field_name(name).kind_;
}

std::uint32_t classify_response_header_name(std::string_view name) noexcept {
    return classify_http_field_name(name).response_bit_;
}

std::string_view response_known_header_lowercase_name(std::uint32_t bit) noexcept {
    if (!std::has_single_bit(bit)) {
        return {};
    }
    const auto index = static_cast<std::size_t>(std::countr_zero(bit));
    return index < response_field_names.size() ? response_field_names[index] : std::string_view{};
}

}  // namespace ruvia::detail

// The method vocabulary: recognising the standard tokens, spelling them back,
// validating an unknown one, and the two properties -- safe and idempotent --
// that decide what a recipient may do with a request (RFC 9110 section 9.2).

namespace ruvia {

http_known_method classify_http_method(std::string_view method) noexcept {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return http_known_method::get;
            }
            if (method == "PUT") {
                return http_known_method::put;
            }
            break;
        case 4:
            if (method == "POST") {
                return http_known_method::post;
            }
            if (method == "HEAD") {
                return http_known_method::head;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return http_known_method::patch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return http_known_method::delete_value;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return http_known_method::options;
            }
            if (method == "CONNECT") {
                return http_known_method::connect;
            }
            break;
        default:
            break;
    }
    return http_known_method::unknown;
}

std::string_view known_http_method_token(http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return "GET";
        case http_known_method::post:
            return "POST";
        case http_known_method::put:
            return "PUT";
        case http_known_method::delete_value:
            return "DELETE";
        case http_known_method::patch:
            return "PATCH";
        case http_known_method::head:
            return "HEAD";
        case http_known_method::options:
            return "OPTIONS";
        case http_known_method::connect:
            return "CONNECT";
        case http_known_method::unknown:
        default:
            return {};
    }
}

bool is_valid_http_method_token(std::string_view method) noexcept {
    return is_valid_http_header_name(method);
}

bool is_http_method_safe(std::string_view method) noexcept {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

bool is_http_method_idempotent(std::string_view method) noexcept {
    return is_http_method_safe(method) || method == "PUT" || method == "DELETE";
}

}  // namespace ruvia

namespace ruvia {

bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 2U:
            return http_ascii_equals_ignore_case(field_name, "TE");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer") ||
                   http_ascii_equals_ignore_case(field_name, "Upgrade");
        case 10U:
            return http_ascii_equals_ignore_case(field_name, "Connection") ||
                   http_ascii_equals_ignore_case(field_name, "Keep-Alive");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Connection");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 18U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Authenticate");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Authorization");
        default:
            return false;
    }
}

bool is_http_standard_connection_option(std::string_view option) noexcept {
    return (option.size() == 5U && http_ascii_equals_ignore_case(option, "close")) ||
           is_http_hop_by_hop_field_name(option);
}

void http_connection_nominated_fields::update(std::string_view connection_field_value) {
    detail::http_visit_comma_separated_tokens(connection_field_value, [&](std::string_view token) {
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        if (http_ascii_equals_ignore_case(token, "close")) {
            has_close_ = true;
        } else if (http_ascii_equals_ignore_case(token, "keep-alive")) {
            has_keep_alive_ = true;
        } else if (http_ascii_equals_ignore_case(token, "Upgrade")) {
            has_upgrade_ = true;
        } else if (http_ascii_equals_ignore_case(token, "TE")) {
            has_te_ = true;
        }
        if (contains(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        tokens_.push_back(std::move(lower));
        return true;
    });
}

void http_connection_nominated_fields::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Connection")) {
            update(field.value());
        }
    }
}

bool http_connection_nominated_fields::contains(std::string_view field_name) const noexcept {
    return std::ranges::any_of(tokens_, [field_name](std::string_view token) noexcept {
        return http_ascii_equals_ignore_case(token, field_name);
    });
}

std::vector<std::string> http_connection_nominated_fields::non_standard_options() const {
    std::vector<std::string> result;
    for (const auto& token : tokens_) {
        if (!is_http_standard_connection_option(token)) {
            result.emplace_back(token);
        }
    }
    return result;
}

std::string http_connection_nominated_fields::rebuild_connection_field(
    bool retain_upgrade, bool include_non_standard) const {
    std::string output;
    if (retain_upgrade && has_upgrade_) {
        output.append("Upgrade");
    }
    if (include_non_standard) {
        for (const auto& token : tokens_) {
            if (is_http_standard_connection_option(token)) {
                continue;
            }
            if (!output.empty()) {
                output.append(", ");
            }
            output.append(token);
        }
    }
    return output;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value) {
    http_connection_nominated_fields nominated;
    nominated.update(connection_field_value);
    return nominated;
}
bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade) noexcept {
    if (retain_upgrade && nominated.has_upgrade() &&
        http_ascii_equals_ignore_case(field_name, "Upgrade")) {
        return false;
    }
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_http_hop_by_hop_trailer_field(
    std::string_view field_name, const http_connection_nominated_fields& nominated) noexcept {
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_http_request_trailer_name(field_name);
}

bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_response_trailer_name(field_name);
}
}  // namespace ruvia

namespace ruvia {

http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept {
    return http_conditional_headers{
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_unmodified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_unmodified_since),
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_modified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_modified_since),
        detail::http_request_access::known_header(request, detail::http_field_kind::range),
        detail::http_request_access::known_header(request, detail::http_field_kind::if_range),
        detail::http_request_access::has_known_header(request, detail::http_field_kind::if_range)};
}

void http_etag_field_condition::update(
    std::string_view value, std::string_view expected, bool strong) noexcept {
    const bool already_present = present_;
    present_ = true;
    const auto trimmed = http_trim_ows(value);
    if (trimmed == "*") {
        wildcard_ = true;
        if (already_present) {
            valid_ = false;
        }
        return;
    }
    if (wildcard_) {
        valid_ = false;
    }
    const auto result_value = detail::http_parse_etag_list_matches(value, expected, strong);
    valid_ = valid_ && result_value.valid_;
    matched_ = matched_ || result_value.matched_;
}

http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept {
    http_etag_preconditions result;
    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them line by line and
    // retain whole-list validity without allocating a joined string.
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_match, [&result, etag](std::string_view value) noexcept {
            result.if_match_.update(value, etag, true);
            return true;
        });
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_none_match, [&result, etag](std::string_view value) noexcept {
            result.if_none_match_.update(value, etag, false);
            return true;
        });
    return result;
}

bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return date.has_value() && modified_seconds <= *date;
}

bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return !date.has_value() || modified_seconds <= *date;
}

bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept {
    if (header_value.empty()) {
        return false;
    }
    const auto value = http_trim_ows(header_value);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::http_strong_etag_equals(value, etag);
    }
    if (!date_validator_strong || !modified_seconds) {
        return false;
    }
    const auto date = parse_http_date(value);
    return date.has_value() && *modified_seconds == *date;
}

bool is_valid_http_entity_tag(std::string_view value) noexcept {
    if (value.starts_with("W/")) {
        value.remove_prefix(2);
    }
    if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t i = 1U; i + 1U < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '"' || (byte != 0x21U && !(byte >= 0x23U && byte <= 0x7eU) && byte < 0x80U)) {
            return false;
        }
    }
    return true;
}

bool is_weak_http_entity_tag(std::string_view value) noexcept {
    return value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool is_strong_http_entity_tag(std::string_view value) noexcept {
    return !value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_strong_http_entity_tag(left) && is_strong_http_entity_tag(right) && left == right;
}

bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_valid_http_entity_tag(left) && is_valid_http_entity_tag(right) &&
           detail::http_weak_etag_equals(left, right);
}

std::string weaken_http_etag(std::string_view etag) {
    etag = http_trim_ows(etag);
    if (is_strong_http_entity_tag(etag)) {
        std::string weakened;
        weakened.reserve(2U + etag.size());
        weakened.append("W/");
        weakened.append(etag);
        return weakened;
    }
    return std::string(etag);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

enum class http_accept_token_match : int {
    none = -1,
    wildcard = 0,
    prefix = 1,
    exact = 2,
};

// RFC 4647 section 3.3.1 basic filtering: a language range matches a tag that
// equals it or extends it at a subtag boundary, so "en" matches "en-US" but
// never "english". Only Accept-Language uses this; charsets match exactly or by
// wildcard.
[[nodiscard]] http_accept_token_match http_accept_token_matches(
    std::string_view range, std::string_view offered, bool prefix_matching) noexcept {
    if (range == "*") {
        return http_accept_token_match::wildcard;
    }
    if (http_ascii_equals_ignore_case(range, offered)) {
        return http_accept_token_match::exact;
    }
    if (prefix_matching && offered.size() > range.size() && offered[range.size()] == '-' &&
        http_ascii_equals_ignore_case(range, offered.substr(0, range.size()))) {
        return http_accept_token_match::prefix;
    }
    return http_accept_token_match::none;
}

void http_accumulate_token_acceptance(std::string_view accept, std::string_view offered,
    bool prefix_matching, int& best_specificity, int& best_quality) noexcept {
    http_visit_comma_separated_quoted(accept,
        [offered, prefix_matching, &best_specificity, &best_quality](std::string_view item) noexcept {
            const auto range = http_header_token_before_parameters(item);
            const auto match = http_accept_token_matches(range, offered, prefix_matching);
            if (match == http_accept_token_match::none) {
                return true;
            }
            const auto specificity = prefix_matching && match != http_accept_token_match::wildcard
                                         ? static_cast<int>(std::min(range.size(),
                                               static_cast<std::size_t>((std::numeric_limits<int>::max)())))
                                         : static_cast<int>(match);
            const auto quality = http_weight_parameter(item);
            if (specificity > best_specificity ||
                (specificity == best_specificity && quality > best_quality)) {
                best_specificity = specificity;
                best_quality = quality;
            }
            return true;
        });
}

[[nodiscard]] int http_media_range_match_specificity(std::string_view range,
    const http_media_type_parts& offered_parts, const http_media_type_parameters& offered_parameters,
    std::size_t& parameter_count, int& quality) noexcept {
    http_media_type_parts range_parts;
    if (!http_parse_media_type_parts(range, true, range_parts)) {
        return -1;
    }
    if (range_parts.type_ != "*" && !http_ascii_equals_ignore_case(range_parts.type_, offered_parts.type_)) {
        return -1;
    }
    if (range_parts.subtype_ != "*" && !http_ascii_equals_ignore_case(range_parts.subtype_, offered_parts.subtype_)) {
        return -1;
    }
    http_media_type_parameters parameters;
    if (!parameters.parse(range, &quality)) {
        return -1;
    }
    parameter_count = 0;
    if (!parameters.visit([&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "q")) {
                return true;
            }
            if (!offered_parameters.contains(name, value, offered_parts)) {
                return false;
            }
            ++parameter_count;
            return true;
        })) {
        return -1;
    }
    return range_parts.type_ == "*" ? 0 : (range_parts.subtype_ == "*" ? 1 : 2);
}

void http_accumulate_media_type_acceptance(std::string_view accept, std::string_view offered,
    int& best_specificity, std::size_t& best_parameter_count, int& best_quality) noexcept {
    http_media_type_parts offered_parts;
    http_media_type_parameters offered_parameters;
    if (!http_parse_media_type(offered, offered_parts, offered_parameters)) {
        return;
    }
    http_visit_comma_separated_quoted(accept,
        [offered_parts, &offered_parameters, &best_specificity, &best_parameter_count, &best_quality](std::string_view item) noexcept {
            int quality = 1000;
            std::size_t parameter_count = 0;
            const auto specificity = http_media_range_match_specificity(item, offered_parts, offered_parameters, parameter_count, quality);
            if (specificity >= 0) {
                if (specificity > best_specificity ||
                    (specificity == best_specificity &&
                        (parameter_count > best_parameter_count ||
                            (parameter_count == best_parameter_count && quality > best_quality)))) {
                    best_specificity = specificity;
                    best_parameter_count = parameter_count;
                    best_quality = quality;
                }
            }
            return true;
        });
}

}  // namespace
}  // namespace ruvia::detail
namespace ruvia {

std::string_view http_media_type_only(borrowed_text value) noexcept {
    return detail::http_header_token_before_parameters(value.view());
}

bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    return detail::is_valid_http_content_type_field_value(value);
}

void http_accept_match::update_media_type(std::string_view field, std::string_view offered) noexcept {
    detail::http_accumulate_media_type_acceptance(field, offered, specificity_, parameter_specificity_, quality_);
}

void http_accept_match::update_token(std::string_view field, std::string_view offered,
    http_accept_token_match_mode mode) noexcept {
    detail::http_accumulate_token_acceptance(
        field, offered, mode == http_accept_token_match_mode::language_prefix, specificity_, quality_);
}

}  // namespace ruvia

namespace ruvia {
namespace {
using item_type = detail::http_structured_item;

template <typename input_type>
std::variant<std::monostate, http_priority_error> parse_priority_members(detail::http_structured_parser<input_type> parser, http_priority_fields& fields_value) noexcept {
    parser.spaces();
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return http_priority_error::invalid_syntax;
        }
        item_type item{.kind_ = item_type::kind_type::boolean, .boolean_ = true};
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return http_priority_error::invalid_syntax;
            }
        } else if (!parser.parameters()) {
            return http_priority_error::invalid_syntax;
        }
        if (key == "u") {
            fields_value.urgency_ = item.kind_ == item_type::kind_type::integer
                                        ? http_priority_urgency::try_from_value(item.integer_)
                                        : std::nullopt;
        }
        if (key == "i") {
            fields_value.incremental_ = item.kind_ == item_type::kind_type::boolean ? std::optional(item.boolean_) : std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return http_priority_error::invalid_syntax;
        }
        parser.ows();
        if (parser.empty()) {
            return http_priority_error::invalid_syntax;
        }
    }
    return {};
}
}  // namespace

std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(detail::http_structured_parser{detail::http_structured_text_input{value}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
namespace detail {
std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(
    http_header_range_view headers) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(
            http_structured_parser{
                http_structured_header_input{headers.begin(), headers.end(), "priority"}},
            fields);
        parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
}  // namespace detail
std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept {
    std::array<char, 12> bytes_value{};
    std::size_t count = 0;
    if (fields_value.urgency_) {
        bytes_value[count++] = 'u';
        bytes_value[count++] = '=';
        bytes_value[count++] = static_cast<char>('0' + fields_value.urgency_->value());
    }
    if (fields_value.incremental_) {
        if (*fields_value.incremental_) {
            if (count) {
                bytes_value[count++] = ',';
                bytes_value[count++] = ' ';
            }
            bytes_value[count++] = 'i';
        } else {
            if (count) {
                bytes_value[count++] = ',';
            }
            bytes_value[count++] = 'i';
            bytes_value[count++] = '=';
            bytes_value[count++] = '?';
            bytes_value[count++] = '0';
        }
    }
    if (output.size() < count) {
        return http_priority_error::output_too_small;
    }
    std::copy_n(bytes_value.begin(), count, output.begin());
    return count;
}
std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept {
    if (payload_value.size() < 4) {
        return http_priority_error::invalid_frame;
    }
    const auto* id_bytes = reinterpret_cast<const unsigned char*>(payload_value.data());
    const auto id = ((static_cast<std::uint32_t>(id_bytes[0]) & 0x7fU) << 24) |
                    (static_cast<std::uint32_t>(id_bytes[1]) << 16) |
                    (static_cast<std::uint32_t>(id_bytes[2]) << 8) | static_cast<std::uint32_t>(id_bytes[3]);
    if (!id) {
        return http_priority_error::invalid_value;
    }
    auto fields_value = parse_http_priority({payload_value.data() + 4, payload_value.size() - 4});
    if ((fields_value.index() != 0)) {
        return std::get<1>(fields_value);
    }
    return http_priority_update{id, (id & 1U) == 0, std::get<0>(fields_value)};
}
std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t id, http_priority_fields fields_value) noexcept {
    if (!id || id > 0x7fffffffU) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 12> value{};
    const auto length = encode_http_priority(value, fields_value);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    if (output.size() < 13 + std::get<0>(length)) {
        return http_priority_error::output_too_small;
    }
    (void)encode_http2_frame_header(output.first(http2_frame_header_bytes),
        static_cast<std::uint32_t>(4 + std::get<0>(length)), http2_frame_type::priority_update, 0, 0);
    for (std::size_t index = 0; index < 4; ++index) {
        output[http2_frame_header_bytes + index] = static_cast<char>((id >> (24 - 8 * index)) & 0xffU);
    }
    std::copy_n(value.begin(), std::get<0>(length), output.begin() + 13);
    return 13 + std::get<0>(length);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_valid_http_cors_request_header_names(std::string_view value) noexcept {
    http_field_name_list names(value);
    bool saw_name = false;
    while (names.next()) {
        saw_name = true;
    }
    // Access-Control-Request-Headers requires at least one field name, unlike
    // the general #field-name grammar.
    return saw_name && names.valid();
}

// Host is a once-only field (RFC 9112 section 3.2) without a cached singleton
// bit; its kind index is below 32 like every singleton kind.
[[nodiscard]] constexpr std::uint32_t once_only_bit(http_field_kind kind) noexcept {
    return kind == http_field_kind::host ? 1U << static_cast<std::uint32_t>(kind)
                                         : singleton_request_header_bit(kind);
}

// RFC 9110 section 5.6.6 parameter: no whitespace around '='.
[[nodiscard]] bool is_exact_http_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(part.substr(0, equals)) &&
           is_http_token_or_quoted_string(part.substr(equals + 1));
}

[[nodiscard]] bool http_expectation_item(std::string_view item) noexcept {
    item = http_trim_ows(item);
    if (item.empty()) {
        return false;
    }

    const auto parameters_start = http_find_unquoted_delimiter(item, 0, ';');
    const auto leading = http_trim_ows(item.substr(0, parameters_start));
    if (leading.find('=') == std::string_view::npos) {
        return parameters_start == item.size() && is_valid_http_header_name(leading);
    }
    // RFC 9110 section 5.6.6: parameters = *( OWS ";" OWS [ parameter ] ).
    return is_exact_http_parameter(leading) && http_all_parameters(item, [](std::string_view parameter) noexcept {
        return parameter.empty() || is_exact_http_parameter(parameter);
    });
}

// Recipient-side Expect grammar. Empty list members are ignored, but every
// non-empty member still has to be a syntactically valid expectation before
// product policy decides whether unsupported extensions are rejected with 417
// or ignored.
[[nodiscard]] bool is_valid_received_http_expect_field_value(std::string_view value) noexcept {
    return http_visit_list_members(value, http_field_list_role::recipient, [](std::string_view item) noexcept {
        return http_expectation_item(item) ? http_field_list_parse_status::ok : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] bool is_valid_te(
    std::string_view value, http_field_list_role role, request_field_framing framing) noexcept {
    if (framing == request_field_framing::binary) {
        return is_valid_binary_http_te_field_value(value, role);
    }
    return is_valid_http_te_field_value(value, role);
}

[[nodiscard]] bool is_valid_value(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_framing framing, http_request_field_source source) noexcept {
    // RFC 9110 sections 2.2 and 5.6.1: forwarding is not generation. Reuse the
    // recipient value rules only for end-to-end application metadata. Ownership
    // and singleton checks below still use the actual sender role.
    const auto application_role = source == http_request_field_source::forwarded
                                      ? http_field_list_role::recipient
                                      : role;
    switch (kind) {
        case http_field_kind::origin:
            return application_role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case http_field_kind::access_control_request_method:
            return application_role == http_field_list_role::recipient || is_valid_http_method_token(value);
        case http_field_kind::access_control_request_headers:
            return application_role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case http_field_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return application_role == http_field_list_role::recipient ||
                   is_valid_http_content_type_field_value(value);
        case http_field_kind::content_encoding:
            // Admission leaves received metadata to the decoder. Forwarding
            // validates the recipient list grammar, not arbitrary nonempty text.
            return role == http_field_list_role::recipient ||
                   is_valid_http_content_encoding_field_value(value, application_role);
        case http_field_kind::trailer:
            // The declaration neither frames content nor authorizes actual trailers.
            return role == http_field_list_role::recipient ||
                   is_valid_http_request_trailer_field_value(value, role);
        case http_field_kind::te:
            return is_valid_te(value, role, framing);
        case http_field_kind::expect:
            // Binary field semantics feed the existing 417/ignore policy, not malformed-message rejection.
            return framing == request_field_framing::binary ||
                   is_valid_received_http_expect_field_value(value);
        case http_field_kind::host:
            return is_valid_host_header(value);
        default:
            return true;
    }
}

}  // namespace

bool request_field_state::seen(http_field_kind kind) const noexcept {
    return (seen_ & once_only_bit(kind)) != 0;
}

request_field_status validate_request_field(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return request_field_status::invalid;
    }
    if (role == http_field_list_role::sender &&
        (kind == http_field_kind::expect || kind == http_field_kind::content_length)) {
        return request_field_status::managed_by_sender;
    }
    if (!is_valid_value(kind, value, role, state.framing_, source)) {
        return request_field_status::invalid;
    }
    // Invalid date conditions, including repeated field lines, are ignored
    // rather than rejecting the request (RFC 9110 sections 13.1.3 and 13.1.4).
    // A repeated User-Agent violates only the sender rule of RFC 9110 section
    // 5.3 and controls no framing, routing or access, so recipients keep its
    // lines. Senders still cannot generate these singleton fields more than once.
    if (role == http_field_list_role::recipient &&
        (kind == http_field_kind::if_modified_since ||
            kind == http_field_kind::if_unmodified_since || kind == http_field_kind::user_agent)) {
        return request_field_status::ok;
    }
    if (const auto bit = once_only_bit(kind); bit != 0) {
        if ((state.seen_ & bit) != 0) {
            return request_field_status::invalid;
        }
        state.seen_ |= bit;
    }
    return request_field_status::ok;
}

bool is_protocol_managed_request_field(http_field_kind kind, std::string_view name) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::trailer:
        case http_field_kind::expect:
        case http_field_kind::connection:
        case http_field_kind::te:
        case http_field_kind::upgrade:
            return true;
        case http_field_kind::other:
            // Keep-Alive and Proxy-Connection have no request identity.
            return is_forbidden_http_binary_connection_field(name);
        default:
            return false;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_http_protocol_managed_request_field(std::string_view name) noexcept {
    return detail::is_protocol_managed_request_field(detail::classify_http_field(name), name);
}

bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    const auto kind = detail::classify_http_field(name);
    if (detail::is_protocol_managed_request_field(kind, name)) {
        return false;
    }
    // TE 是唯一按 HTTP 版本区分的请求字段语义，已由托管字段边界排除。
    // 每次只检查一个字段；重复检测由调用方持有的字段集合负责。
    detail::request_field_state state(detail::request_field_framing::http1);
    return detail::validate_request_field(kind, value, detail::http_field_list_role::sender, state) ==
           detail::request_field_status::ok;
}

std::optional<std::string_view> http_field_name_list::next() noexcept {
    while (!finished_) {
        const auto separator = remaining_.find(',');
        const auto member = remaining_.substr(0, separator);
        if (separator == std::string_view::npos) {
            finished_ = true;
        } else {
            remaining_.remove_prefix(separator + 1);
        }
        const auto name = http_trim_ows(member);
        if (name.empty()) {
            if (finished_) {
                valid_ = true;
            }
            continue;
        }
        if (!is_valid_http_header_name(name)) {
            finished_ = true;
            valid_ = false;
            return std::nullopt;
        }
        if (finished_) {
            valid_ = true;
        }
        return name;
    }
    return std::nullopt;
}

void http_request_expectations::parse_field(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view member) noexcept {
        if (http_ascii_equals_ignore_case(member, detail::http_continue_expectation_token)) {
            flags_ |= continue_value;
        } else {
            flags_ |= unsupported;
        }
        return true;
    });
}
}  // namespace ruvia

namespace ruvia::detail {

bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept {
    return http_field_value_prefix_size({data, size}) == size;
}

bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_type:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::if_match:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_none_match:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::range:
        case http_field_kind::upgrade:
        case http_field_kind::authorization:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return true;
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::access_control_request_method:
        case http_field_kind::origin:
        case http_field_kind::user_agent:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
            break;
    }

    switch (name.size()) {
        case 10:
            return http_ascii_equals_ignore_case(name, "Keep-Alive") ||
                   http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") ||
                   http_ascii_equals_ignore_case(name, "Content-Range");
        case 16:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 18:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        default:
            return false;
    }
}

bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_http_field(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == http_field_kind::access_control_request_headers ||
           kind == http_field_kind::access_control_request_method ||
           kind == http_field_kind::origin ||
           http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name);
        known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classify_http_field(name))) {
        return true;
    }

    switch (name.size()) {
        case 3:
            // Response control data (RFC 9110 §7.4).
            return http_ascii_equals_ignore_case(name, "Age");
        case 6:
            return http_ascii_equals_ignore_case(name, "Pragma");
        case 7:
            return http_ascii_equals_ignore_case(name, "Expires") ||
                   http_ascii_equals_ignore_case(name, "Warning");
        case 11:
            return http_ascii_equals_ignore_case(name, "Retry-After");
        case 15:
            return http_ascii_equals_ignore_case(name, "X-Frame-Options") ||
                   http_ascii_equals_ignore_case(name, "Referrer-Policy") ||
                   http_ascii_equals_ignore_case(name, "Clear-Site-Data");
        case 16:
            return http_ascii_equals_ignore_case(name, "X-XSS-Protection") ||
                   http_ascii_equals_ignore_case(name, "WWW-Authenticate");
        case 18:
            return http_ascii_equals_ignore_case(name, "Permissions-Policy");
        case 19:
            return http_ascii_equals_ignore_case(name, "Content-Disposition");
        case 22:
            return http_ascii_equals_ignore_case(name, "X-Content-Type-Options");
        case 23:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy");
        case 25:
            return http_ascii_equals_ignore_case(name, "Strict-Transport-Security");
        case 35:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy-Report-Only");
        default:
            return false;
    }
}

bool is_forbidden_http_binary_response_field(std::string_view name) noexcept {
    return is_forbidden_http_binary_connection_field(name) || http_ascii_equals_ignore_case(name, "te");
}

[[nodiscard]] bool is_valid_http_response_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_response_trailer_name(name); });
}

[[nodiscard]] bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) &&
           is_valid_http_header_value(value);
}

response_field_status validate_response_field(http_field_kind kind, std::string_view value,
    response_field_state& state) noexcept {
    switch (kind) {
        case http_field_kind::content_type:
            if (!is_valid_http_content_type_field_value(value)) {
                return response_field_status::invalid;
            }
            if (state.content_type_seen_) {
                return response_field_status::repeated_singleton;
            }
            state.content_type_seen_ = true;
            break;
        case http_field_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        case http_field_kind::trailer:
            if (!is_valid_http_response_trailer_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        default:
            break;
    }
    return response_field_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    detail::response_field_state state;
    return detail::validate_response_field(detail::classify_http_field(name), value, state) ==
           detail::response_field_status::ok;
}

http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parse_delta_seconds(std::string_view value) noexcept {
    std::uint64_t result_value = 0;
    bool any_digit = false;
    const auto accumulate = [&result_value, &any_digit](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9') {
            return false;
        }
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        result_value = result_value > (maximum - digit) / 10 ? maximum : result_value * 10 + digit;
        any_digit = true;
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor cursor(value);
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (!accumulate(ch)) {
                return std::nullopt;
            }
        }
        if (!cursor.closed() || cursor.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const auto ch : value) {
            if (!accumulate(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    if (!any_digit) {
        return std::nullopt;
    }
    return result_value;
}

[[nodiscard]] bool parse_nominated_field_list_arg(
    std::string_view arg, auto&& add_field) noexcept {
    if (arg.empty()) {
        return false;
    }
    std::array<char, 256> unescaped_buf{};
    std::string_view list_text = arg;
    if (arg.front() == '"') {
        http_quoted_string_cursor cursor(arg);
        std::size_t len = 0;
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (len >= unescaped_buf.size()) {
                return false;
            }
            unescaped_buf[len++] = static_cast<char>(ch);
        }
        if (!cursor.closed() || cursor.consumed() != arg.size()) {
            return false;
        }
        list_text = std::string_view(unescaped_buf.data(), len);
    } else if (!is_valid_http_header_name(arg)) {
        return false;
    }

    http_field_name_list names(list_text);
    bool saw_name = false;
    while (const auto name = names.next()) {
        saw_name = true;
        if (!add_field(*name)) {
            return false;
        }
    }
    return saw_name && names.valid();
}
}  // namespace

void cache_control_field_parser::update(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view token) noexcept {
        const auto eq = token.find('=');
        const bool has_argument = eq != std::string_view::npos;
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);
        switch (name.size()) {
            case 6U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "public")) {
                    value_.set(cache_control_directive::public_value);
                }
                break;
            case 7U:
                if (http_ascii_equals_ignore_case(name, "max-age")) {
                    if (!max_age_seen_) {
                        max_age_seen_ = true;
                        value_.max_age_ = parse_delta_seconds(arg);
                    }
                } else if (http_ascii_equals_ignore_case(name, "private")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::private_value);
                        value_.set(cache_control_directive::private_unqualified);
                        value_.private_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::private_value);
                        if (!value_.is_unqualified_private()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.private_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::private_unqualified);
                                value_.private_fields_.clear();
                            }
                        }
                    }
                }
                break;
            case 8U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-store")) {
                    value_.set(cache_control_directive::no_store);
                } else if (http_ascii_equals_ignore_case(name, "no-cache")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::no_cache);
                        value_.set(cache_control_directive::no_cache_unqualified);
                        value_.no_cache_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::no_cache);
                        if (!value_.is_unqualified_no_cache()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.no_cache_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::no_cache_unqualified);
                                value_.no_cache_fields_.clear();
                            }
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "s-maxage")) {
                    if (!s_max_age_seen_) {
                        s_max_age_seen_ = true;
                        value_.s_max_age_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 9U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "immutable")) {
                    value_.set(cache_control_directive::immutable);
                } else if (http_ascii_equals_ignore_case(name, "max-stale")) {
                    if (!max_stale_seen_) {
                        max_stale_seen_ = true;
                        if (!has_argument) {
                            value_.set(cache_control_directive::max_stale_any);
                        } else {
                            value_.max_stale_ = parse_delta_seconds(arg);
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "min-fresh")) {
                    if (!min_fresh_seen_) {
                        min_fresh_seen_ = true;
                        value_.min_fresh_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 12U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-transform")) {
                    value_.set(cache_control_directive::no_transform);
                }
                break;
            case 14U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "only-if-cached")) {
                    value_.set(cache_control_directive::only_if_cached);
                } else if (http_ascii_equals_ignore_case(name, "stale-if-error")) {
                    if (!stale_if_error_seen_) {
                        stale_if_error_seen_ = true;
                        value_.stale_if_error_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 15U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "must-revalidate")) {
                    value_.set(cache_control_directive::must_revalidate);
                }
                break;
            case 16U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "proxy-revalidate")) {
                    value_.set(cache_control_directive::proxy_revalidate);
                }
                break;
            case 22U:
                if (http_ascii_equals_ignore_case(name, "stale-while-revalidate")) {
                    if (!stale_while_revalidate_seen_) {
                        stale_while_revalidate_seen_ = true;
                        value_.stale_while_revalidate_ = parse_delta_seconds(arg);
                    }
                }
                break;
            default:
                break;
        }
        return true;
    });
}

cache_control parse_cache_control(std::string_view value) noexcept {
    cache_control_field_parser parser;
    parser.update(value);
    return parser.finish();
}

cache_control detail::parse_cache_control_headers(detail::http_header_range_view headers) noexcept {
    cache_control_field_parser parser;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

bool http_cache_control_has_no_transform(std::string_view field_value) noexcept {
    return parse_cache_control(field_value).no_transform();
}

bool cache_control::nominated_field_set::contains(std::string_view field_name) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (http_ascii_equals_ignore_case(at(i), field_name)) {
            return true;
        }
    }
    return false;
}

bool cache_control::nominated_field_set::add_lowercase(std::string_view field_name) noexcept {
    if (field_name.empty() || field_name.size() > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    if (contains(field_name)) {
        return true;
    }
    if (count_ >= max_nominated_fields ||
        field_name.size() > max_nominated_field_bytes - storage_used_) {
        return false;
    }
    offsets_[count_] = storage_used_;
    lengths_[count_] = static_cast<std::uint8_t>(field_name.size());
    for (std::size_t i = 0; i < field_name.size(); ++i) {
        storage_[storage_used_ + i] =
            static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(field_name[i])));
    }
    storage_used_ = static_cast<std::uint16_t>(storage_used_ + field_name.size());
    ++count_;
    return true;
}

bool cache_control::has_private_field(std::string_view field_name) const noexcept {
    return !is_unqualified_private() && private_fields_.contains(field_name);
}

bool cache_control::has_no_cache_field(std::string_view field_name) const noexcept {
    return !is_unqualified_no_cache() && no_cache_fields_.contains(field_name);
}

template <typename input_type>
std::optional<cache_control> parse_targeted_cache_control_from_input(input_type input) noexcept {
    detail::http_structured_parser<input_type> parser{std::move(input)};
    parser.ows();
    if (parser.empty()) {
        return std::nullopt;
    }
    cache_control result;
    bool saw_member = false;
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return std::nullopt;
        }
        saw_member = true;
        detail::http_structured_item item{
            .kind_ = detail::http_structured_item::kind_type::boolean,
            .boolean_ = true,
        };
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return std::nullopt;
            }
        } else if (!parser.parameters()) {
            return std::nullopt;
        }

        const auto apply_bool_directive = [&](cache_control_directive dir) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                if (item.boolean_) {
                    result.set(dir);
                } else {
                    result.clear_directive(dir);
                }
            } else {
                result.clear_directive(dir);
            }
        };
        const auto apply_delta_directive =
            [&](std::optional<std::uint64_t>& target) noexcept {
                if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                    item.integer_ >= 0) {
                    target = static_cast<std::uint64_t>(item.integer_);
                } else {
                    target.reset();
                }
            };
        const auto apply_qualified_directive = [&](cache_control_directive flag_dir,
                                                   cache_control_directive unqual_dir,
                                                   cache_control::nominated_field_set& fields) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                fields.clear();
                if (item.boolean_) {
                    result.set(flag_dir);
                    result.set(unqual_dir);
                } else {
                    result.clear_directive(flag_dir);
                    result.clear_directive(unqual_dir);
                }
            } else if (item.kind_ == detail::http_structured_item::kind_type::string ||
                       item.kind_ == detail::http_structured_item::kind_type::token) {
                fields.clear();
                result.clear_directive(unqual_dir);
                result.set(flag_dir);
                std::array<char, 256> unescaped{};
                std::string_view list_text = item.text_;
                if (item.kind_ == detail::http_structured_item::kind_type::string &&
                    item.has_escapes_) {
                    std::size_t out_len = 0;
                    for (std::size_t i = 0; i < item.text_.size(); ++i) {
                        if (item.text_[i] == '\\' && i + 1U < item.text_.size()) {
                            ++i;
                        }
                        if (out_len >= unescaped.size()) {
                            result.set(unqual_dir);
                            fields.clear();
                            return;
                        }
                        unescaped[out_len++] = item.text_[i];
                    }
                    list_text = std::string_view(unescaped.data(), out_len);
                }
                http_field_name_list names(list_text);
                bool saw_name = false;
                bool ok = true;
                while (const auto field_name = names.next()) {
                    saw_name = true;
                    if (!fields.add_lowercase(*field_name)) {
                        ok = false;
                        break;
                    }
                }
                if (!saw_name || !names.valid() || !ok) {
                    result.set(unqual_dir);
                    fields.clear();
                }
            } else {
                fields.clear();
                result.clear_directive(flag_dir);
                result.clear_directive(unqual_dir);
            }
        };

        if (key == "no-store") {
            apply_bool_directive(cache_control_directive::no_store);
        } else if (key == "no-transform") {
            apply_bool_directive(cache_control_directive::no_transform);
        } else if (key == "must-revalidate") {
            apply_bool_directive(cache_control_directive::must_revalidate);
        } else if (key == "proxy-revalidate") {
            apply_bool_directive(cache_control_directive::proxy_revalidate);
        } else if (key == "public") {
            apply_bool_directive(cache_control_directive::public_value);
        } else if (key == "immutable") {
            apply_bool_directive(cache_control_directive::immutable);
        } else if (key == "only-if-cached") {
            apply_bool_directive(cache_control_directive::only_if_cached);
        } else if (key == "private") {
            apply_qualified_directive(cache_control_directive::private_value,
                cache_control_directive::private_unqualified, result.private_fields_);
        } else if (key == "no-cache") {
            apply_qualified_directive(cache_control_directive::no_cache,
                cache_control_directive::no_cache_unqualified, result.no_cache_fields_);
        } else if (key == "max-age") {
            apply_delta_directive(result.max_age_);
        } else if (key == "s-maxage") {
            apply_delta_directive(result.s_max_age_);
        } else if (key == "stale-while-revalidate") {
            apply_delta_directive(result.stale_while_revalidate_);
        } else if (key == "stale-if-error") {
            apply_delta_directive(result.stale_if_error_);
        } else if (key == "min-fresh") {
            apply_delta_directive(result.min_fresh_);
        } else if (key == "max-stale") {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean && item.boolean_) {
                result.set(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            } else if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                       item.integer_ >= 0) {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_ = static_cast<std::uint64_t>(item.integer_);
            } else {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            }
        }

        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            return std::nullopt;
        }
    }
    if (!saw_member) {
        return std::nullopt;
    }
    return result;
}

std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept {
    return parse_targeted_cache_control_from_input(detail::http_structured_text_input{value});
}

std::optional<cache_control> detail::parse_targeted_cache_control_headers(
    detail::http_header_range_view headers, std::string_view header_name) noexcept {
    if (header_name.empty()) {
        return std::nullopt;
    }
    return parse_targeted_cache_control_from_input(
        detail::http_structured_header_input{headers.begin(), headers.end(), header_name});
}

targeted_cache_control_selection detail::resolve_targeted_cache_control_headers(
    detail::http_header_range_view headers,
    std::span<const std::string_view> target_headers) noexcept {
    const auto has_cache_control = [&headers]() noexcept {
        for (const auto field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
                return true;
            }
        }
        return false;
    };
    for (const auto target_name : target_headers) {
        if (target_name.empty()) {
            continue;
        }
        if (http_ascii_equals_ignore_case(target_name, "Cache-Control")) {
            if (has_cache_control()) {
                return {
                    .control_ = parse_cache_control_headers(headers),
                    .selected_header_ = target_name,
                    .targeted_ = false,
                };
            }
            continue;
        }
        if (const auto targeted = parse_targeted_cache_control_headers(headers, target_name)) {
            return {
                .control_ = *targeted,
                .selected_header_ = target_name,
                .targeted_ = true,
            };
        }
    }
    return {
        .control_ = parse_cache_control_headers(headers),
        .selected_header_ = has_cache_control() ? std::string_view("Cache-Control")
                                                : std::string_view{},
        .targeted_ = false,
    };
}

bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (control.is_field_qualified_private()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_private_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    const auto count = control.private_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.private_field(i));
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (!control.is_field_qualified_private()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_private_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response_headers& headers) {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response& response) {
    return strip_http_cache_private_headers(response, parse_cache_control(response));
}

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (control.is_field_qualified_no_cache()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_no_cache_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    const auto count = control.no_cache_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.no_cache_field(i));
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (!control.is_field_qualified_no_cache()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_no_cache_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

namespace {

void append_preserved_cache_control_directives(
    std::string_view field_value, std::string& output) {
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view token) {
        const auto eq = token.find('=');
        const auto name =
            http_trim_ows(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (name.empty() || http_ascii_equals_ignore_case(name, "max-age") ||
            http_ascii_equals_ignore_case(name, "s-maxage")) {
            return true;
        }
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(token);
        return true;
    });
}

void finish_rewritten_cache_control_max_age(
    std::string& output, std::uint64_t client_max_age) {
    if (!output.empty()) {
        output.append(", ");
    }
    output.append("max-age=");
    http_append_unsigned(output, client_max_age);
}

[[nodiscard]] std::string normalize_vary_field_value(std::string_view raw) {
    const auto trimmed = http_trim_ows(raw);
    std::string normalized;
    normalized.reserve(trimmed.size());
    bool in_ows = false;
    for (const char ch : trimmed) {
        if (ch == ' ' || ch == '\t') {
            if (!in_ows) {
                normalized.push_back(' ');
                in_ows = true;
            }
        } else {
            normalized.push_back(ch);
            in_ows = false;
        }
    }
    return normalized;
}

template <typename target_headers_type>
void merge_304_headers_impl(
    target_headers_type& cached_headers, detail::http_header_range_view not_modified_headers) {
    const auto nominated = parse_http_connection_nominated_fields(not_modified_headers);
    std::vector<std::string_view> updated_names;
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) ||
            is_http_hop_by_hop_or_nominated_field(name, nominated) ||
            is_http_304_forbidden_header(name)) {
            continue;
        }
        const bool already_recorded =
            std::ranges::any_of(updated_names, [name](std::string_view existing) noexcept {
                return http_ascii_equals_ignore_case(existing, name);
            });
        if (!already_recorded) {
            updated_names.push_back(name);
        }
    }
    if (updated_names.empty()) {
        return;
    }
    if constexpr (std::same_as<target_headers_type, http_response_headers>) {
        for (const auto name : updated_names) {
            cached_headers.remove_header(name);
        }
    } else {
        std::erase_if(cached_headers, [&](const http_header& existing) noexcept {
            return std::ranges::any_of(updated_names, [&](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(existing.name(), updated);
            });
        });
    }
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        const bool should_copy =
            std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(name, updated);
            });
        if (!should_copy) {
            continue;
        }
        if constexpr (std::same_as<target_headers_type, http_response_headers>) {
            if (cached_headers.header(name).has_value() &&
                !http_ascii_equals_ignore_case(name, "Date") &&
                !http_ascii_equals_ignore_case(name, "ETag") &&
                !http_ascii_equals_ignore_case(name, "Expires") &&
                !http_ascii_equals_ignore_case(name, "Content-Location") &&
                !http_ascii_equals_ignore_case(name, "Last-Modified")) {
                cached_headers.header(
                    name, field.value(), {.mode_ = http_response_header_mode::append});
            } else {
                cached_headers.header(name, field.value());
            }
        } else {
            cached_headers.emplace_back(name, field.value());
        }
    }
}
}  // namespace

std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age) {
    std::string output;
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::string detail::rewrite_http_cache_control_max_age_headers(
    detail::http_header_range_view headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age) {
    auto rewritten = rewrite_http_cache_control_max_age(response_headers, client_max_age);
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return http_ascii_equals_ignore_case(field.name(), "Cache-Control") ||
               http_ascii_equals_ignore_case(field.name(), "Expires");
    });
    response_headers.emplace_back("Cache-Control", rewritten);
}

void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto& field : response_headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    response_headers.remove_header("Expires");
    response_headers.header("Cache-Control", output);
}

void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age) {
    auto staged = response.headers().clone(1);
    rewrite_http_response_cache_max_age(staged, client_max_age);
    response.commit_headers(std::move(staged));
}

void http_vary_spec::update(std::string_view field_value) {
    if (wildcard_) {
        return;
    }
    detail::http_visit_comma_separated_tokens(field_value, [&](std::string_view token) {
        if (token == "*") {
            wildcard_ = true;
            field_names_.clear();
            return false;
        }
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        const auto it = std::ranges::lower_bound(field_names_, lower);
        if (it == field_names_.end() || *it != lower) {
            field_names_.insert(it, std::move(lower));
        }
        return true;
    });
}

void http_vary_spec::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

http_vary_spec parse_http_vary(std::string_view field_value) {
    http_vary_spec spec;
    spec.update(field_value);
    return spec;
}

std::optional<std::string> detail::format_http_vary_cache_key_headers(
    const http_vary_spec& vary, detail::http_header_range_view request_headers) {
    if (vary.is_wildcard()) {
        return std::nullopt;
    }
    std::string key;
    for (const auto& field_name : vary.field_names()) {
        key.append(field_name);
        bool present = false;
        for (const auto header : request_headers) {
            if (!http_ascii_equals_ignore_case(header.name(), field_name)) {
                continue;
            }
            if (!present) {
                key.append(":1:");
                present = true;
            } else {
                key.append(", ");
            }
            key.append(normalize_vary_field_value(header.value()));
        }
        if (!present) {
            key.append(":0");
        }
        key.push_back('\n');
    }
    return key;
}

std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty() || trimmed.front() == '"') {
        return std::nullopt;
    }
    return parse_delta_seconds(trimmed);
}

std::uint64_t detail::compute_http_cache_age_headers(
    detail::http_header_range_view response_headers, std::time_t request_time,
    std::time_t response_time, std::time_t now) noexcept {
    std::optional<std::time_t> date_value;
    std::optional<std::uint64_t> age_value;
    for (const auto field : response_headers) {
        if (!date_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Date")) {
            date_value = parse_http_date(field.value());
        } else if (!age_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Age")) {
            age_value = parse_http_age(field.value());
        }
    }
    return compute_http_cache_age(date_value, age_value, request_time, response_time, now);
}

bool is_http_304_forbidden_header(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 6U:
            return http_ascii_equals_ignore_case(field_name, "Digest");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer");
        case 11U:
            return http_ascii_equals_ignore_case(field_name, "Content-MD5") ||
                   http_ascii_equals_ignore_case(field_name, "Repr-Digest");
        case 12U:
            return http_ascii_equals_ignore_case(field_name, "Content-Type");
        case 13U:
            return http_ascii_equals_ignore_case(field_name, "Content-Range");
        case 14U:
            return http_ascii_equals_ignore_case(field_name, "Content-Length") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Digest");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Content-Encoding") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Language");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Content-Disposition");
        default:
            return false;
    }
}

void sanitize_http_304_response_headers(std::vector<http_header>& headers) {
    std::erase_if(headers, [](const http_header& field) noexcept {
        return is_http_304_forbidden_header(field.name());
    });
}

void sanitize_http_304_response_headers(http_response_headers& headers) {
    std::vector<std::string> to_remove;
    for (const auto& field : headers) {
        if (is_http_304_forbidden_header(field.name())) {
            to_remove.emplace_back(field.name());
        }
    }
    for (const auto& name : to_remove) {
        headers.remove_header(name);
    }
}

void sanitize_http_304_response_headers(http_response& response) {
    auto staged = response.headers().clone();
    sanitize_http_304_response_headers(staged);
    response.commit_headers(std::move(staged));
}

void detail::merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void detail::merge_http_304_response_headers_into(
    http_response_headers& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response) {
    auto staged = cached_response.headers().clone(not_modified_response.headers().size());
    merge_http_304_response_headers(staged, not_modified_response.headers());
    cached_response.commit_headers(std::move(staged));
}

}  // namespace ruvia

namespace ruvia::detail {

// UTC calendar fields for an HTTP-date, or no value outside years 0000-9999
// (the four-digit IMF-fixdate year). Civil conversion is independent of the C
// runtime's date range and locale; in particular, Windows gmtime_s cannot
// represent pre-1970 UTC timestamps. The range check runs before calendar
// conversion so huge time_t values cannot overflow it.
[[nodiscard]] std::optional<std::tm> http_utc_tm(std::time_t time) noexcept {
    using namespace std::chrono;
    constexpr auto first = duration_cast<seconds>(sys_days{year{0} / January / 1}.time_since_epoch()).count();
    constexpr auto end = duration_cast<seconds>(sys_days{year{10000} / January / 1}.time_since_epoch()).count();
    if (std::cmp_less(time, first) || std::cmp_greater_equal(time, end)) {
        return std::nullopt;
    }
    const sys_seconds instant{seconds{static_cast<seconds::rep>(time)}};
    const auto date = floor<days>(instant);
    const year_month_day calendar{date};
    const hh_mm_ss clock{instant - date};
    std::tm utc{};
    utc.tm_year = static_cast<int>(calendar.year()) - 1900;
    utc.tm_mon = static_cast<int>(static_cast<unsigned>(calendar.month())) - 1;
    utc.tm_mday = static_cast<int>(static_cast<unsigned>(calendar.day()));
    utc.tm_wday = static_cast<int>(weekday{date}.c_encoding());
    utc.tm_hour = static_cast<int>(clock.hours().count());
    utc.tm_min = static_cast<int>(clock.minutes().count());
    utc.tm_sec = static_cast<int>(clock.seconds().count());
    return utc;
}

std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept {
    if (year < static_cast<int>((std::chrono::year::min)()) || year > static_cast<int>((std::chrono::year::max)()) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days_since_epoch = static_cast<std::int64_t>(std::chrono::sys_days{date}.time_since_epoch().count());
    const auto total = days_since_epoch * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                       static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    if (!std::in_range<std::time_t>(total)) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(total);
}

namespace {

// "Date: <value>\r\n" -- the cache is the single owner of this wire format.
// Both the full-line accessor (HTTP/1 text response) and the
// value-only accessor (HPACK :date for HTTP/2) derive their views from these
// constants, so the framing lives in exactly one place.
inline constexpr std::string_view date_header_prefix = "Date: ";
inline constexpr std::string_view date_header_suffix = "\r\n";

struct date_cache final {
    std::array<char, 64> line_{};
    std::size_t size_{0};
    std::time_t second_{};
    bool initialized_{false};
};

[[nodiscard]] date_cache& worker_date_cache() noexcept {
    thread_local date_cache cache;
    return cache;
}

}  // namespace

std::string_view cached_date_header() noexcept {
    const auto now = std::time(nullptr);
    auto& cache = worker_date_cache();
    if (!cache.initialized_ || cache.second_ != now) {
        cache.second_ = now;
        cache.initialized_ = true;
        cache.size_ = 0;
        // RFC 9110 section 6.6.1: do not generate Date without a usable clock.
        if (now != std::time_t{-1}) {
            if (const auto date = format_http_date(now)) {
                std::memcpy(cache.line_.data(), date_header_prefix.data(), date_header_prefix.size());
                std::memcpy(cache.line_.data() + date_header_prefix.size(), date->data(), date->size());
                cache.size_ = date_header_prefix.size() + date->size();
                cache.line_[cache.size_++] = date_header_suffix[0];
                cache.line_[cache.size_++] = date_header_suffix[1];
            }
        }
    }
    return std::string_view(cache.line_.data(), cache.size_);
}

std::string_view cached_date_value() noexcept {
    const auto line = cached_date_header();
    constexpr std::size_t framing = date_header_prefix.size() + date_header_suffix.size();
    if (line.size() <= framing) {
        return {};
    }
    return line.substr(date_header_prefix.size(), line.size() - framing);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

// IMF-fixdate day names (RFC 9110 section 5.6.7), indexed by tm_wday.
constexpr std::array<std::string_view, 7> http_weekday_names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

[[nodiscard]] int month_index(std::string_view value) noexcept {
    for (std::size_t i = 0; i < detail::http_month_names.size(); ++i) {
        if (value == detail::http_month_names[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] bool is_short_weekday(std::string_view value) noexcept {
    return std::ranges::find(http_weekday_names, value) != http_weekday_names.end();
}

[[nodiscard]] bool is_long_weekday(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] std::optional<int> parse_fixed_digits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed_value = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed_value = parsed_value * 10 + (c - '0');
    }
    return parsed_value;
}

[[nodiscard]] std::optional<std::time_t> parse_imf_fixdate(std::string_view value) noexcept {
    if (value.size() != http_imf_fixdate_size || !is_short_weekday(value.substr(0, 3)) || value[3] != ',' ||
        value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || value[25] != ' ' || value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(value.substr(5, 2));
    const auto month = month_index(value.substr(8, 3));
    const auto year = parse_fixed_digits(value.substr(12, 4));
    const auto hour = parse_fixed_digits(value.substr(17, 2));
    const auto minute = parse_fixed_digits(value.substr(20, 2));
    const auto second = parse_fixed_digits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] std::optional<std::time_t> parse_asctime_date(std::string_view value) noexcept {
    if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value[19] != ' ' ||
        !is_short_weekday(value.substr(0, 3))) {
        return std::nullopt;
    }
    const auto month = month_index(value.substr(4, 3));
    auto day_field = value.substr(8, 2);
    if (day_field.front() == ' ') {
        day_field.remove_prefix(1);
    }
    const auto day = parse_fixed_digits(day_field);
    const auto hour = parse_fixed_digits(value.substr(11, 2));
    const auto minute = parse_fixed_digits(value.substr(14, 2));
    const auto second = parse_fixed_digits(value.substr(17, 2));
    const auto year = parse_fixed_digits(value.substr(20, 4));
    if (month == 0 || !day || !hour || !minute || !second || !year) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] int resolve_rfc850_year(int short_year, int current_year) noexcept {
    const auto current_century = current_year - current_year % 100;
    auto year = current_century + short_year;
    // RFC 9110 section 5.6.7: an obsolete two-digit date that appears more
    // than 50 years in the future denotes the most recent past year with the
    // same final two digits. This is a rolling pivot, not the fixed POSIX
    // 68/69 split.
    if (year > current_year + 50) {
        year -= 100;
    }
    return year;
}

[[nodiscard]] std::optional<std::time_t> parse_rfc850_date(std::string_view value) noexcept {
    const auto comma = value.find(", ");
    if (comma == std::string_view::npos || !is_long_weekday(value.substr(0, comma))) {
        return std::nullopt;
    }
    const auto body = value.substr(comma + 2);
    if (body.size() != 22 || body[2] != '-' || body[6] != '-' || body[9] != ' ' ||
        body[12] != ':' || body[15] != ':' || body[18] != ' ' || body.substr(19, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(body.substr(0, 2));
    const auto month = month_index(body.substr(3, 3));
    const auto short_year = parse_fixed_digits(body.substr(7, 2));
    const auto hour = parse_fixed_digits(body.substr(10, 2));
    const auto minute = parse_fixed_digits(body.substr(13, 2));
    const auto second = parse_fixed_digits(body.substr(16, 2));
    if (!day || month == 0 || !short_year || !hour || !minute || !second) {
        return std::nullopt;
    }
    const auto now = std::time(nullptr);
    if (now == std::time_t{-1}) {
        return std::nullopt;
    }
    const auto current_utc = detail::http_utc_tm(now);
    if (!current_utc) {
        return std::nullopt;
    }
    const int current_year = current_utc->tm_year + 1900;
    auto year = resolve_rfc850_year(*short_year, current_year);
    // RFC 9110 section 5.6.7 applies the rolling 50-year pivot to the full
    // timestamp. Equal years alone do not establish that a date is in range.
    const std::array date_fields{month, *day, *hour, *minute, *second};
    const std::array reference_fields{current_utc->tm_mon + 1, current_utc->tm_mday, current_utc->tm_hour,
        current_utc->tm_min, current_utc->tm_sec};
    if (year == current_year + 50 && date_fields > reference_fields) {
        year -= 100;
    }
    return detail::http_civil_to_time_t(year, month, *day, *hour, *minute, *second);
}

}  // namespace

// Allocation-free wire value; an unrepresentable date must not be truncated or
// substituted with a different timestamp. Fixed English names are independent
// of the process locale, as required by RFC 9110 section 5.6.7.
std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept {
    const auto utc = detail::http_utc_tm(value);
    if (!utc) {
        return std::nullopt;
    }
    std::array<char, http_imf_fixdate_size> output{};
    auto* out = output.data();
    const long year = static_cast<long>(utc->tm_year) + 1900;

    std::size_t i = 0;
    const auto put2 = [out, &i](int field) noexcept {
        out[i++] = static_cast<char>('0' + (field / 10) % 10);
        out[i++] = static_cast<char>('0' + field % 10);
    };
    const auto put3 = [out, &i](std::string_view name) noexcept {
        out[i++] = name[0];
        out[i++] = name[1];
        out[i++] = name[2];
    };

    put3(http_weekday_names[static_cast<std::size_t>(utc->tm_wday)]);
    out[i++] = ',';
    out[i++] = ' ';
    put2(utc->tm_mday);
    out[i++] = ' ';
    put3(detail::http_month_names[static_cast<std::size_t>(utc->tm_mon)]);
    out[i++] = ' ';
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 1000) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 100) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 10) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>(year % 10));
    out[i++] = ' ';
    put2(utc->tm_hour);
    out[i++] = ':';
    put2(utc->tm_min);
    out[i++] = ':';
    put2(utc->tm_sec);
    out[i++] = ' ';
    out[i++] = 'G';
    out[i++] = 'M';
    out[i++] = 'T';
    return output;
}

std::optional<std::time_t> parse_http_date(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (const auto imf = parse_imf_fixdate(value)) {
        return imf;
    }
    if (const auto rfc850 = parse_rfc850_date(value)) {
        return rfc850;
    }
    return parse_asctime_date(value);
}

}  // namespace ruvia
