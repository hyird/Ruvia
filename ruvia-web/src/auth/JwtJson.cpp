#include "ruvia/web/detail/auth/JwtInternal.h"

#include "ruvia/http/detail/HttpNumberFormat.h"
#include "ruvia/web/detail/json/JsonObjectFields.h"
#include "ruvia/web/detail/json/JsonString.h"

#include <charconv>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ruvia {
namespace detail {

[[nodiscard]] std::optional<std::int64_t> jwtParseJsonIntegerValue(std::string_view value) {
    skipJsonWhitespace(value);
    if (value.empty()) {
        return std::nullopt;
    }
    std::int64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr == value.data()) {
        return std::nullopt;
    }
    return parsed;
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

[[nodiscard]] std::optional<std::string_view> jwtRawJsonStringValue(std::string_view value) noexcept {
    const auto parsed = parseJsonString(value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    skipJsonWhitespace(value);
    if (!value.empty()) {
        return std::nullopt;
    }
    return parsed->raw();
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

std::string_view jwtFindJsonString(
    std::string_view json,
    std::string_view key,
    std::pmr::memory_resource* resource) {
    auto* const resolved = pmrResourceOrDefault(resource);
    std::string_view result;
    (void)visitJsonObjectFields(
        ResolvedPmrResourceTag{},
        json,
        resolved,
        [&](std::string_view member, std::string_view value) {
            if (member != key) {
                return true;
            }
            if (const auto raw = jwtRawJsonStringValue(value)) {
                result = *raw;
            }
            return false;
        });
    return result;
}

}  // namespace detail

namespace {

// Decode the JWT "aud" claim (RFC 7519 §4.1.3): either a single JSON string or a
// JSON array of strings. Each decoded audience is appended to `out`. On any
// malformed structure (non-string element, unterminated array, trailing junk)
// `out` is cleared, so the verifier sees an empty audience set and fails closed
// rather than acting on a half-parsed list.
void jwtDecodeAudiences(
    std::pmr::vector<std::pmr::string>& out,
    std::string_view value,
    std::pmr::memory_resource* resource) {
    detail::skipJsonWhitespace(value);
    if (value.empty()) {
        return;
    }
    if (value.front() != '[') {
        if (auto single = detail::jwtDecodeJsonStringValue(value, resource)) {
            out.push_back(std::move(*single));
        }
        return;
    }

    value.remove_prefix(1);  // consume '['
    detail::skipJsonWhitespace(value);
    if (!value.empty() && value.front() == ']') {
        return;  // an empty array carries no audience
    }
    for (;;) {
        const auto parsed = detail::parseJsonString(value);
        if (!parsed.has_value()) {
            out.clear();  // a non-string array element is not a valid audience
            return;
        }
        std::optional<std::pmr::string> decoded;
        if (parsed->encoding() == detail::JsonStringEncoding::kEscaped) {
            decoded = detail::decodeJsonString(parsed->raw(), resource);
            if (!decoded.has_value()) {
                out.clear();
                return;
            }
        } else {
            decoded.emplace(parsed->raw(), resource);
        }
        out.push_back(std::move(*decoded));

        detail::skipJsonWhitespace(value);
        if (value.empty()) {
            out.clear();  // unterminated array
            return;
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
            }
            return;
        }
        out.clear();  // malformed element separator
        return;
    }
}

}  // namespace

JwtPayload detail::JwtPayloadAccess::decodePayloadJson(std::string_view json, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    JwtPayload payload(resolved);

    bool issuerSeen = false;
    bool subjectSeen = false;
    bool audienceSeen = false;
    bool idSeen = false;
    bool expSeen = false;
    bool nbfSeen = false;
    bool iatSeen = false;

    const bool valid = detail::visitJsonObjectFields(
        detail::ResolvedPmrResourceTag{},
        json,
        resolved,
        [&](std::string_view key, std::string_view value) {
            if (key == "iss") {
                if (!issuerSeen) {
                    issuerSeen = true;
                    if (auto issuer = detail::jwtDecodeJsonStringValue(value, resolved)) {
                        payload.issuer_ = std::move(*issuer);
                    }
                }
                return true;
            }
            if (key == "sub") {
                if (!subjectSeen) {
                    subjectSeen = true;
                    if (auto subject = detail::jwtDecodeJsonStringValue(value, resolved)) {
                        payload.subject_ = std::move(*subject);
                    }
                }
                return true;
            }
            if (key == "aud") {
                if (!audienceSeen) {
                    audienceSeen = true;
                    jwtDecodeAudiences(payload.audiences_, value, resolved);
                }
                return true;
            }
            if (key == "jti") {
                if (!idSeen) {
                    idSeen = true;
                    if (auto id = detail::jwtDecodeJsonStringValue(value, resolved)) {
                        payload.id_ = std::move(*id);
                    }
                }
                return true;
            }
            if (key == "exp") {
                if (!expSeen) {
                    expSeen = true;
                    if (const auto exp = detail::jwtParseJsonIntegerValue(value)) {
                        payload.expiresAt_ = detail::jwtFromEpochSeconds(*exp);
                    }
                }
                return true;
            }
            if (key == "nbf") {
                if (!nbfSeen) {
                    nbfSeen = true;
                    if (const auto nbf = detail::jwtParseJsonIntegerValue(value)) {
                        payload.notBefore_ = detail::jwtFromEpochSeconds(*nbf);
                    }
                }
                return true;
            }
            if (key == "iat") {
                if (!iatSeen) {
                    iatSeen = true;
                    if (const auto iat = detail::jwtParseJsonIntegerValue(value)) {
                        payload.issuedAt_ = detail::jwtFromEpochSeconds(*iat);
                    }
                }
                return true;
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
            return true;
        });
    if (!valid) {
        throw std::runtime_error("JWT payload is not a JSON object");
    }
    return payload;
}

}  // namespace ruvia
