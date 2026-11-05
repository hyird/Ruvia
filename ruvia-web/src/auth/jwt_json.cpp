#include <algorithm>
#include <array>
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

#include "ruvia/web/detail/json/json_array_elements.h"
#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/json/json_string.h"

#include "auth/jwt_date.h"
#include "auth/jwt_primitives.h"

namespace ruvia {
namespace detail {

// Duplicate names are detected once the object is complete; any duplicate
// rejects the object, so visiting each field on demand changes nothing for
// callers while avoiding a pre-pass skip and field slice buffering.
template <typename visitor_type>
[[nodiscard]] bool visit_unique_jwt_json_object_fields(
    std::string_view json, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    // Literal names are views into json. Escaped names arrive decoded in storage
    // that lives for one visitor call, so only those are copied; list nodes keep
    // each copy at a stable address.
    static constexpr std::size_t inline_capacity = 16;
    std::array<std::string_view, inline_capacity> inline_names;
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
    auto input = json;
    auto& visitor_ref = visitor;
    const bool consumed = consume_json_object_fields(resolved_pmr_resource_tag{}, input, resource, 0,
        [&](std::string_view name, std::string_view& value_input) {
            if (not_after(json.data(), name.data()) &&
                not_after(name.data() + name.size(), json.data() + json.size())) {
                record_name(name);
            } else {
                record_name(decoded_names.emplace_front(name));
            }
            return static_cast<bool>(visitor_ref(name, value_input));
        });
    if (!consumed) {
        return false;
    }
    skip_json_whitespace(input);
    if (!input.empty()) {
        return false;
    }
    if (heap_names.empty()) {
        if (inline_count <= 8) {
            for (std::size_t i = 1; i < inline_count; ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    if (inline_names[i] == inline_names[j]) {
                        return false;
                    }
                }
            }
        } else {
            const std::span<std::string_view> active_names(inline_names.data(), inline_count);
            std::ranges::sort(active_names);
            if (std::ranges::adjacent_find(active_names) != active_names.end()) {
                return false;
            }
        }
    } else {
        std::ranges::sort(heap_names);
        if (std::ranges::adjacent_find(heap_names) != heap_names.end()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::string_view> jwt_parse_json_numeric_date(std::string_view& value) noexcept {
    skip_json_whitespace(value);
    const auto length = scan_json_number_token_length(value);
    if (length == 0) {
        return std::nullopt;
    }
    const auto number = value.substr(0, length);
    if (!jwt_is_numeric_date(number)) {
        return std::nullopt;
    }
    value.remove_prefix(length);
    return number;
}

[[nodiscard]] std::optional<std::pmr::string> jwt_decode_json_string_value(
    std::string_view& value, std::pmr::memory_resource* resource) {
    const auto parsed_value = parse_json_string(value);
    if (!parsed_value.has_value()) {
        return std::nullopt;
    }
    return decode_json_string(*parsed_value, resource);
}

void jwt_append_json_member(
    jwt_json_buffer& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string<json_hex_case::lower>(out, name);
    out.push_back(':');
    append_json_string<json_hex_case::lower>(out, value);
}

void jwt_append_json_numeric_date(
    jwt_json_buffer& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string<json_hex_case::lower>(out, name);
    out.push_back(':');
    out.append(value);
}

void jwt_validate_jose_header(std::string_view json,
    std::optional<std::string_view> expected_algorithm, std::pmr::memory_resource* resource) {
    std::optional<std::string_view> algorithm;
    std::pmr::string decoded_algorithm(resource);
    bool unsupported_extension = false;
    const bool valid = visit_unique_jwt_json_object_fields(
        json, resource, [&](std::string_view name, std::string_view& value) {
            if (name == "alg") {
                const auto parsed = parse_json_string(value);
                if (!parsed.has_value()) {
                    return false;
                }
                if (parsed->encoding() == json_string_encoding::literal) {
                    algorithm = parsed->raw();
                } else {
                    decoded_algorithm = decode_json_string(*parsed, resource);
                    algorithm = decoded_algorithm;
                }
                return true;
            }
            if (name == "crit" || name == "b64") {
                // No critical extensions are implemented. RFC 7797 section 7
                // forbids b64=false in JWTs; section 6 requires crit whenever
                // b64 is present, even when true. Reject this extension also
                // when crit is missing, rather than silently decoding a payload
                // whose header declares different encoding semantics.
                unsupported_extension = true;
                return false;
            }
            return skip_json_value(value, 1);
        });
    if (!valid || !algorithm.has_value() || unsupported_extension) {
        throw jwt_error(jwt_error::code_type::invalid_header, "JWT JOSE header is invalid");
    }
    if (expected_algorithm.has_value()) {
        if (*algorithm != *expected_algorithm) {
            throw jwt_error(jwt_error::code_type::algorithm_mismatch, "JWT algorithm mismatch");
        }
    } else if (algorithm->empty()) {
        throw jwt_error(jwt_error::code_type::invalid_header, "JWT JOSE header is invalid");
    }
}

}  // namespace detail

namespace {

// Decode the JWT "aud" claim (RFC 7519 §4.1.3): either a single JSON string or a
// JSON array of strings. Each decoded audience is appended to `out`. On any
// malformed structure (non-string element, unterminated array, trailing junk)
// `out` is cleared and false is returned, so verification fails closed rather
// than acting on a half-parsed list.
[[nodiscard]] bool jwt_decode_audiences(std::pmr::vector<std::pmr::string>& out,
    std::string_view& value, std::pmr::memory_resource* resource) {
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

    const bool consumed = detail::consume_json_array_elements(value, 1,
        [&out, resource](std::string_view& element) {
            if (auto decoded = detail::jwt_decode_json_string_value(element, resource)) {
                out.push_back(std::move(*decoded));
                return true;
            }
            return false;
        });
    if (!consumed || out.empty()) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

bool detail::jwt_payload_access::decode_jwt_payload_claim(jwt_payload& payload_value, std::string_view key,
    std::string_view& value, std::uint8_t& seen_reserved_mask, std::pmr::memory_resource* resolved) {
    if (const auto bit = detail::jwt_reserved_claim_bit(key); bit != 0) {
        if ((seen_reserved_mask & bit) != 0) {
            return false;
        }
        seen_reserved_mask |= bit;
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
                payload_value.expires_at_.emplace(jwt_numeric_date(*exp, resolved));
                return true;
            }
            return false;
        }
        if (key == "nbf") {
            if (const auto nbf = detail::jwt_parse_json_numeric_date(value)) {
                payload_value.not_before_.emplace(jwt_numeric_date(*nbf, resolved));
                return true;
            }
            return false;
        }
        if (const auto iat = detail::jwt_parse_json_numeric_date(value)) {
            payload_value.issued_at_.emplace(jwt_numeric_date(*iat, resolved));
            return true;
        }
        return false;
    }
    // A claim value can be any JSON value (RFC 7519 §2), and claims that are
    // not understood MUST be ignored (§4). Only string values are exposed
    // through claims(); every other valid JSON value is skipped at depth 1.
    detail::skip_json_whitespace(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return detail::skip_json_value(value, 1);
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
    std::uint8_t seen_reserved_mask = 0;
    const bool valid = detail::visit_unique_jwt_json_object_fields(
        json, resolved, [&](std::string_view key, std::string_view& value) {
            return decode_jwt_payload_claim(payload_value, key, value, seen_reserved_mask, resolved);
        });
    if (!valid) {
        throw jwt_error(
            jwt_error::code_type::invalid_payload, "JWT payload is not a valid unique claims object");
    }
    return payload_value;
}

}  // namespace ruvia
