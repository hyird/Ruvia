#include "ruvia/web/session.h"

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_response.h"

#include "http/secure_token.h"
#include "http/session_access.h"
#include "test_harness.h"

namespace {

ruvia::http_response make_response() {
    return ruvia::http_response({.resource_ = std::pmr::new_delete_resource()});
}

class failing_allocation_resource final : public std::pmr::memory_resource {
public:
    void fail_allocation_after_successful_allocations(std::size_t count) noexcept {
        fail_after_ = count;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_after_.has_value()) {
            if (*fail_after_ == 0) {
                fail_after_.reset();
                throw std::bad_alloc();
            }
            --*fail_after_;
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::optional<std::size_t> fail_after_;
};

}  // namespace

#ifdef RUVIA_ENABLE_REDIS

RUVIA_TEST(session_middleware_rejects_invalid_config_before_use) {
    const auto rejection = [](const ruvia::session_config& config) {
        try {
            const ruvia::session_middleware middleware(config);
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string{};
    };

    auto config = ruvia::session_config{};
    config.redis_alias_.clear();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session Redis alias must not be empty"));

    config = ruvia::session_config{};
    config.cookie_name_ = "bad cookie";
    RUVIA_CHECK_EQ(
        rejection(config), std::string_view("session cookie name must be a valid HTTP token"));

    config = ruvia::session_config{};
    config.key_prefix_.clear();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session key prefix must not be empty"));

    config = ruvia::session_config{};
    config.ttl_ = std::chrono::seconds::zero();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session TTL must be greater than zero"));
}

#endif

RUVIA_TEST(session_cookie_append_preserves_existing_set_cookie) {
    auto response = make_response();
    response.header("Set-Cookie", "theme=light", {.mode_ = ruvia::http_response_header_mode::append});

    ruvia::detail::append_session_cookie_header(
        response, std::pmr::new_delete_resource(), "sid", "abcdef", false);

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
    auto response = make_response();

    ruvia::detail::append_session_cookie_header(
        response, std::pmr::new_delete_resource(), "sid", "abcdef", true);

    const auto& headers = response.headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{1});
    const auto it = headers.begin();
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(
        it->value(), std::string_view("sid=abcdef; Path=/; HttpOnly; Secure; SameSite=Lax"));
}

RUVIA_TEST(session_state_makes_persistence_decisions_exclusive) {
    ruvia::detail::context_session_state state(std::pmr::new_delete_resource());
    RUVIA_CHECK(state.untouched() != nullptr);

    state.observe_presented_id("deadbeef");
    RUVIA_CHECK(state.unrecognized() != nullptr);
    state.set("user=1");
    RUVIA_CHECK(state.persist_new() != nullptr);
    RUVIA_CHECK_EQ(state.persist_new()->data_, std::string_view("user=1"));

    ruvia::detail::context_session_state recognized(std::pmr::new_delete_resource());
    recognized.observe_presented_id("abcdef");
    recognized.load_recognized("user=2");
    RUVIA_CHECK(recognized.loaded() != nullptr);
    recognized.regenerate();
    RUVIA_CHECK(recognized.rotate() != nullptr);
    RUVIA_CHECK_EQ(recognized.rotate()->old_id_, std::string_view("abcdef"));
    RUVIA_CHECK_EQ(recognized.rotate()->data_, std::string_view("user=2"));
}

RUVIA_TEST(session_clear_never_requests_a_fresh_id) {
    ruvia::detail::context_session_state absent(std::pmr::new_delete_resource());
    absent.clear();
    RUVIA_CHECK(absent.cleared() != nullptr);
    RUVIA_CHECK(!absent.cleared()->old_id_.has_value());
    RUVIA_CHECK(absent.persist_new() == nullptr);

    ruvia::detail::context_session_state recognized(std::pmr::new_delete_resource());
    recognized.observe_presented_id("abcdef");
    recognized.load_recognized("user=2");
    recognized.clear();
    RUVIA_CHECK(recognized.cleared() != nullptr);
    RUVIA_CHECK_EQ(*recognized.cleared()->old_id_, std::string_view("abcdef"));
    RUVIA_CHECK(recognized.persist_new() == nullptr);
}

// The middleware deletes the server-side blob only when the cleared state still
// carries the presented id, so no later call may drop it. A second clear() must be
// idempotent -- otherwise logout silently degrades to expiring the cookie while the
// session stays live in storage for the rest of its TTL.
RUVIA_TEST(session_repeated_clear_keeps_the_id_to_delete) {
    ruvia::detail::context_session_state recognized(std::pmr::new_delete_resource());
    recognized.observe_presented_id("abcdef");
    recognized.load_recognized("user=2");
    recognized.clear();
    recognized.clear();
    RUVIA_CHECK(recognized.cleared() != nullptr);
    RUVIA_CHECK(recognized.cleared()->old_id_.has_value());
    if (recognized.cleared()->old_id_.has_value()) {
        RUVIA_CHECK_EQ(*recognized.cleared()->old_id_, std::string_view("abcdef"));
    }

    // Clearing without a presented id stays a plain clear, not a rotation.
    ruvia::detail::context_session_state absent(std::pmr::new_delete_resource());
    absent.clear();
    absent.clear();
    RUVIA_CHECK(absent.cleared() != nullptr);
    RUVIA_CHECK(!absent.cleared()->old_id_.has_value());
}

// clear() then set() is "drop the old session, start a fresh one". That is a
// rotation: the new id must be minted AND the old blob deleted. Landing in
// persist_new would mint the id but orphan the old blob.
RUVIA_TEST(session_clear_then_set_rotates_instead_of_orphaning) {
    ruvia::detail::context_session_state recognized(std::pmr::new_delete_resource());
    recognized.observe_presented_id("abcdef");
    recognized.load_recognized("user=2");
    recognized.clear();
    recognized.set("user=3");
    RUVIA_CHECK(recognized.rotate() != nullptr);
    RUVIA_CHECK(recognized.persist_new() == nullptr);
    if (recognized.rotate() != nullptr) {
        RUVIA_CHECK_EQ(recognized.rotate()->old_id_, std::string_view("abcdef"));
        RUVIA_CHECK_EQ(recognized.rotate()->data_, std::string_view("user=3"));
    }

    // With no presented id there is nothing to delete, so a fresh session is right.
    ruvia::detail::context_session_state absent(std::pmr::new_delete_resource());
    absent.clear();
    absent.set("user=4");
    RUVIA_CHECK(absent.persist_new() != nullptr);
    RUVIA_CHECK(absent.rotate() == nullptr);
    if (absent.persist_new() != nullptr) {
        RUVIA_CHECK_EQ(absent.persist_new()->data_, std::string_view("user=4"));
    }
}

RUVIA_TEST(session_set_allocation_failure_preserves_old_id_state) {
    failing_allocation_resource resource;
    const std::string old_id(80, 'a');
    const std::string old_data(80, 'u');
    const std::string new_data(4096, 'n');

    ruvia::detail::context_session_state loaded(&resource);
    loaded.observe_presented_id(old_id);
    loaded.load_recognized(old_data);
    RUVIA_CHECK(loaded.loaded() != nullptr);

    resource.fail_allocation_after_successful_allocations(0);
    bool persist_failed = false;
    try {
        loaded.set(new_data);
    } catch (const std::bad_alloc&) {
        persist_failed = true;
    }
    RUVIA_CHECK(persist_failed);
    RUVIA_CHECK(loaded.loaded() != nullptr);
    if (loaded.loaded() != nullptr) {
        RUVIA_CHECK_EQ(loaded.loaded()->id_, std::string_view(old_id));
        RUVIA_CHECK_EQ(loaded.loaded()->data_, std::string_view(old_data));
    }

    ruvia::detail::context_session_state cleared(&resource);
    cleared.observe_presented_id(old_id);
    cleared.load_recognized(old_data);
    cleared.clear();
    RUVIA_CHECK(cleared.cleared() != nullptr);

    resource.fail_allocation_after_successful_allocations(0);
    bool rotate_failed = false;
    try {
        cleared.set(new_data);
    } catch (const std::bad_alloc&) {
        rotate_failed = true;
    }
    RUVIA_CHECK(rotate_failed);
    RUVIA_CHECK(cleared.cleared() != nullptr);
    if (cleared.cleared() != nullptr && cleared.cleared()->old_id_.has_value()) {
        RUVIA_CHECK_EQ(*cleared.cleared()->old_id_, std::string_view(old_id));
    } else {
        RUVIA_CHECK(false);
    }
}

RUVIA_TEST(secure_token_generation_reports_failure_as_a_type) {
    char too_small[47];
    const auto failure = ruvia::detail::generate_secure_token(too_small);
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(failure.ready() == nullptr);

    char storage[48];
    const auto ready = ruvia::detail::generate_secure_token(storage);
    RUVIA_CHECK(ready.ready() != nullptr);
    RUVIA_CHECK_EQ(ready.ready()->value().size(), std::size_t{48});
}

RUVIA_TEST(session_id_validation_accepts_only_lowercase_hex) {
    using ruvia::detail::is_valid_session_id;

    // A session id read back from the client's cookie is trusted enough to key
    // session storage, so the validator must be strict: non-empty, at most 128
    // chars, and lowercase hex only (the shape generate_session_id produces).
    RUVIA_CHECK(is_valid_session_id("deadbeef"));
    RUVIA_CHECK(is_valid_session_id("0123456789abcdef"));
    RUVIA_CHECK(
        is_valid_session_id(std::string_view(std::string(128, 'a'))));  // exactly 128 is allowed

    RUVIA_CHECK(!is_valid_session_id(""));  // empty
    RUVIA_CHECK(
        !is_valid_session_id(std::string_view(std::string(129, 'a'))));    // one past the max length
    RUVIA_CHECK(!is_valid_session_id("DEADBEEF"));                         // uppercase hex is rejected
    RUVIA_CHECK(!is_valid_session_id("deadbeeg"));                         // 'g' is not a hex digit
    RUVIA_CHECK(!is_valid_session_id("dead beef"));                        // space
    RUVIA_CHECK(!is_valid_session_id("../../etc"));                        // path-traversal shape can never validate
    RUVIA_CHECK(!is_valid_session_id(std::string_view("dead\0beef", 9)));  // embedded NUL
}

RUVIA_TEST(session_commit_freezes_before_io_and_failure_is_terminal) {
    ruvia::detail::context_session_state state(std::pmr::new_delete_resource());
    state.bind();
    state.set("user=1");
    RUVIA_CHECK(state.begin_commit());
    int rejections = 0;
    try {
        state.set("user=2");
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        state.clear();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        state.regenerate();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        (void)state.begin_commit();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    RUVIA_CHECK_EQ(rejections, 4);
    state.fail_commit(std::make_exception_ptr(std::runtime_error("storage failed")));
    for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool failed = false;
        try {
            (void)state.begin_commit();
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) == "storage failed";
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(state.data(), std::string_view("user=1"));
    }
}
