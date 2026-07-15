#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/http/SessionInternal.h"
#include "ruvia/web/detail/http/CsrfInternal.h"
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

RUVIA_TEST(session_state_makes_persistence_decisions_exclusive) {
    ruvia::detail::ContextSessionState state(std::pmr::new_delete_resource());
    RUVIA_CHECK(state.untouched() != nullptr);

    state.observePresentedId("deadbeef");
    RUVIA_CHECK(state.unrecognized() != nullptr);
    state.set("user=1");
    RUVIA_CHECK(state.persistNew() != nullptr);
    RUVIA_CHECK_EQ(state.persistNew()->data, std::string_view("user=1"));

    ruvia::detail::ContextSessionState recognized(std::pmr::new_delete_resource());
    recognized.observePresentedId("abcdef");
    recognized.loadRecognized("user=2");
    RUVIA_CHECK(recognized.loaded() != nullptr);
    recognized.regenerate();
    RUVIA_CHECK(recognized.rotate() != nullptr);
    RUVIA_CHECK_EQ(recognized.rotate()->oldId, std::string_view("abcdef"));
    RUVIA_CHECK_EQ(recognized.rotate()->data, std::string_view("user=2"));
}

RUVIA_TEST(session_clear_never_requests_a_fresh_id) {
    ruvia::detail::ContextSessionState absent(std::pmr::new_delete_resource());
    absent.clear();
    RUVIA_CHECK(absent.cleared() != nullptr);
    RUVIA_CHECK(!absent.cleared()->oldId.has_value());
    RUVIA_CHECK(absent.persistNew() == nullptr);

    ruvia::detail::ContextSessionState recognized(std::pmr::new_delete_resource());
    recognized.observePresentedId("abcdef");
    recognized.loadRecognized("user=2");
    recognized.clear();
    RUVIA_CHECK(recognized.cleared() != nullptr);
    RUVIA_CHECK_EQ(*recognized.cleared()->oldId, std::string_view("abcdef"));
    RUVIA_CHECK(recognized.persistNew() == nullptr);
}

// The middleware deletes the server-side blob only when the cleared state still
// carries the presented id, so no later call may drop it. A second clear() must be
// idempotent -- otherwise logout silently degrades to expiring the cookie while the
// session stays live in storage for the rest of its TTL.
RUVIA_TEST(session_repeated_clear_keeps_the_id_to_delete) {
    ruvia::detail::ContextSessionState recognized(std::pmr::new_delete_resource());
    recognized.observePresentedId("abcdef");
    recognized.loadRecognized("user=2");
    recognized.clear();
    recognized.clear();
    RUVIA_CHECK(recognized.cleared() != nullptr);
    RUVIA_CHECK(recognized.cleared()->oldId.has_value());
    if (recognized.cleared()->oldId.has_value()) {
        RUVIA_CHECK_EQ(*recognized.cleared()->oldId, std::string_view("abcdef"));
    }

    // Clearing without a presented id stays a plain clear, not a rotation.
    ruvia::detail::ContextSessionState absent(std::pmr::new_delete_resource());
    absent.clear();
    absent.clear();
    RUVIA_CHECK(absent.cleared() != nullptr);
    RUVIA_CHECK(!absent.cleared()->oldId.has_value());
}

// clear() then set() is "drop the old session, start a fresh one". That is a
// rotation: the new id must be minted AND the old blob deleted. Landing in
// persistNew would mint the id but orphan the old blob.
RUVIA_TEST(session_clear_then_set_rotates_instead_of_orphaning) {
    ruvia::detail::ContextSessionState recognized(std::pmr::new_delete_resource());
    recognized.observePresentedId("abcdef");
    recognized.loadRecognized("user=2");
    recognized.clear();
    recognized.set("user=3");
    RUVIA_CHECK(recognized.rotate() != nullptr);
    RUVIA_CHECK(recognized.persistNew() == nullptr);
    if (recognized.rotate() != nullptr) {
        RUVIA_CHECK_EQ(recognized.rotate()->oldId, std::string_view("abcdef"));
        RUVIA_CHECK_EQ(recognized.rotate()->data, std::string_view("user=3"));
    }

    // With no presented id there is nothing to delete, so a fresh session is right.
    ruvia::detail::ContextSessionState absent(std::pmr::new_delete_resource());
    absent.clear();
    absent.set("user=4");
    RUVIA_CHECK(absent.persistNew() != nullptr);
    RUVIA_CHECK(absent.rotate() == nullptr);
    if (absent.persistNew() != nullptr) {
        RUVIA_CHECK_EQ(absent.persistNew()->data, std::string_view("user=4"));
    }
}

RUVIA_TEST(secure_token_generation_reports_failure_as_a_type) {
    char tooSmall[47];
    const auto failure = ruvia::detail::generateSecureToken(tooSmall);
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(failure.ready() == nullptr);

    char storage[48];
    const auto ready = ruvia::detail::generateSecureToken(storage);
    RUVIA_CHECK(ready.ready() != nullptr);
    RUVIA_CHECK_EQ(ready.ready()->value().size(), std::size_t{48});
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
