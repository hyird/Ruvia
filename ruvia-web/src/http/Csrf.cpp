#include "ruvia/web/Csrf.h"

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/detail/http/SecureToken.h"
#include "ruvia/web/detail/util/RegistrationResource.h"

#include <array>
#include <cstddef>
#include <stdexcept>

#include <openssl/rand.h>

#include "ruvia/http/detail/util/Hex.h"

namespace ruvia::detail {

SecureTokenResult generateSecureToken(std::span<char> buffer) noexcept {
    constexpr std::size_t kRandomBytes = 24;  // 24 bytes -> 48 hex characters
    if (buffer.size() < kRandomBytes * 2) {
        return SecureTokenResult::makeFailure();
    }
    unsigned char raw[kRandomBytes];
    if (RAND_bytes(raw, static_cast<int>(kRandomBytes)) != 1) {
        return SecureTokenResult::makeFailure();
    }
    for (std::size_t i = 0; i < kRandomBytes; ++i) {
        buffer[i * 2] = lowerHexDigit(raw[i] >> 4);
        buffer[i * 2 + 1] = lowerHexDigit(raw[i]);
    }
    return SecureTokenResult::makeReady(std::string_view(buffer.data(), kRandomBytes * 2));
}

}  // namespace ruvia::detail

namespace ruvia {

CsrfProtection::ConfigStorage::ValidatedConfig CsrfProtection::ConfigStorage::validate(
    const CsrfProtectionConfig& source) {
    if (!isValidHttpHeaderName(source.cookieName)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!isValidHttpHeaderName(source.headerName)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    return ValidatedConfig{.source = &source};
}

CsrfProtection::ConfigStorage::ConfigStorage(
    const CsrfProtectionConfig& source, std::pmr::memory_resource* resource)
    : ConfigStorage(validate(source), resource) {}

CsrfProtection::ConfigStorage::ConfigStorage(
    ValidatedConfig validated, std::pmr::memory_resource* resource)
    : cookieName(validated.source->cookieName, resource),
      headerName(validated.source->headerName, resource) {}

CsrfProtection::CsrfProtection()
    : CsrfProtection(CsrfProtectionConfig{}) {}

CsrfProtection::CsrfProtection(const CsrfProtectionConfig& config)
    : config_(config, detail::registrationResource()) {}

Task<void> CsrfProtection::handle(Context& c, Next& next) {
    const auto method = c.req().knownMethod();
    const bool safe = method == HttpKnownMethod::kGet || method == HttpKnownMethod::kHead ||
                      method == HttpKnownMethod::kOptions;
    const auto cookie = c.req().cookie(config_.cookieName);
    if (!safe) {
        const auto header = c.req().header(config_.headerName);
        if (!cookie || cookie->empty() || !header || header->empty() ||
            !detail::csrfTokensEqual(*cookie, *header)) {
            c.respond(c.error({.status = ruvia::http_status::kForbidden,
                .code = "csrf_token_mismatch",
                .message = "CSRF token missing or invalid"}));
            co_return;
        }
    } else if (!cookie || cookie->empty()) {
        // Reseed on an absent OR empty cookie. The unsafe path above already
        // rejects an empty cookie as invalid, so if the safe path only reissued
        // when the cookie was fully absent, a present-but-empty "XSRF-TOKEN="
        // would never be repaired: every safe request would leave it empty and
        // every unsafe request would 403 on it -- a permanent wedge. Treating
        // absent and empty identically here keeps the issue and validation sides
        // of the double-submit symmetric.
        std::array<char, 64> buffer;
        const auto tokenResult = detail::generateSecureToken(buffer);
        const auto* token = tokenResult.ready();
        if (token == nullptr) {
            c.respond(c.error({.status = ruvia::http_status::kInternalServerError,
                .code = "secure_random_failed",
                .message = "secure token generation failed"}));
            co_return;
        }
        const auto connection = getConnInfo(c);
        const CookieOptions options{
            .path = "/",
            .sameSite = CookieSameSite::kLax,
            .secure = connection.tls() != nullptr ? CookieAttributePolicy::kEmit
                                                  : CookieAttributePolicy::kOmit,
        };
        c.setCookie({.name = config_.cookieName, .value = token->value(), .attributes = options});
    }
    co_await next();
}

}  // namespace ruvia
