#include "ruvia/web/detail/auth/JwtInternal.h"

#include "ruvia/http/detail/HttpNumberFormat.h"
#include "ruvia/web/detail/json/JsonObjectFields.h"
#include "ruvia/web/detail/json/JsonString.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ruvia {
namespace detail {

template <typename Visitor>
[[nodiscard]] bool visitUniqueJwtJsonObjectFields(
    std::string_view json,
    std::pmr::memory_resource* resource,
    Visitor&& visitor) {
    std::pmr::vector<std::pmr::string> names(resource);
    bool duplicate = false;
    const bool valid = visitJsonObjectFields(
        ResolvedPmrResourceTag{},
        json,
        resource,
        [&](std::string_view name, std::string_view value) {
            for (const auto& existing : names) {
                if (std::string_view(existing) == name) {
                    duplicate = true;
                    return false;
                }
            }
            names.emplace_back(name);
            visitor(name, value);
            return true;
        });
    return valid && !duplicate;
}

[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
jwtParseJsonNumericDate(std::string_view value) {
    skipJsonWhitespace(value);
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec == std::errc{}) {
        auto remaining = value.substr(static_cast<std::size_t>(ptr - value.data()));
        skipJsonWhitespace(remaining);
        if (remaining.empty()) {
            return jwtFromEpochSeconds(parsed);
        }
    }

    double fractional = 0;
    const auto [fractionalPtr, fractionalEc] = std::from_chars(
        value.data(), value.data() + value.size(), fractional);
    if (fractionalEc != std::errc{} || !std::isfinite(fractional)) {
        return std::nullopt;
    }
    auto remaining = value.substr(
        static_cast<std::size_t>(fractionalPtr - value.data()));
    skipJsonWhitespace(remaining);
    if (!remaining.empty()) {
        return std::nullopt;
    }

    using Clock = std::chrono::system_clock;
    const auto maxSeconds = std::chrono::duration<long double>(
        Clock::duration::max()).count();
    const auto minSeconds = std::chrono::duration<long double>(
        Clock::duration::min()).count();
    if (fractional >= maxSeconds) {
        return Clock::time_point::max();
    }
    if (fractional <= minSeconds) {
        return Clock::time_point::min();
    }
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<long double>(fractional)));
}

[[nodiscard]] std::optional<std::pmr::string> jwtDecodeJsonStringValue(
    std::string_view value,
    std::pmr::memory_resource* resource) {
    const auto parsed = parseJsonString(value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    skipJsonWhitespace(value);
    if (!value.empty()) {
        return std::nullopt;
    }

    if (parsed->encoding() == JsonStringEncoding::kLiteral) {
        return std::pmr::string(parsed->raw(), resource);
    }
    return decodeJsonString(parsed->raw(), resource);
}

void jwtAppendJsonEscaped(std::pmr::string& out, std::string_view value) {
    out.push_back('"');
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out.append("\\u00");
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
}

void jwtAppendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) { out.push_back(','); }
    first = false;
    jwtAppendJsonEscaped(out, name);
    out.push_back(':');
    jwtAppendJsonEscaped(out, value);
}

void jwtAppendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::int64_t value) {
    if (!first) { out.push_back(','); }
    first = false;
    jwtAppendJsonEscaped(out, name);
    out.push_back(':');
    appendHttpFormattedNumber(out, value, "failed to format JWT numeric claim");
}

std::pmr::string jwtParseJoseAlgorithm(
    std::string_view json,
    std::pmr::memory_resource* resource) {
    auto* const resolved = pmrResourceOrDefault(resource);
    std::pmr::string algorithm(resolved);
    bool algorithmSeen = false;
    bool algorithmValid = false;
    bool unsupportedCriticalHeader = false;
    const bool valid = visitUniqueJwtJsonObjectFields(
        json,
        resolved,
        [&](std::string_view name, std::string_view value) {
            if (name == "alg") {
                algorithmSeen = true;
                if (auto decoded = jwtDecodeJsonStringValue(value, resolved)) {
                    algorithm = std::move(*decoded);
                    algorithmValid = true;
                }
            } else if (name == "crit") {
                unsupportedCriticalHeader = true;
            }
        });
    if (!valid || !algorithmSeen || !algorithmValid ||
        unsupportedCriticalHeader) {
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
[[nodiscard]] bool jwtDecodeAudiences(
    std::pmr::vector<std::pmr::string>& out,
    std::string_view value,
    std::pmr::memory_resource* resource) {
    detail::skipJsonWhitespace(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() != '[') {
        if (auto single = detail::jwtDecodeJsonStringValue(value, resource)) {
            out.push_back(std::move(*single));
            return true;
        }
        return false;
    }

    value.remove_prefix(1);  // consume '['
    detail::skipJsonWhitespace(value);
    if (!value.empty() && value.front() == ']') {
        value.remove_prefix(1);
        detail::skipJsonWhitespace(value);
        return value.empty();  // an empty array carries no audience
    }
    for (;;) {
        detail::skipJsonWhitespace(value);
        const auto parsed = detail::parseJsonString(value);
        if (!parsed.has_value()) {
            out.clear();
            return false;  // a non-string array element is not a valid audience
        }
        std::optional<std::pmr::string> decoded;
        if (parsed->encoding() == detail::JsonStringEncoding::kEscaped) {
            decoded = detail::decodeJsonString(parsed->raw(), resource);
            if (!decoded.has_value()) {
                out.clear();
                return false;
            }
        } else {
            decoded.emplace(parsed->raw(), resource);
        }
        out.push_back(std::move(*decoded));

        detail::skipJsonWhitespace(value);
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
            detail::skipJsonWhitespace(value);
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

JwtPayload detail::JwtPayloadAccess::decodePayloadJson(std::string_view json, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    JwtPayload payload(resolved);

    bool registeredClaimInvalid = false;

    const bool valid = detail::visitUniqueJwtJsonObjectFields(
        json,
        resolved,
        [&](std::string_view key, std::string_view value) {
            if (key == "iss") {
                if (auto issuer = detail::jwtDecodeJsonStringValue(value, resolved)) {
                    payload.issuer_ = std::move(*issuer);
                } else {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (key == "sub") {
                if (auto subject = detail::jwtDecodeJsonStringValue(value, resolved)) {
                    payload.subject_ = std::move(*subject);
                } else {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (key == "aud") {
                if (!jwtDecodeAudiences(payload.audiences_, value, resolved)) {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (key == "jti") {
                if (auto id = detail::jwtDecodeJsonStringValue(value, resolved)) {
                    payload.id_ = std::move(*id);
                } else {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (key == "exp") {
                if (const auto exp = detail::jwtParseJsonNumericDate(value)) {
                    payload.expiresAt_ = *exp;
                } else {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (key == "nbf") {
                if (const auto nbf = detail::jwtParseJsonNumericDate(value)) {
                    payload.notBefore_ = *nbf;
                } else {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (key == "iat") {
                if (const auto iat = detail::jwtParseJsonNumericDate(value)) {
                    payload.issuedAt_ = *iat;
                } else {
                    registeredClaimInvalid = true;
                }
                return;
            }
            if (!detail::jwtIsReservedClaim(key)) {
                if (auto claimValue = detail::jwtDecodeJsonStringValue(value, resolved)) {
                    std::pmr::string claimName(resolved);
                    claimName.assign(key.data(), key.size());
                    payload.claims_.push_back(detail::JwtPayloadAccess::claim(
                        std::move(claimName),
                        std::move(*claimValue)));
                }
            }
        });
    if (!valid || registeredClaimInvalid) {
        throw std::runtime_error("JWT payload is not a valid unique claims object");
    }
    return payload;
}

}  // namespace ruvia
