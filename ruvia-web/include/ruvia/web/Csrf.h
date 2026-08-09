#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

#include <stdexcept>

namespace ruvia {

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without a token cookie is issued a fresh
// one (readable by JavaScript so a SPA can echo it). An unsafe request must
// repeat that cookie's value in the header; a missing or mismatched token is
// rejected with 403. Register on a controller, group, or route that should
// enforce browser XSRF checks. Cookie and header names default to "XSRF-TOKEN"
// and "X-XSRF-TOKEN" and can be rebranded per app.
class CsrfProtection final : public Middleware<CsrfProtection> {
public:
    explicit CsrfProtection(::ruvia::BorrowedText cookieName = "XSRF-TOKEN", ::ruvia::BorrowedText headerName = "X-XSRF-TOKEN")
        : cookieName_(cookieName),
          headerName_(headerName) {
        if (!isValidHttpHeaderName(cookieName_.view())) {
            throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
        }
        if (!isValidHttpHeaderName(headerName_.view())) {
            throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
        }
    }

    Task<void> handle(Context& c, Next& next);

private:
    ::ruvia::BorrowedText cookieName_;
    ::ruvia::BorrowedText headerName_;
};

}  // namespace ruvia
