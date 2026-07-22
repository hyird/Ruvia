#include "ruvia/web/Context.h"

#include <chrono>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/cookie/SetCookiePlan.h"
#include "ruvia/web/detail/auth/CookieSignature.h"

// Setting response cookies, including the two rules that make a cookie's name on
// the wire differ from the name the application used: a __Host-/__Secure- prefix
// becomes part of the name, and a signed cookie's MAC covers that wire name.

namespace ruvia {
namespace {

// The name the client sends back in Cookie is the wire name: an enum prefix
// becomes part of the name at serialization. Request-side lookups and the MAC
// of a signed cookie must both use it; the bare name never reaches the client.
[[nodiscard]] std::string_view cookieWireName(
    std::pmr::string& storage,
    std::string_view name,
    const ruvia::CookieOptions& options) {
    if (!options.prefix) {
        return name;
    }
    const auto prefix = ruvia::detail::cookiePrefixText(*options.prefix);
    storage.reserve(prefix.size() + name.size());
    storage.append(prefix.data(), prefix.size());
    storage.append(name.data(), name.size());
    return storage;
}

[[nodiscard]] std::pmr::string composeSignedCookieValue(
    std::pmr::memory_resource* resource,
    std::string_view name,
    std::string_view value,
    std::string_view secret) {
    std::pmr::string signedValue(resource);
    signedValue.reserve(value.size() + 1 + detail::kCookieSignatureSize);
    if (!value.empty()) {
        signedValue.append(value.data(), value.size());
    }
    signedValue.push_back('.');
    char signature[detail::kCookieSignatureSize];
    detail::writeCookieSignature(signature, secret, name, value);
    signedValue.append(signature, sizeof(signature));
    return signedValue;
}

}  // namespace

void Context::setCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
    const detail::SetCookiePlan plan(name, value, options);
    auto& header = detail::upsertResponseSetCookieUninitializedValue(
        responseState_.activeResponse(),
        plan.wirePrefix(),
        plan.name(),
        plan.size());
    plan.write(detail::responseHeaderValueBegin(header));
}

void Context::setSignedCookie(
    std::string_view name,
    std::string_view value,
    std::string_view secret,
    const CookieOptions& options) {
    std::pmr::string wireName(resource());
    setCookie(
        name,
        composeSignedCookieValue(
            resource(),
            cookieWireName(wireName, name, options),
            value,
            secret),
        options);
}

void Context::deleteCookie(std::string_view name, CookieOptions options) {
    options.maxAge = std::chrono::seconds(0);
    setCookie(name, "", options);
}

}  // namespace ruvia
