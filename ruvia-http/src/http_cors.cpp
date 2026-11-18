#include "http_cors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "decimal_text.h"
#include "http_authority.h"
#include "http_field_text.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_uri_syntax.h"

namespace ruvia {
namespace {

[[nodiscard]] bool is_valid_cors_origin_value(std::string_view origin) noexcept {
    return origin == "null" || is_valid_http_serialized_origin(origin);
}

[[nodiscard]] std::string join_tokens(std::span<const std::string> items) {
    std::string output;
    if (items.empty()) {
        return output;
    }
    std::size_t total = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            total += 2U;
        }
        total += items[index].size();
    }
    output.reserve(total);
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        output.append(items[index]);
    }
    return output;
}

[[nodiscard]] bool vary_value_contains_token(std::string_view vary_value, std::string_view target_token) noexcept {
    bool found = false;
    detail::http_visit_comma_separated_tokens(vary_value, [&](std::string_view token) noexcept {
        if (token == "*" || http_ascii_equals_ignore_case(token, target_token)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

[[nodiscard]] bool headers_have_vary_token(const http_header_range_view& headers, std::string_view token) noexcept {
    for (const auto header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Vary") && vary_value_contains_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

template <std::size_t name_length, std::size_t value_length>
consteval auto make_cors_header_bytes(const char (&name)[name_length], const char (&value)[value_length]) noexcept {
    std::array<char, (name_length - 1U) + (value_length - 1U) + 1U> bytes{};
    for (std::size_t index = 0; index < name_length - 1U; ++index) {
        bytes[index] = name[index];
    }
    for (std::size_t index = 0; index < value_length - 1U; ++index) {
        bytes[(name_length - 1U) + index] = value[index];
    }
    return bytes;
}

}  // namespace

bool is_http_cors_controlled_response_header(std::string_view name) noexcept {
    switch (name.size()) {
        case 22U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Max-Age");
        case 27U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Origin");
        case 28U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Methods") || http_ascii_equals_ignore_case(name, "Access-Control-Allow-Headers");
        case 29U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Expose-Headers");
        case 32U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Credentials");
        default:
            return false;
    }
}

void merge_http_vary_tokens(editable_http_headers headers, std::span<const std::string_view> tokens) {
    const auto fields = headers.fields();
    std::array<std::string_view, 8> inline_missing{};
    std::size_t inline_missing_count = 0;
    std::vector<std::string_view> overflow_missing;
    const auto current_missing = [&]() noexcept -> std::span<const std::string_view> {
        return overflow_missing.empty() ? std::span<const std::string_view>(inline_missing.data(), inline_missing_count) : std::span<const std::string_view>(overflow_missing);
    };
    std::size_t added_bytes = 0;
    for (const auto token : tokens) {
        const auto trimmed = http_trim_ows(token);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed == "*") {
            if (!headers_have_vary_token(fields, "*")) {
                headers.set("Vary", "*");
            }
            return;
        }
        if (!is_valid_http_header_name(trimmed)) {
            throw std::invalid_argument("Vary token must be a valid HTTP field name");
        }
        if (headers_have_vary_token(fields, trimmed)) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(current_missing(), [trimmed](std::string_view current) noexcept {
            return http_ascii_equals_ignore_case(current, trimmed);
        });
        if (!duplicate) {
            added_bytes += trimmed.size() + 2U;
            if (!overflow_missing.empty()) {
                overflow_missing.push_back(trimmed);
            } else if (inline_missing_count < inline_missing.size()) {
                inline_missing[inline_missing_count++] = trimmed;
            } else {
                overflow_missing.reserve(tokens.size());
                overflow_missing.assign(inline_missing.begin(), inline_missing.end());
                overflow_missing.push_back(trimmed);
            }
        }
    }
    const auto missing = current_missing();
    if (missing.empty()) {
        return;
    }

    std::array<char, 256> inline_combined{};
    std::size_t inline_combined_size = 0;
    std::string overflow_combined;
    const auto append_chunk = [&](std::string_view chunk) {
        if (!overflow_combined.empty()) {
            overflow_combined.append(chunk);
            return;
        }
        if (chunk.size() <= inline_combined.size() - inline_combined_size) {
            std::copy(chunk.begin(), chunk.end(), inline_combined.data() + inline_combined_size);
            inline_combined_size += chunk.size();
            return;
        }
        overflow_combined.reserve(inline_combined_size + added_bytes + chunk.size() + 32U);
        overflow_combined.append(inline_combined.data(), inline_combined_size);
        overflow_combined.append(chunk);
    };
    const auto append_member = [&](std::string_view member) {
        if (inline_combined_size != 0 || !overflow_combined.empty()) {
            append_chunk(", ");
        }
        append_chunk(member);
    };
    for (const auto field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "Vary")) {
            continue;
        }
        const auto listed = http_trim_ows(field.value());
        if (!listed.empty()) {
            append_member(listed);
        }
    }
    for (const auto token : missing) {
        append_member(token);
    }
    const std::string_view combined_view = !overflow_combined.empty() ? std::string_view(overflow_combined) : std::string_view(inline_combined.data(), inline_combined_size);
    headers.set("Vary", combined_view);
}

void merge_http_vary_token(editable_http_headers headers, std::string_view token) {
    merge_http_vary_tokens(headers, std::span<const std::string_view>(&token, 1));
}

http_cors_policy::http_cors_policy(const http_cors_policy_options& config) {
    initialize(config);
}

http_cors_policy::http_cors_policy(http_cors_policy_options&& config) {
    initialize(std::move(config));
}

template <typename config_type>
void http_cors_policy::initialize(config_type&& config) {
    allow_any_origin_ = config.allow_any_origin_;
    reflect_request_headers_ = config.reflect_request_headers_;
    allow_credentials_ = config.allow_credentials_;
    constexpr bool move_owned = std::is_rvalue_reference_v<config_type&&>;
    const auto add_suffix = [this](std::string_view pattern) {
        if (pattern.empty()) {
            throw std::invalid_argument("CORS origin suffix must not be empty");
        }
        suffix_rule rule;
        auto rest = pattern;
        if (const auto scheme_pos = rest.find("://"); scheme_pos != std::string_view::npos) {
            const auto scheme = rest.substr(0, scheme_pos);
            if (!http_ascii_equals_ignore_case(scheme, "http") && !http_ascii_equals_ignore_case(scheme, "https")) {
                throw std::invalid_argument("CORS origin suffix scheme must be http or https");
            }
            rule.scheme_ = http_ascii_lowercase(scheme);
            rest = rest.substr(scheme_pos + 3U);
        }
        if (rest.starts_with("*.")) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(2U);
        } else if (rest.starts_with('.')) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(1U);
        }
        const auto authority = parse_http_authority(rest);
        if (!authority || authority->host().empty() || authority->port_kind() == http_authority_port_kind::empty || authority->host().starts_with('[') || is_valid_http_ipv4_literal(authority->host()) || authority->host().front() == '.' || authority->host().back() == '.') {
            throw std::invalid_argument("CORS origin suffix must name a valid domain");
        }
        rule.host_suffix_ = http_ascii_lowercase(authority->host());
        rule.port_ = authority->port();
        suffix_rules_.push_back(std::move(rule));
    };

    for (auto& origin_entry : config.allowed_origins_) {
        const std::string_view view(origin_entry);
        if (view == "*") {
            allow_any_origin_ = true;
            continue;
        }
        if (view.starts_with("*.") || view.starts_with('.') || view.find("://*.") != std::string_view::npos || view.find("://.") != std::string_view::npos) {
            add_suffix(view);
            continue;
        }
        if (!is_valid_cors_origin_value(view)) {
            throw std::invalid_argument("CORS allowed origin must be a WHATWG serialized origin");
        }
        if constexpr (move_owned) {
            exact_origins_.push_back(std::move(origin_entry));
        } else {
            exact_origins_.emplace_back(view);
        }
    }

    for (const auto& suffix_entry : config.allowed_origin_suffixes_) {
        add_suffix(suffix_entry);
    }
    if (exact_origins_.empty() && suffix_rules_.empty()) {
        allow_any_origin_ = true;
    }
    // Fail closed: reflecting every Origin with credentials would let any site
    // read credentialed responses (Fetch forbids "*" with credentials).
    if (allow_any_origin_ && allow_credentials_) {
        throw std::invalid_argument("CORS credentials require an explicit allowed origin list");
    }
    allow_any_method_ = config.allowed_methods_.empty();
    for (const auto& method : config.allowed_methods_) {
        const std::string_view view(method);
        if (view == "*") {
            allow_any_method_ = true;
            continue;
        }
        if (!is_valid_http_method_token(view)) {
            throw std::invalid_argument("CORS allowed method must be a valid HTTP method token");
        }
    }
    if (!allow_any_method_) {
        if constexpr (move_owned) {
            allowed_methods_ = std::move(config.allowed_methods_);
        } else {
            allowed_methods_ = config.allowed_methods_;
        }
        joined_allowed_methods_ = join_tokens(allowed_methods_);
    }

    for (auto& header_name : config.allowed_headers_) {
        const std::string_view view(header_name);
        if (view == "*") {
            reflect_request_headers_ = true;
            continue;
        }
        if (!is_valid_http_header_name(view)) {
            throw std::invalid_argument("CORS allowed header must be a valid HTTP field name");
        }
        if constexpr (move_owned) {
            allowed_headers_.push_back(std::move(header_name));
        } else {
            allowed_headers_.emplace_back(view);
        }
    }
    if (allowed_headers_.empty()) {
        reflect_request_headers_ = true;
    }
    joined_allowed_headers_ = join_tokens(allowed_headers_);
    for (const auto& expose_name : config.expose_headers_) {
        if (!is_valid_http_header_name(expose_name)) {
            throw std::invalid_argument("CORS exposed header must be a valid HTTP field name");
        }
    }
    joined_expose_headers_ = join_tokens(config.expose_headers_);

    if (config.max_age_.has_value()) {
        if (config.max_age_->count() < 0) {
            throw std::invalid_argument("CORS max age must not be negative");
        }
        std::array<char, max_decimal_size> buffer{};
        const auto formatted = format_decimal(buffer, static_cast<std::uint64_t>(config.max_age_->count()));
        max_age_.assign(formatted);
    }
}

bool http_cors_policy::allows_origin(std::string_view origin) const noexcept {
    if (!is_valid_cors_origin_value(origin)) {
        return false;
    }
    if (allow_any_origin_) {
        return true;
    }
    if (origin == "null") {
        return std::ranges::any_of(exact_origins_, [](std::string_view configured) noexcept {
            return configured == "null";
        });
    }

    const auto scheme_sep = origin.find("://");
    if (scheme_sep == std::string_view::npos) {
        return false;
    }
    const auto scheme = origin.substr(0, scheme_sep);
    const auto authority_text = origin.substr(scheme_sep + 3U);

    for (const auto& configured : exact_origins_) {
        if (configured == "null" || configured.size() != origin.size()) {
            continue;
        }
        const auto conf_sep = configured.find("://");
        if (conf_sep != scheme_sep || !http_ascii_equals_ignore_case(scheme, std::string_view(configured).substr(0, conf_sep))) {
            continue;
        }
        if (http_ascii_equals_ignore_case(authority_text, std::string_view(configured).substr(conf_sep + 3U))) {
            return true;
        }
    }

    if (suffix_rules_.empty()) {
        return false;
    }
    const auto authority = parse_http_authority(authority_text);
    if (!authority || authority->host().empty() || authority->host().starts_with('[') || is_valid_http_ipv4_literal(authority->host())) {
        return false;
    }
    const auto host = authority->host();
    const auto port = authority->port();

    for (const auto& rule : suffix_rules_) {
        if (!rule.scheme_.empty() && !http_ascii_equals_ignore_case(scheme, rule.scheme_)) {
            continue;
        }
        if (rule.port_.has_value() && port != rule.port_) {
            continue;
        }
        const std::string_view suffix(rule.host_suffix_);
        if (!rule.subdomain_only_ && http_ascii_equals_ignore_case(host, suffix)) {
            return true;
        }
        if (is_dns_subdomain_of(host, suffix)) {
            return true;
        }
    }
    return false;
}

bool http_cors_policy::allows_method(std::string_view method) const noexcept {
    if (!is_valid_http_method_token(method)) {
        return false;
    }
    if (allow_any_method_) {
        return true;
    }
    // HTTP method tokens are case-sensitive (RFC 9110 §9.1).
    return std::ranges::find(allowed_methods_, method) != allowed_methods_.end();
}

bool http_cors_policy::allows_request_header(std::string_view name) const noexcept {
    if (!is_valid_http_header_name(name)) {
        return false;
    }
    if (reflect_request_headers_) {
        return true;
    }
    return std::ranges::any_of(allowed_headers_, [name](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, name);
    });
}

bool http_cors_policy::collect_requested_headers(const http_header_range_view& fields, std::size_t maximum, std::string* reflected) const {
    std::size_t count = 0;
    for (const auto field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        http_field_name_list names(field.value());
        bool saw_name = false;
        while (const auto name = names.next()) {
            saw_name = true;
            if (count == maximum || *name == "*" || !allows_request_header(*name)) {
                return false;
            }
            ++count;
            if (reflected != nullptr) {
                if (!reflected->empty()) {
                    reflected->append(", ");
                }
                reflected->append(*name);
            }
        }
        if (!saw_name || !names.valid()) {
            return false;
        }
    }
    return true;
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(http_cors_preflight_input input, std::size_t max_requested_headers) const {
    const std::array fields{
        http_header_view("Origin", input.origin_.view()),
        http_header_view("Access-Control-Request-Method", input.access_control_request_method_.view()),
        http_header_view("Access-Control-Request-Headers", input.access_control_request_headers_.view()),
    };
    const auto count = http_trim_ows(input.access_control_request_headers_.view()).empty() ? 2U : 3U;
    return evaluate_preflight(input.method_, std::span(fields).first(count), max_requested_headers);
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(borrowed_text method, const http_header_range_view& fields, std::size_t max_requested_headers) const {
    const auto origin_field = find_unique_http_header(fields, "Origin");
    const auto method_field = find_unique_http_header(fields, "Access-Control-Request-Method");
    if (method.view() != "OPTIONS" || origin_field.status_ != unique_lookup_status::found || method_field.status_ != unique_lookup_status::found) {
        return {};
    }
    const auto origin = origin_field.value_;
    const auto requested_method = http_trim_ows(method_field.value_);
    if (origin.empty() || !allows_origin(origin) || requested_method.empty() || !allows_method(requested_method)) {
        return {};
    }

    std::string reflected_headers;
    if (!collect_requested_headers(fields, max_requested_headers, reflect_request_headers_ ? &reflected_headers : nullptr)) {
        return {};
    }

    static constexpr auto allow_origin_any_bytes = make_cors_header_bytes("Access-Control-Allow-Origin", "*");
    static constexpr auto allow_credentials_true_bytes = make_cors_header_bytes("Access-Control-Allow-Credentials", "true");
    static constexpr auto preflight_vary_bytes = make_cors_header_bytes("Vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");

    std::vector<http_header> headers;
    headers.reserve(6);
    if (allow_any_origin_) {
        headers.push_back(http_header(allow_origin_any_bytes.data(), 27U, 1U));
    } else {
        headers.emplace_back("Access-Control-Allow-Origin", origin);
    }
    if (allow_credentials_) {
        headers.push_back(http_header(allow_credentials_true_bytes.data(), 32U, 4U));
    }
    const std::string_view allow_methods = !joined_allowed_methods_.empty() ? std::string_view(joined_allowed_methods_) : requested_method;
    headers.emplace_back("Access-Control-Allow-Methods", allow_methods);
    if (reflect_request_headers_) {
        if (!reflected_headers.empty()) {
            headers.emplace_back("Access-Control-Allow-Headers", reflected_headers);
        }
    } else if (!joined_allowed_headers_.empty()) {
        headers.emplace_back("Access-Control-Allow-Headers", joined_allowed_headers_);
    }
    if (!max_age_.empty()) {
        headers.emplace_back("Access-Control-Max-Age", max_age_);
    }
    headers.push_back(http_header(preflight_vary_bytes.data(), 4U, 69U));

    return http_cors_preflight_result(true, std::move(headers));
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(const http_request& request, std::size_t max_requested_headers) const {
    return evaluate_preflight(request.method(), request.headers(), max_requested_headers);
}

bool http_cors_policy::rewrite_response_headers(borrowed_text request_origin, editable_http_headers response_headers) const {
    auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    // Removing or merging fields can invalidate an origin borrowed from the
    // same block. Snapshot only when the policy will echo those bytes.
    std::string retained_origin;
    if (granted && !allow_any_origin_) {
        for (const auto field : response_headers.fields()) {
            if (detail::http_text_overlaps(origin, field.name()) || detail::http_text_overlaps(origin, field.value())) {
                retained_origin.assign(origin);
                origin = retained_origin;
                break;
            }
        }
    }
    // Vary and up to three Access-Control-* lines are written: an
    // http_response takes all of them or none.
    response_headers.transact([this, origin, granted](editable_http_headers candidate) {
        candidate.remove_if(is_http_cors_controlled_response_header);
        // Even a wildcard grant depends on a valid Origin being present.
        merge_http_vary_token(candidate, "Origin");
        if (!granted) {
            return;
        }
        candidate.set("Access-Control-Allow-Origin", allow_any_origin_ ? std::string_view("*") : origin);
        if (allow_credentials_) {
            candidate.set("Access-Control-Allow-Credentials", "true");
        }
        if (!joined_expose_headers_.empty()) {
            candidate.set("Access-Control-Expose-Headers", joined_expose_headers_);
        }
    }, 4);
    return granted;
}

bool http_cors_policy::apply_response_headers(const http_request& request, http_response& response) const {
    const bool options = request.known_method() == http_known_method::options;
    // Even a wildcard policy withholds the grant for an invalid Origin.
    response.add_vary_token("Origin");
    if (options) {
        response.add_vary_token("Access-Control-Request-Method");
        // The requested names affect acceptance even with a fixed allowlist.
        response.add_vary_token("Access-Control-Request-Headers");
    }

    const auto origin_field = find_unique_http_header(request, "Origin");
    const auto method_field = find_unique_http_header(request, "Access-Control-Request-Method");
    if (origin_field.status_ == unique_lookup_status::repeated || (options && method_field.status_ == unique_lookup_status::repeated)) {
        return false;
    }
    const auto origin = origin_field.status_ == unique_lookup_status::found ? std::optional(origin_field.value_) : std::nullopt;
    if (origin.has_value() && !allows_origin(*origin)) {
        return false;
    }
    const auto requested_method = options && method_field.status_ == unique_lookup_status::found ? std::optional(method_field.value_) : std::nullopt;
    if (requested_method.has_value() && !allows_method(http_trim_ows(*requested_method))) {
        return false;
    }
    const bool preflight = options && origin.has_value() && requested_method.has_value();
    const bool reflect_headers = preflight && reflect_request_headers_ && !response.header("Access-Control-Allow-Headers").has_value();
    std::string reflected_headers;
    if (options && !collect_requested_headers(request.headers(), (std::numeric_limits<std::size_t>::max)(), reflect_headers ? &reflected_headers : nullptr)) {
        return false;
    }

    if (!response.header("Access-Control-Allow-Origin").has_value()) {
        if (allow_any_origin_) {
            response.header("Access-Control-Allow-Origin", "*");
        } else if (origin.has_value()) {
            response.header("Access-Control-Allow-Origin", *origin);
        } else if (exact_origins_.size() == 1 && suffix_rules_.empty()) {
            response.header("Access-Control-Allow-Origin", exact_origins_.front());
        }
    }
    if (allow_credentials_ && !response.header("Access-Control-Allow-Credentials").has_value()) {
        response.header("Access-Control-Allow-Credentials", "true");
    }

    if (preflight) {
        if (!response.header("Access-Control-Allow-Methods").has_value()) {
            if (!joined_allowed_methods_.empty()) {
                response.header("Access-Control-Allow-Methods", joined_allowed_methods_);
            } else if (const auto allow = response.header("Allow"); allow.has_value() && !allow->empty()) {
                response.header("Access-Control-Allow-Methods", *allow);
            } else {
                response.header("Access-Control-Allow-Methods", http_trim_ows(*requested_method));
            }
        }
        if (!response.header("Access-Control-Allow-Headers").has_value()) {
            if (!reflect_request_headers_ && !joined_allowed_headers_.empty()) {
                response.header("Access-Control-Allow-Headers", joined_allowed_headers_);
            } else if (reflect_request_headers_ && !reflected_headers.empty()) {
                response.header("Access-Control-Allow-Headers", reflected_headers);
            }
        }
        if (!max_age_.empty() && !response.header("Access-Control-Max-Age").has_value()) {
            response.header("Access-Control-Max-Age", max_age_);
        }
        return true;
    }

    if (!joined_expose_headers_.empty() && !response.header("Access-Control-Expose-Headers").has_value()) {
        response.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

}  // namespace ruvia
