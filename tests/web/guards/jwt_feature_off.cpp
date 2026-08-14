#include "ruvia/web/auth/Jwt.h"

void jwtFeatureOffMustNotCompile() {
    ruvia::JwtSignOptions options;
    (void)ruvia::jwtSign(options);
}
