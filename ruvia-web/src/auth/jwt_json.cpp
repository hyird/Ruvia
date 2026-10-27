#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <forward_list>
#include <functional>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/json/json_array_elements.h"
#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/json/json_string.h"
#include "ruvia/web/detail/util/decimal_number.h"

#include "auth/jwt_primitives.h"

namespace ruvia {
namespace detail {

// Duplicate names are detected once the object is complete; any duplicate
// rejects the object, so visiting it first changes nothing for callers.
template <typename visitor_type>
[[nodiscard]] bool visit_unique_jwt_json_object_fields(
    std::string_view json, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    // Literal names are views into json. Escaped names arrive decoded in storage
    // that lives for one visitor call, so only those are copied; list nodes keep
    // each copy at a stable address.
    static constexpr std::size_t inline_capacity = 16;
    std::array<std::string_view, inline_capacity> inline_names{};
    std::size_t inline_count = 0;
    std::pmr::vector<std::string_view> heap_names(resource);
    std::pmr::forward_list<std::pmr::string> decoded_names(resource);
    const std::less_equal<> not_after;
    const auto record_name = [&](std::string_view stored_name) {
        if (!heap_names.empty()) {
            heap_names.push_back(stored_name);
        } else if (inline_count < inline_capacity) {
            inline_names[inline_count++] = stored_name;
        } else {
            heap_names.reserve(inline_capacity * 2U);
            heap_names.assign(inline_names.begin(), inline_names.end());
            heap_names.push_back(stored_name);
        }
    };
    const auto visited = visit_json_object_fields(resolved_pmr_resource_tag{}, json, resource,
        [&](std::string_view name, std::string_view value) {
            if (not_after(json.data(), name.data()) &&
                not_after(name.data() + name.size(), json.data() + json.size())) {
                record_name(name);
            } else {
                record_name(decoded_names.emplace_front(name));
            }
            visitor(name, value);
        });
    if (visited != json_object_visit_result::complete) {
        return false;
    }
    if (heap_names.empty()) {
        const std::span<std::string_view> active(inline_names.data(), inline_count);
        std::ranges::sort(active);
        return std::ranges::adjacent_find(active) == active.end();
    }
    std::ranges::sort(heap_names);
    return std::ranges::adjacent_find(heap_names) == heap_names.end();
}

[[nodiscard]] std::optional<std::chrono::system_clock::time_point> jwt_parse_json_numeric_date(
    std::string_view value) {
    skip_json_whitespace(value);
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t parsed_value = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (ec == std::errc{}) {
        auto remaining = value.substr(static_cast<std::size_t>(ptr - value.data()));
        skip_json_whitespace(remaining);
        if (remaining.empty()) {
            return jwt_from_epoch_seconds(parsed_value);
        }
    }

    auto fractional_text = value;
    while (!fractional_text.empty() &&
           (fractional_text.back() == ' ' || fractional_text.back() == '\t' ||
               fractional_text.back() == '\r' || fractional_text.back() == '\n')) {
        fractional_text.remove_suffix(1);
    }
    const auto parsed_fractional = detail::parse_decimal_number<long double>(fractional_text);
    if (!parsed_fractional || !std::isfinite(*parsed_fractional)) {
        return std::nullopt;
    }

    const auto fractional = *parsed_fractional;

    using clock_type = std::chrono::system_clock;
    const auto max_seconds = std::chrono::duration<long double>(clock_type::duration::max()).count();
    const auto min_seconds = std::chrono::duration<long double>(clock_type::duration::min()).count();
    if (fractional >= max_seconds) {
        return clock_type::time_point::max();
    }
    if (fractional <= min_seconds) {
        return clock_type::time_point::min();
    }
    return clock_type::time_point(std::chrono::duration_cast<clock_type::duration>(
        std::chrono::duration<long double>(fractional)));
}

[[nodiscard]] std::optional<std::pmr::string> jwt_decode_json_string_token(
    const json_string_token& token, std::pmr::memory_resource* resource) {
    if (token.encoding() == json_string_encoding::literal) {
        return std::pmr::string(token.raw(), resource);
    }
    return decode_json_string(token.raw(), resource);
}

[[nodiscard]] std::optional<std::pmr::string> jwt_decode_json_string_value(
    std::string_view value, std::pmr::memory_resource* resource) {
    const auto parsed_value = parse_json_string(value);
    if (!parsed_value.has_value()) {
        return std::nullopt;
    }
    skip_json_whitespace(value);
    if (!value.empty()) {
        return std::nullopt;
    }
    return jwt_decode_json_string_token(*parsed_value, resource);
}

void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string<json_hex_case::lower>(out, name);
    out.push_back(':');
    append_json_string<json_hex_case::lower>(out, value);
}

void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::int64_t value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string<json_hex_case::lower>(out, name);
    out.push_back(':');
    http_append_formatted_number(out, value, "failed to format JWT numeric claim");
}

std::pmr::string jwt_parse_jose_algorithm(std::string_view json, std::pmr::memory_resource* resource) {
    std::pmr::string algorithm(resource);
    bool algorithm_seen = false;
    bool algorithm_valid = false;
    bool unsupported_extension = false;
    const bool valid = visit_unique_jwt_json_object_fields(
        json, resource, [&](std::string_view name, std::string_view value) {
            if (name == "alg") {
                algorithm_seen = true;
                if (auto decoded = jwt_decode_json_string_value(value, resource)) {
                    algorithm = std::move(*decoded);
                    algorithm_valid = true;
                }
            } else if (name == "crit" || name == "b64") {
                // No critical extensions are implemented. RFC 7797 section 7
                // forbids b64=false in JWTs; section 6 requires crit whenever
                // b64 is present, even when true. Reject this extension also
                // when crit is missing, rather than silently decoding a payload
                // whose header declares different encoding semantics.
                unsupported_extension = true;
            }
        });
    if (!valid || !algorithm_seen || !algorithm_valid || unsupported_extension) {
        throw jwt_error(jwt_error::code_type::invalid_header, "JWT JOSE header is invalid");
    }
    return algorithm;
}

}  // namespace detail

namespace {

// Decode the JWT "aud" claim (RFC 7519 §4.1.3): either a single JSON string or a
// JSON array of strings. Each decoded audience is appended to `out`. On any
// malformed structure (non-string element, unterminated array, trailing junk)
// `out` is cleared and false is returned, so verification fails closed rather
// than acting on a half-parsed list.
[[nodiscard]] bool jwt_decode_audiences(std::pmr::vector<std::pmr::string>& out,
    std::string_view value, std::pmr::memory_resource* resource) {
    detail::skip_json_whitespace(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() != '[') {
        if (auto single = detail::jwt_decode_json_string_value(value, resource)) {
            out.push_back(std::move(*single));
            return true;
        }
        return false;
    }

    const bool consumed = detail::consume_json_array_elements(value, 0,
        [&out, resource](std::string_view& element) {
            const auto parsed_value = detail::parse_json_string(element);
            if (!parsed_value.has_value()) {
                return false;
            }
            auto decoded = detail::jwt_decode_json_string_token(*parsed_value, resource);
            if (!decoded.has_value()) {
                return false;
            }
            out.push_back(std::move(*decoded));
            return true;
        });
    detail::skip_json_whitespace(value);
    // RFC 7519 §4.1.3: an "aud" claim with no value identifies nobody, so the
    // token is rejected rather than treated as unaudienced.
    if (!consumed || !value.empty() || out.empty()) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

bool detail::jwt_payload_access::decode_jwt_payload_claim(jwt_payload& payload_value, std::string_view key,
    std::string_view value, std::pmr::memory_resource* resolved) {
    if (key == "iss") {
        if (auto issuer = detail::jwt_decode_json_string_value(value, resolved)) {
            payload_value.issuer_ = std::move(*issuer);
            return true;
        }
        return false;
    }
    if (key == "sub") {
        if (auto subject = detail::jwt_decode_json_string_value(value, resolved)) {
            payload_value.subject_ = std::move(*subject);
            return true;
        }
        return false;
    }
    if (key == "aud") {
        return jwt_decode_audiences(payload_value.audiences_, value, resolved);
    }
    if (key == "jti") {
        if (auto id = detail::jwt_decode_json_string_value(value, resolved)) {
            payload_value.id_ = std::move(*id);
            return true;
        }
        return false;
    }
    if (key == "exp") {
        if (const auto exp = detail::jwt_parse_json_numeric_date(value)) {
            payload_value.expires_at_ = *exp;
            return true;
        }
        return false;
    }
    if (key == "nbf") {
        if (const auto nbf = detail::jwt_parse_json_numeric_date(value)) {
            payload_value.not_before_ = *nbf;
            return true;
        }
        return false;
    }
    if (key == "iat") {
        if (const auto iat = detail::jwt_parse_json_numeric_date(value)) {
            payload_value.issued_at_ = *iat;
            return true;
        }
        return false;
    }
    // A claim value can be any JSON value (RFC 7519 §2), and claims that are
    // not understood MUST be ignored (§4). The object visit already checked
    // the value's JSON syntax and the claim name's uniqueness; only string
    // values are exposed through claims(), every other value is ignored.
    auto string_value = value;
    detail::skip_json_whitespace(string_value);
    if (string_value.empty() || string_value.front() != '"') {
        return true;
    }
    if (auto claim_value = detail::jwt_decode_json_string_value(value, resolved)) {
        std::pmr::string claim_name(resolved);
        claim_name.assign(key.data(), key.size());
        payload_value.claims_.push_back(detail::jwt_payload_access::claim(
            std::move(claim_name), std::move(*claim_value)));
        return true;
    }
    return false;
}

jwt_payload detail::jwt_payload_access::decode_payload_json(
    std::string_view json, std::pmr::memory_resource* resolved) {
    jwt_payload payload_value(resolved);
    bool claim_invalid = false;
    const bool valid = detail::visit_unique_jwt_json_object_fields(
        json, resolved, [&](std::string_view key, std::string_view value) {
            if (!decode_jwt_payload_claim(payload_value, key, value, resolved)) {
                claim_invalid = true;
            }
        });
    if (!valid || claim_invalid) {
        throw jwt_error(
            jwt_error::code_type::invalid_payload, "JWT payload is not a valid unique claims object");
    }
    return payload_value;
}

}  // namespace ruvia
