#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/http/SessionInternal.h"
#include "ruvia/http/HttpResponse.h"

namespace {

ruvia::HttpResponse makeResponse() {
    return ruvia::HttpResponse(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(session_cookie_append_preserves_existing_set_cookie) {
    auto response = makeResponse();
    response.header("Set-Cookie", "theme=light", {.append = true});

    ruvia::detail::appendSessionCookieHeader(response,
                                            std::pmr::new_delete_resource(),
                                            "abcdef",
                                            false);

    const auto& headers = response.headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{2});
    auto it = headers.begin();
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("theme=light"));
    ++it;
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("sid=abcdef; Path=/; HttpOnly; SameSite=Lax"));
}

RUVIA_TEST(session_cookie_secure_flag_appended_for_secure_requests) {
    auto response = makeResponse();

    ruvia::detail::appendSessionCookieHeader(response,
                                            std::pmr::new_delete_resource(),
                                            "abcdef",
                                            true);

    const auto& headers = response.headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{1});
    const auto it = headers.begin();
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("sid=abcdef; Path=/; HttpOnly; Secure; SameSite=Lax"));
}

RUVIA_TEST(session_mint_decision_defends_fixation) {
    using ruvia::detail::sessionShouldMintNewId;

    // A brand-new session (no id yet) always gets a server-chosen id.
    RUVIA_CHECK(sessionShouldMintNewId(/*idEmpty=*/true, /*recognized=*/false, /*regen=*/false));
    // An id the client presented that was NOT found in the store is never adopted
    // (unrecognized-id fixation) -- a fresh id is minted.
    RUVIA_CHECK(sessionShouldMintNewId(/*idEmpty=*/false, /*recognized=*/false, /*regen=*/false));
    // A recognized session normally keeps its id (stable across requests).
    RUVIA_CHECK(!sessionShouldMintNewId(/*idEmpty=*/false, /*recognized=*/true, /*regen=*/false));
    // ...but regenerateSession() forces a fresh id even for a recognized session:
    // this is the defense against an attacker-owned *recognized* planted id.
    RUVIA_CHECK(sessionShouldMintNewId(/*idEmpty=*/false, /*recognized=*/true, /*regen=*/true));
}

RUVIA_TEST(session_id_validation_accepts_only_lowercase_hex) {
    using ruvia::detail::isValidSessionId;

    // A session id read back from the client's cookie is trusted enough to key
    // session storage, so the validator must be strict: non-empty, at most 128
    // chars, and lowercase hex only (the shape generateSessionId produces).
    RUVIA_CHECK(isValidSessionId("deadbeef"));
    RUVIA_CHECK(isValidSessionId("0123456789abcdef"));
    RUVIA_CHECK(isValidSessionId(std::string_view(std::string(128, 'a'))));  // exactly 128 is allowed

    RUVIA_CHECK(!isValidSessionId(""));                                      // empty
    RUVIA_CHECK(!isValidSessionId(std::string_view(std::string(129, 'a')))); // one past the max length
    RUVIA_CHECK(!isValidSessionId("DEADBEEF"));                             // uppercase hex is rejected
    RUVIA_CHECK(!isValidSessionId("deadbeeg"));                            // 'g' is not a hex digit
    RUVIA_CHECK(!isValidSessionId("dead beef"));                           // space
    RUVIA_CHECK(!isValidSessionId("../../etc"));                           // path-traversal shape can never validate
    RUVIA_CHECK(!isValidSessionId(std::string_view("dead\0beef", 9)));     // embedded NUL
}
