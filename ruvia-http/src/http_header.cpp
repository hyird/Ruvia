#include "http_header.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "http2_connection.h"
#include "http_cache.h"
#include "http_content_coding.h"
#include "http_response.h"

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

#include "borrowed_text.h"
#include "http_connection_fields.h"
#include "http_request.h"
#include "http_response_server.h"

namespace ruvia::detail {

struct http_media_type_parts final {
    std::string_view type_;
    std::string_view subtype_;
};

// Both values have already passed parameter syntax validation. Compare their
// decoded bytes without materializing quoted strings.
[[nodiscard]] inline bool http_media_parameter_value_equals(std::string_view left, std::string_view right, bool ascii_case_insensitive = false) noexcept {
    if (left.front() != '"' && right.front() != '"') {
        return ascii_case_insensitive ? http_ascii_equals_ignore_case(left, right) : left == right;
    }
    struct cursor final {
        std::string_view value_;
        http_quoted_string_cursor quoted_cursor_;
        std::size_t position_{0};

        explicit cursor(std::string_view value) noexcept :
            value_(value),
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

    [[nodiscard]] bool contains(std::string_view name, std::string_view value, const http_media_type_parts& parts) const noexcept {
        const auto parameter_value = find(name);
        if (!parameter_value.has_value()) {
            return false;
        }
        // RFC 3676 section 4 defines these case-insensitive values only for text/plain.
        const bool ascii_case_insensitive = http_ascii_equals_ignore_case(name, "charset") || ((http_ascii_equals_ignore_case(name, "format") || http_ascii_equals_ignore_case(name, "delsp")) && http_ascii_equals_ignore_case(parts.type_, "text") && http_ascii_equals_ignore_case(parts.subtype_, "plain"));
        return http_media_parameter_value_equals(*parameter_value, value, ascii_case_insensitive);
    }
};

[[nodiscard]] inline bool http_parse_media_type_parts(std::string_view value, bool allow_wildcard, http_media_type_parts& parts) noexcept {
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
    return (type_wildcard || is_valid_http_header_name(parts.type_)) && (subtype_wildcard || is_valid_http_header_name(parts.subtype_));
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type_parts(value_type&&, bool, http_media_type_parts&) = delete;

[[nodiscard]] inline bool http_parse_media_type(std::string_view value, http_media_type_parts& parts, http_media_type_parameters& parameters) noexcept {
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
            return http_ascii_equals_ignore_case(field_name, "Trailer") || http_ascii_equals_ignore_case(field_name, "Upgrade");
        case 10U:
            return http_ascii_equals_ignore_case(field_name, "Connection") || http_ascii_equals_ignore_case(field_name, "Keep-Alive");
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
    return (option.size() == 5U && http_ascii_equals_ignore_case(option, "close")) || is_http_hop_by_hop_field_name(option);
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

std::string http_connection_nominated_fields::rebuild_connection_field(bool retain_upgrade, bool include_non_standard) const {
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

http_connection_nominated_fields parse_http_connection_nominated_fields(std::string_view connection_field_value) {
    http_connection_nominated_fields nominated;
    nominated.update(connection_field_value);
    return nominated;
}
bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name, const http_connection_nominated_fields& nominated, bool retain_upgrade) noexcept {
    if (retain_upgrade && nominated.has_upgrade() && http_ascii_equals_ignore_case(field_name, "Upgrade")) {
        return false;
    }
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_http_hop_by_hop_trailer_field(std::string_view field_name, const http_connection_nominated_fields& nominated) noexcept {
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
    return http_conditional_headers{detail::http_request_access::has_repeated_known_header(request, detail::http_field_kind::if_unmodified_since) ? std::string_view{} : detail::http_request_access::known_header(request, detail::http_field_kind::if_unmodified_since), detail::http_request_access::has_repeated_known_header(request, detail::http_field_kind::if_modified_since) ? std::string_view{} : detail::http_request_access::known_header(request, detail::http_field_kind::if_modified_since), detail::http_request_access::known_header(request, detail::http_field_kind::range), detail::http_request_access::known_header(request, detail::http_field_kind::if_range), detail::http_request_access::has_known_header(request, detail::http_field_kind::if_range)};
}

void http_etag_field_condition::update(std::string_view value, std::string_view expected, bool strong) noexcept {
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

http_etag_preconditions get_http_etag_preconditions(const http_request& request, std::string_view etag) noexcept {
    http_etag_preconditions result;
    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them line by line and
    // retain whole-list validity without allocating a joined string.
    (void)detail::http_request_access::visit_known_headers(request, detail::http_field_kind::if_match, [&result, etag](std::string_view value) noexcept {
        result.if_match_.update(value, etag, true);
        return true;
    });
    (void)detail::http_request_access::visit_known_headers(request, detail::http_field_kind::if_none_match, [&result, etag](std::string_view value) noexcept {
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

bool http_if_range_allows(std::string_view header_value, std::string_view etag, std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept {
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
    return is_valid_http_entity_tag(left) && is_valid_http_entity_tag(right) && detail::http_weak_etag_equals(left, right);
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
[[nodiscard]] http_accept_token_match http_accept_token_matches(std::string_view range, std::string_view offered, bool prefix_matching) noexcept {
    if (range == "*") {
        return http_accept_token_match::wildcard;
    }
    if (http_ascii_equals_ignore_case(range, offered)) {
        return http_accept_token_match::exact;
    }
    if (prefix_matching && offered.size() > range.size() && offered[range.size()] == '-' && http_ascii_equals_ignore_case(range, offered.substr(0, range.size()))) {
        return http_accept_token_match::prefix;
    }
    return http_accept_token_match::none;
}

void http_accumulate_token_acceptance(std::string_view accept, std::string_view offered, bool prefix_matching, int& best_specificity, int& best_quality) noexcept {
    http_visit_comma_separated_quoted(accept, [offered, prefix_matching, &best_specificity, &best_quality](std::string_view item) noexcept {
        const auto range = http_header_token_before_parameters(item);
        const auto match = http_accept_token_matches(range, offered, prefix_matching);
        if (match == http_accept_token_match::none) {
            return true;
        }
        const auto specificity = prefix_matching && match != http_accept_token_match::wildcard ? static_cast<int>(std::min(range.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()))) : static_cast<int>(match);
        const auto quality = http_weight_parameter(item);
        if (specificity > best_specificity || (specificity == best_specificity && quality > best_quality)) {
            best_specificity = specificity;
            best_quality = quality;
        }
        return true;
    });
}

[[nodiscard]] int http_media_range_match_specificity(std::string_view range, const http_media_type_parts& offered_parts, const http_media_type_parameters& offered_parameters, std::size_t& parameter_count, int& quality) noexcept {
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

void http_accumulate_media_type_acceptance(std::string_view accept, std::string_view offered, int& best_specificity, std::size_t& best_parameter_count, int& best_quality) noexcept {
    http_media_type_parts offered_parts;
    http_media_type_parameters offered_parameters;
    if (!http_parse_media_type(offered, offered_parts, offered_parameters)) {
        return;
    }
    http_visit_comma_separated_quoted(accept, [offered_parts, &offered_parameters, &best_specificity, &best_parameter_count, &best_quality](std::string_view item) noexcept {
        int quality = 1000;
        std::size_t parameter_count = 0;
        const auto specificity = http_media_range_match_specificity(item, offered_parts, offered_parameters, parameter_count, quality);
        if (specificity >= 0) {
            if (specificity > best_specificity || (specificity == best_specificity && (parameter_count > best_parameter_count || (parameter_count == best_parameter_count && quality > best_quality)))) {
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

void http_accept_match::update_token(std::string_view field, std::string_view offered, http_accept_token_match_mode mode) noexcept {
    detail::http_accumulate_token_acceptance(field, offered, mode == http_accept_token_match_mode::language_prefix, specificity_, quality_);
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
            fields_value.urgency_ = item.kind_ == item_type::kind_type::integer ? http_priority_urgency::try_from_value(item.integer_) : std::nullopt;
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
std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(http_header_range_view headers) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(http_structured_parser{http_structured_header_input{headers.begin(), headers.end(), "priority"}}, fields); parsed_value.index() != 0) {
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
    const auto id = ((static_cast<std::uint32_t>(id_bytes[0]) & 0x7fU) << 24) | (static_cast<std::uint32_t>(id_bytes[1]) << 16) | (static_cast<std::uint32_t>(id_bytes[2]) << 8) | static_cast<std::uint32_t>(id_bytes[3]);
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
    (void)encode_http2_frame_header(output.first(http2_frame_header_bytes), static_cast<std::uint32_t>(4 + std::get<0>(length)), http2_frame_type::priority_update, 0, 0);
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
    return kind == http_field_kind::host ? 1U << static_cast<std::uint32_t>(kind) : singleton_request_header_bit(kind);
}

// RFC 9110 section 5.6.6 parameter: no whitespace around '='.
[[nodiscard]] bool is_exact_http_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(part.substr(0, equals)) && is_http_token_or_quoted_string(part.substr(equals + 1));
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

[[nodiscard]] bool is_valid_te(std::string_view value, http_field_list_role role, request_field_framing framing) noexcept {
    if (framing == request_field_framing::binary) {
        return is_valid_binary_http_te_field_value(value, role);
    }
    return is_valid_http_te_field_value(value, role);
}

[[nodiscard]] bool is_valid_value(http_field_kind kind, std::string_view value, http_field_list_role role, request_field_framing framing, http_request_field_source source) noexcept {
    // RFC 9110 sections 2.2 and 5.6.1: forwarding is not generation. Reuse the
    // recipient value rules only for end-to-end application metadata. Ownership
    // and singleton checks below still use the actual sender role.
    const auto application_role = source == http_request_field_source::forwarded ? http_field_list_role::recipient : role;
    switch (kind) {
        case http_field_kind::origin:
            return application_role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case http_field_kind::access_control_request_method:
            return application_role == http_field_list_role::recipient || is_valid_http_method_token(value);
        case http_field_kind::access_control_request_headers:
            return application_role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case http_field_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return application_role == http_field_list_role::recipient || is_valid_http_content_type_field_value(value);
        case http_field_kind::content_encoding:
            // Admission leaves received metadata to the decoder. Forwarding
            // validates the recipient list grammar, not arbitrary nonempty text.
            return role == http_field_list_role::recipient || is_valid_http_content_encoding_field_value(value, application_role);
        case http_field_kind::trailer:
            // The declaration neither frames content nor authorizes actual trailers.
            return role == http_field_list_role::recipient || is_valid_http_request_trailer_field_value(value, role);
        case http_field_kind::te:
            return is_valid_te(value, role, framing);
        case http_field_kind::expect:
            // Binary field semantics feed the existing 417/ignore policy, not malformed-message rejection.
            return framing == request_field_framing::binary || is_valid_received_http_expect_field_value(value);
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

request_field_status validate_request_field(http_field_kind kind, std::string_view value, http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return request_field_status::invalid;
    }
    if (role == http_field_list_role::sender && (kind == http_field_kind::expect || kind == http_field_kind::content_length)) {
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
    if (role == http_field_list_role::recipient && (kind == http_field_kind::if_modified_since || kind == http_field_kind::if_unmodified_since || kind == http_field_kind::user_agent)) {
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
    return detail::validate_request_field(kind, value, detail::http_field_list_role::sender, state) == detail::request_field_status::ok;
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

bool is_forbidden_common_trailer_name(std::string_view name, http_field_kind kind) noexcept {
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
            return http_ascii_equals_ignore_case(name, "Keep-Alive") || http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") || http_ascii_equals_ignore_case(name, "Content-Range");
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
    return kind == http_field_kind::access_control_request_headers || kind == http_field_kind::access_control_request_method || kind == http_field_kind::origin || http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name); known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
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
            return http_ascii_equals_ignore_case(name, "Expires") || http_ascii_equals_ignore_case(name, "Warning");
        case 11:
            return http_ascii_equals_ignore_case(name, "Retry-After");
        case 15:
            return http_ascii_equals_ignore_case(name, "X-Frame-Options") || http_ascii_equals_ignore_case(name, "Referrer-Policy") || http_ascii_equals_ignore_case(name, "Clear-Site-Data");
        case 16:
            return http_ascii_equals_ignore_case(name, "X-XSS-Protection") || http_ascii_equals_ignore_case(name, "WWW-Authenticate");
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

[[nodiscard]] bool is_valid_http_response_trailer_field_value(std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role, [](std::string_view name) noexcept {
        return is_forbidden_response_trailer_name(name);
    });
}

[[nodiscard]] bool response_trailer_field_valid(std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) && is_valid_http_header_value(value);
}

response_field_status validate_response_field(http_field_kind kind, std::string_view value, response_field_state& state) noexcept {
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
    return detail::validate_response_field(detail::classify_http_field(name), value, state) == detail::response_field_status::ok;
}

http_response_trailer_section validate_http_response_trailers(std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) || !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

}  // namespace ruvia
