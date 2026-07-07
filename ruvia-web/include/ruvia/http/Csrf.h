#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"

namespace ruvia {

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without an XSRF-TOKEN cookie is issued a
// fresh one (readable by JavaScript so a SPA can echo it). An unsafe request
// must repeat that cookie's value in the X-XSRF-TOKEN header; a missing or
// mismatched token is rejected with 403. Register on a controller, group, or
// route that should enforce browser XSRF checks.
class CsrfProtection final : public Middleware<CsrfProtection> {
public:
    Task<void> handle(Context& c, Next& next);
};

}  // namespace ruvia
