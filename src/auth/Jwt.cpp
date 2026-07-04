#include "ruvia/auth/Jwt.h"

#include "JwtInternal.h"

#include <stdexcept>

namespace ruvia {

JwtPayload::JwtPayload(std::pmr::memory_resource* resource)
    : issuer_(detail::pmrResourceOrDefault(resource)),
      subject_(issuer_.get_allocator().resource()),
      audience_(issuer_.get_allocator().resource()),
      id_(issuer_.get_allocator().resource()),
      claims_(issuer_.get_allocator().resource()) {}

std::string_view JwtPayload::issuer() const noexcept { return issuer_; }
std::string_view JwtPayload::subject() const noexcept { return subject_; }
std::string_view JwtPayload::audience() const noexcept { return audience_; }
std::string_view JwtPayload::id() const noexcept { return id_; }
std::optional<std::chrono::system_clock::time_point> JwtPayload::expiresAt() const noexcept { return expiresAt_; }
std::optional<std::chrono::system_clock::time_point> JwtPayload::notBefore() const noexcept { return notBefore_; }
std::optional<std::chrono::system_clock::time_point> JwtPayload::issuedAt() const noexcept { return issuedAt_; }
std::span<const JwtClaim> JwtPayload::claims() const noexcept { return {claims_.data(), claims_.size()}; }

std::optional<std::string_view> JwtPayload::claim(std::string_view name) const noexcept {
    for (const auto& item : claims_) {
        if (item.name() == name) {
            return item.value();
        }
    }
    return std::nullopt;
}

std::pmr::string jwtSign(const JwtSignOptions& options, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    const auto now = std::chrono::system_clock::now();
    std::pmr::string header(resolved);
    header.append("{\"alg\":");
    detail::jwtAppendJsonEscaped(header, detail::jwtAlgorithmName(options.algorithm));
    header.append(",\"typ\":\"JWT\"}");

    std::pmr::string payload(resolved);
    payload.push_back('{');
    bool first = true;
    if (!options.issuer.empty()) { detail::jwtAppendJsonMember(payload, first, "iss", options.issuer); }
    if (!options.subject.empty()) { detail::jwtAppendJsonMember(payload, first, "sub", options.subject); }
    if (!options.audience.empty()) { detail::jwtAppendJsonMember(payload, first, "aud", options.audience); }
    if (!options.id.empty()) { detail::jwtAppendJsonMember(payload, first, "jti", options.id); }
    detail::jwtAppendJsonMember(payload, first, "iat", detail::jwtEpochSeconds(now));
    if (options.expiresIn.count() > 0) {
        detail::jwtAppendJsonMember(payload, first, "exp", detail::jwtEpochSeconds(now + options.expiresIn));
    }
    if (options.notBeforeDelay.count() > 0) {
        detail::jwtAppendJsonMember(payload, first, "nbf", detail::jwtEpochSeconds(now + options.notBeforeDelay));
    }
    for (const auto& claim : options.claims) {
        if (claim.name().empty() || detail::jwtIsReservedClaim(claim.name())) {
            throw std::invalid_argument("JWT custom claim name is empty or reserved");
        }
        detail::jwtAppendJsonMember(payload, first, claim.name(), claim.value());
    }
    payload.push_back('}');

    auto encodedHeader = detail::jwtBase64UrlEncode(header, resolved);
    auto encodedPayload = detail::jwtBase64UrlEncode(payload, resolved);
    std::pmr::string signingInput(resolved);
    signingInput.append(encodedHeader);
    signingInput.push_back('.');
    signingInput.append(encodedPayload);
    auto signature = detail::jwtHmacSign(options.algorithm, options.secret, signingInput, resolved);
    signingInput.push_back('.');
    signingInput.append(signature);
    return signingInput;
}

JwtPayload jwtVerify(std::string_view token, const JwtVerifyOptions& options, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    const auto parts = detail::jwtSplitToken(token);
    const auto expected = detail::jwtHmacSign(
        options.algorithm,
        options.secret,
        parts.signingInput,
        resolved);
    if (!detail::jwtConstantTimeEquals(expected, parts.signature)) {
        throw std::runtime_error("JWT signature verification failed");
    }
    const auto header = detail::jwtBase64UrlDecode(parts.header, resolved);
    if (detail::jwtFindJsonString(header, "alg", resolved) != detail::jwtAlgorithmName(options.algorithm)) {
        throw std::runtime_error("JWT algorithm mismatch");
    }
    const auto payloadJson = detail::jwtBase64UrlDecode(parts.payload, resolved);
    auto payload = detail::JwtPayloadAccess::decodePayloadJson(payloadJson, resolved);
    const auto now = std::chrono::system_clock::now();
    if (options.requireExpiration && !payload.expiresAt()) {
        throw std::runtime_error("JWT token is missing exp claim");
    }
    if (payload.expiresAt() && now > *payload.expiresAt() + options.leeway) {
        throw std::runtime_error("JWT token is expired");
    }
    if (payload.notBefore() && now + options.leeway < *payload.notBefore()) {
        throw std::runtime_error("JWT token is not yet valid");
    }
    if (!options.issuer.empty() && payload.issuer() != options.issuer) {
        throw std::runtime_error("JWT issuer mismatch");
    }
    if (!options.subject.empty() && payload.subject() != options.subject) {
        throw std::runtime_error("JWT subject mismatch");
    }
    if (!options.audience.empty() && payload.audience() != options.audience) {
        throw std::runtime_error("JWT audience mismatch");
    }
    return payload;
}

JwtPayload jwtDecodeUnverified(std::string_view token, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    const auto parts = detail::jwtSplitToken(token);
    const auto payloadJson = detail::jwtBase64UrlDecode(parts.payload, resolved);
    return detail::JwtPayloadAccess::decodePayloadJson(payloadJson, resolved);
}

std::optional<std::string_view> jwtBearerToken(std::string_view authorization) noexcept {
    constexpr std::string_view prefix = "Bearer ";
    if (authorization.size() <= prefix.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto left = authorization[i] >= 'A' && authorization[i] <= 'Z' ? authorization[i] + 32 : authorization[i];
        const auto right = prefix[i] >= 'A' && prefix[i] <= 'Z' ? prefix[i] + 32 : prefix[i];
        if (left != right) {
            return std::nullopt;
        }
    }
    return authorization.substr(prefix.size());
}

}  // namespace ruvia
