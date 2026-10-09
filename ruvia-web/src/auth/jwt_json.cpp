#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/decimal_number.h"
#include "ruvia/core/number_format.h"
#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/json/json_string.h"

#include "auth/jwt_primitives.h"

namespace ruvia {
namespace detail {

template <typename visitor_type>
[[nodiscard]] bool visit_unique_jwt_json_object_fields(
    std::string_view json, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    std::pmr::vector<std::pmr::string> names(resource);
    const auto visited = visit_json_object_fields(resolved_pmr_resource_tag{}, json, resource,
        [&](std::string_view name, std::string_view value) {
            for (const auto& existing : names) {
                if (std::string_view(existing) == name) {
                    return false;
                }
            }
            names.emplace_back(name);
            visitor(name, value);
            return true;
        });
    return visited == json_object_visit_result::complete;
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
    const auto parsed_fractional = ruvia::parse_decimal_number(fractional_text);
    if ((parsed_fractional.index() != 0) || !std::isfinite(std::get<0>(parsed_fractional))) {
        return std::nullopt;
    }

    const auto fractional = std::get<0>(parsed_fractional);

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

    if (parsed_value->encoding() == json_string_encoding::literal) {
        return std::pmr::string(parsed_value->raw(), resource);
    }
    return decode_json_string(parsed_value->raw(), resource);
}

void jwt_append_json_escaped(std::pmr::string& out, std::string_view value) {
    append_json_string<json_hex_case::lower>(out, value);
}
void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    jwt_append_json_escaped(out, name);
    out.push_back(':');
    jwt_append_json_escaped(out, value);
}

void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::int64_t value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    jwt_append_json_escaped(out, name);
    out.push_back(':');
    append_formatted_number(out, value, "failed to format JWT numeric claim");
}

std::pmr::string jwt_parse_jose_algorithm(std::string_view json, std::pmr::memory_resource* resource) {
    auto* const resolved = pmr_resource_or_default(resource);
    std::pmr::string algorithm(resolved);
    bool algorithm_seen = false;
    bool algorithm_valid = false;
    bool unsupported_extension = false;
    const bool valid = visit_unique_jwt_json_object_fields(
        json, resolved, [&](std::string_view name, std::string_view value) {
            if (name == "alg") {
                algorithm_seen = true;
                if (auto decoded = jwt_decode_json_string_value(value, resolved)) {
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
        throw std::runtime_error("JWT JOSE header is invalid");
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

    value.remove_prefix(1);  // consume '['
    detail::skip_json_whitespace(value);
    if (!value.empty() && value.front() == ']') {
        value.remove_prefix(1);
        detail::skip_json_whitespace(value);
        return value.empty();  // an empty array carries no audience
    }
    for (;;) {
        detail::skip_json_whitespace(value);
        const auto parsed_value = detail::parse_json_string(value);
        if (!parsed_value.has_value()) {
            out.clear();
            return false;  // a non-string array element is not a valid audience
        }
        std::optional<std::pmr::string> decoded;
        if (parsed_value->encoding() == detail::json_string_encoding::escaped) {
            decoded = detail::decode_json_string(parsed_value->raw(), resource);
            if (!decoded.has_value()) {
                out.clear();
                return false;
            }
        } else {
            decoded.emplace(parsed_value->raw(), resource);
        }
        out.push_back(std::move(*decoded));

        detail::skip_json_whitespace(value);
        if (value.empty()) {
            out.clear();  // unterminated array
            return false;
        }
        if (value.front() == ',') {
            value.remove_prefix(1);
            continue;
        }
        if (value.front() == ']') {
            value.remove_prefix(1);
            detail::skip_json_whitespace(value);
            if (!value.empty()) {
                out.clear();  // trailing junk after the array
                return false;
            }
            return true;
        }
        out.clear();  // malformed element separator
        return false;
    }
}

}  // namespace

jwt_payload detail::jwt_payload_access::decode_payload_json(
    std::string_view json, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmr_resource_or_default(resource);
    jwt_payload payload_value({.resource_ = resolved});

    bool registered_claim_invalid = false;

    const bool valid = detail::visit_unique_jwt_json_object_fields(
        json, resolved, [&](std::string_view key, std::string_view value) {
            if (key == "iss") {
                if (auto issuer = detail::jwt_decode_json_string_value(value, resolved)) {
                    payload_value.issuer_ = std::move(*issuer);
                } else {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (key == "sub") {
                if (auto subject = detail::jwt_decode_json_string_value(value, resolved)) {
                    payload_value.subject_ = std::move(*subject);
                } else {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (key == "aud") {
                if (!jwt_decode_audiences(payload_value.audiences_, value, resolved)) {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (key == "jti") {
                if (auto id = detail::jwt_decode_json_string_value(value, resolved)) {
                    payload_value.id_ = std::move(*id);
                } else {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (key == "exp") {
                if (const auto exp = detail::jwt_parse_json_numeric_date(value)) {
                    payload_value.expires_at_ = *exp;
                } else {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (key == "nbf") {
                if (const auto nbf = detail::jwt_parse_json_numeric_date(value)) {
                    payload_value.not_before_ = *nbf;
                } else {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (key == "iat") {
                if (const auto iat = detail::jwt_parse_json_numeric_date(value)) {
                    payload_value.issued_at_ = *iat;
                } else {
                    registered_claim_invalid = true;
                }
                return;
            }
            if (!detail::jwt_is_reserved_claim(key)) {
                if (auto claim_value = detail::jwt_decode_json_string_value(value, resolved)) {
                    std::pmr::string claim_name(resolved);
                    claim_name.assign(key.data(), key.size());
                    payload_value.claims_.push_back(detail::jwt_payload_access::claim(
                        std::move(claim_name), std::move(*claim_value)));
                }
            }
        });
    if (!valid || registered_claim_invalid) {
        throw std::runtime_error("JWT payload is not a valid unique claims object");
    }
    return payload_value;
}

}  // namespace ruvia
