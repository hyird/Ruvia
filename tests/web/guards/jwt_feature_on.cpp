#include <string_view>

#include "ruvia/web/auth/Jwt.h"

#ifndef RUVIA_ENABLE_JWT
#error "ruvia::web must propagate RUVIA_ENABLE_JWT when JWT is enabled"
#endif

int main() {
    ruvia::JwtSignOptions signOptions;
    signOptions.secret = "feature-gate-secret";
    signOptions.subject = "feature-gate";

    const auto token = ruvia::jwtSign(signOptions);

    ruvia::JwtVerifyOptions verifyOptions;
    verifyOptions.secret = signOptions.secret;
    const auto payload = ruvia::jwtVerify(token, verifyOptions);
    return payload.subject() == std::string_view("feature-gate") ? 0 : 1;
}
