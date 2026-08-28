#include "test_harness.h"

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/detail/http/SessionAccess.h"
#include "ruvia/web/detail/http/SecureToken.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Session.h"

namespace {

ruvia::HttpResponse makeResponse() {
    return ruvia::HttpResponse({.resource = std::pmr::new_delete_resource()});
}

class FailingAllocationResource final : public std::pmr::memory_resource {
public:
    void failAllocationAfterSuccessfulAllocations(std::size_t count) noexcept {
        failAfter_ = count;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (failAfter_.has_value()) {
            if (*failAfter_ == 0) {
                failAfter_.reset();
                throw std::bad_alloc();
            }
            --*failAfter_;
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::optional<std::size_t> failAfter_;
};

}  // namespace

#ifdef RUVIA_ENABLE_REDIS

RUVIA_TEST(session_middleware_rejects_invalid_config_before_use) {
    const auto rejection = [](const ruvia::SessionConfig& config) {
        try {
            const ruvia::SessionMiddleware middleware(config);
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string{};
    };

    auto config = ruvia::SessionConfig{};
    config.redisAlias.clear();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session Redis alias must not be empty"));

    config = ruvia::SessionConfig{};
    config.cookieName = "bad cookie";
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session cookie name must be a valid HTTP token"));

    config = ruvia::SessionConfig{};
    config.keyPrefix.clear();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session key prefix must not be empty"));

    config = ruvia::SessionConfig{};
    config.ttl = std::chrono::seconds::zero();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session TTL must be greater than zero"));
}

#endif

RUVIA_TEST(session_cookie_append_preserves_existing_set_cookie) {
    auto response = makeResponse();
    response.header("Set-Cookie", "theme=light", {.mode = ruvia::HttpResponseHeaderMode::kAppend});

    ruvia::detail::appendSessionCookieHeader(response, std::pmr::new_delete_resource(), "sid", "abcdef", false);

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

    ruvia::detail::appendSessionCookieHeader(response, std::pmr::new_delete_resource(), "sid", "abcdef", true);

    const auto& headers = response.headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{1});
    const auto it = headers.begin();
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("sid=abcdef; Path=/; HttpOnly; Secure; SameSite=Lax"));
}

RUVIA_TEST(session_persistence_plan_persists_replacement_before_deleting_old_id) {
    using ruvia::detail::sessionPersistencePlan;
    using ruvia::detail::SessionPersistenceStep;

    const auto fresh = sessionPersistencePlan("newid", {});
    RUVIA_CHECK_EQ(fresh.count, std::size_t{1});
    RUVIA_CHECK(fresh.steps[0] == SessionPersistenceStep::kPersistCurrent);

    const auto cleared = sessionPersistencePlan({}, "oldid");
    RUVIA_CHECK_EQ(cleared.count, std::size_t{1});
    RUVIA_CHECK(cleared.steps[0] == SessionPersistenceStep::kDeleteOld);

    // Rotation must write the replacement blob before deleting the old one.
    // Otherwise a Redis SETEX failure after DEL would log the user out by
    // destroying the recognized session before its successor exists.
    const auto rotated = sessionPersistencePlan("newid", "oldid");
    RUVIA_CHECK_EQ(rotated.count, std::size_t{2});
    RUVIA_CHECK(rotated.steps[0] == SessionPersistenceStep::kPersistCurrent);
    RUVIA_CHECK(rotated.steps[1] == SessionPersistenceStep::kDeleteOld);
}

RUVIA_TEST(session_commit_plan_publishes_new_cookie_after_storage_succeeds) {
    using ruvia::detail::sessionCommitPlan;
    using ruvia::detail::SessionCommitStep;

    const auto fresh = sessionCommitPlan("newid", {}, true);
    RUVIA_CHECK_EQ(fresh.count, std::size_t{2});
    RUVIA_CHECK(fresh.steps[0] == SessionCommitStep::kPersistCurrent);
    RUVIA_CHECK(fresh.steps[1] == SessionCommitStep::kPublishCurrentCookie);

    const auto existing = sessionCommitPlan("sameid", {}, false);
    RUVIA_CHECK_EQ(existing.count, std::size_t{1});
    RUVIA_CHECK(existing.steps[0] == SessionCommitStep::kPersistCurrent);

    // Rotation is the strictest ordering: write the replacement, delete the old
    // blob, then publish the new cookie. Any Redis failure before the final step
    // must leave the client's previous cookie untouched.
    const auto rotated = sessionCommitPlan("newid", "oldid", true);
    RUVIA_CHECK_EQ(rotated.count, std::size_t{3});
    RUVIA_CHECK(rotated.steps[0] == SessionCommitStep::kPersistCurrent);
    RUVIA_CHECK(rotated.steps[1] == SessionCommitStep::kDeleteOld);
    RUVIA_CHECK(rotated.steps[2] == SessionCommitStep::kPublishCurrentCookie);

    const auto noCurrent = sessionCommitPlan({}, "oldid", true);
    RUVIA_CHECK_EQ(noCurrent.count, std::size_t{1});
    RUVIA_CHECK(noCurrent.steps[0] == SessionCommitStep::kDeleteOld);
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

RUVIA_TEST(session_set_allocation_failure_preserves_old_id_state) {
    FailingAllocationResource resource;
    const std::string oldId(80, 'a');
    const std::string oldData(80, 'u');
    const std::string newData(4096, 'n');

    ruvia::detail::ContextSessionState loaded(&resource);
    loaded.observePresentedId(oldId);
    loaded.loadRecognized(oldData);
    RUVIA_CHECK(loaded.loaded() != nullptr);

    resource.failAllocationAfterSuccessfulAllocations(0);
    bool persistFailed = false;
    try {
        loaded.set(newData);
    } catch (const std::bad_alloc&) {
        persistFailed = true;
    }
    RUVIA_CHECK(persistFailed);
    RUVIA_CHECK(loaded.loaded() != nullptr);
    if (loaded.loaded() != nullptr) {
        RUVIA_CHECK_EQ(loaded.loaded()->id, std::string_view(oldId));
        RUVIA_CHECK_EQ(loaded.loaded()->data, std::string_view(oldData));
    }

    ruvia::detail::ContextSessionState cleared(&resource);
    cleared.observePresentedId(oldId);
    cleared.loadRecognized(oldData);
    cleared.clear();
    RUVIA_CHECK(cleared.cleared() != nullptr);

    resource.failAllocationAfterSuccessfulAllocations(0);
    bool rotateFailed = false;
    try {
        cleared.set(newData);
    } catch (const std::bad_alloc&) {
        rotateFailed = true;
    }
    RUVIA_CHECK(rotateFailed);
    RUVIA_CHECK(cleared.cleared() != nullptr);
    if (cleared.cleared() != nullptr && cleared.cleared()->oldId.has_value()) {
        RUVIA_CHECK_EQ(*cleared.cleared()->oldId, std::string_view(oldId));
    } else {
        RUVIA_CHECK(false);
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

    RUVIA_CHECK(!isValidSessionId(""));                                       // empty
    RUVIA_CHECK(!isValidSessionId(std::string_view(std::string(129, 'a'))));  // one past the max length
    RUVIA_CHECK(!isValidSessionId("DEADBEEF"));                               // uppercase hex is rejected
    RUVIA_CHECK(!isValidSessionId("deadbeeg"));                               // 'g' is not a hex digit
    RUVIA_CHECK(!isValidSessionId("dead beef"));                              // space
    RUVIA_CHECK(!isValidSessionId("../../etc"));                              // path-traversal shape can never validate
    RUVIA_CHECK(!isValidSessionId(std::string_view("dead\0beef", 9)));        // embedded NUL
}
