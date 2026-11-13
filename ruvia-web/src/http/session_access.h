#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"

#include "tls/tls_protocol_policy.h"

namespace ruvia {
class session_middleware;
}

namespace ruvia::detail {

struct session_untouched final {};

struct session_unrecognized final {
    std::string id_;
};

struct session_loaded final {
    std::string id_;
    std::string data_;
};

struct session_persist_new final {
    std::string data_;
};

struct session_rotate final {
    std::string old_id_;
    std::string data_;
};

struct session_clear final {
    std::optional<std::string> old_id_;
};

class context_session_state final {
public:
    context_session_state() noexcept = default;

    void bind(const session_middleware* owner);
    [[nodiscard]] bool available() const noexcept {
        return available_;
    }
    [[nodiscard]] const session_middleware* owner() const noexcept {
        return owner_;
    }
    // The first commit freezes all mutation before any asynchronous storage I/O.
    // A failed commit is terminal: its external writes cannot safely be replayed.
    [[nodiscard]] bool begin_commit();
    void finish_commit() noexcept;
    void fail_commit(std::exception_ptr exception) noexcept;
    void observe_presented_id(std::string_view id);
    void load_recognized(std::string_view data);
    void set(std::string_view data);
    void clear();
    void regenerate();

    [[nodiscard]] std::string_view data() const& noexcept;
    [[nodiscard]] std::string_view data() const&& = delete;

    [[nodiscard]] const session_untouched* untouched() const& noexcept {
        return std::get_if<session_untouched>(&value_);
    }
    [[nodiscard]] const session_untouched* untouched() const&& = delete;

    [[nodiscard]] const session_unrecognized* unrecognized() const& noexcept {
        return std::get_if<session_unrecognized>(&value_);
    }
    [[nodiscard]] const session_unrecognized* unrecognized() const&& = delete;

    [[nodiscard]] const session_loaded* loaded() const& noexcept {
        return std::get_if<session_loaded>(&value_);
    }
    [[nodiscard]] const session_loaded* loaded() const&& = delete;

    [[nodiscard]] const session_persist_new* persist_new() const& noexcept {
        return std::get_if<session_persist_new>(&value_);
    }
    [[nodiscard]] const session_persist_new* persist_new() const&& = delete;

    [[nodiscard]] const session_rotate* rotate() const& noexcept {
        return std::get_if<session_rotate>(&value_);
    }
    [[nodiscard]] const session_rotate* rotate() const&& = delete;

    [[nodiscard]] const session_clear* cleared() const& noexcept {
        return std::get_if<session_clear>(&value_);
    }
    [[nodiscard]] const session_clear* cleared() const&& = delete;

private:
    enum class phase_type : unsigned char { active,
        committing,
        committed,
        failed };
    void require_mutable() const;

    bool available_{false};
    const session_middleware* owner_{nullptr};
    phase_type phase_{phase_type::active};
    std::exception_ptr failure_;
    std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new,
        session_rotate, session_clear>
        value_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Privileged access to a context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct session_access final {
    static void bind(context& context_value, const session_middleware* owner) {
        context_value.session_state().bind(owner);
    }

    [[nodiscard]] static bool available(const context& context_value) noexcept {
        return context_value.session_state().available();
    }

    // No callback chain: the bound session_middleware owns this capability's
    // single response-commit stage for both buffered and long-lived routes.
    [[nodiscard]] static task<void> commit(context& context);

    static void observe_presented_id(context& context_value, std::string_view id) {
        context_value.session_state().observe_presented_id(id);
    }

    static void load(context& context_value, std::string_view data) {
        context_value.session_state().load_recognized(data);
    }

    [[nodiscard]] static const context_session_state& state(const context& context_value) noexcept {
        return context_value.session_state();
    }
};

inline constexpr std::string_view session_rotation_script =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "if not redis.call('SET', KEYS[2], ARGV[1], 'EX', ARGV[2], 'NX') then return 0 end "
    "redis.call('DEL', KEYS[1]) return 1";

[[nodiscard]] inline bool is_valid_session_id(std::string_view id) noexcept {
    if (id.size() != secure_token_size) {
        return false;
    }
    for (const char ch : id) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

inline void set_session_cookie(http_response_headers& headers, std::string_view cookie_name,
    std::string_view value, const cookie_options& options) {
    headers.set_cookie(set_cookie_plan(cookie_name, value, options));
}

inline void append_session_cookie_header(
    http_response_headers& headers, std::string_view cookie_name, std::string_view id, bool secure) {
    set_session_cookie(headers, cookie_name, id,
        {
            .same_site_ = cookie_same_site::lax,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

inline void append_expired_session_cookie_header(
    http_response_headers& headers, std::string_view cookie_name, bool secure) {
    set_session_cookie(headers, cookie_name, "",
        {
            .same_site_ = cookie_same_site::lax,
            .expires_ = cookie_deletion_expires,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

}  // namespace ruvia::detail
