#include "ruvia/web/session.h"

#include <stdexcept>

#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"

#include "context/context_access.h"
#include "http/session_access.h"

namespace ruvia {

std::string_view session::data() const& noexcept {
    return state_->data();
}

void session::set(std::string_view data) {
    state_->set(data);
}

void session::clear() {
    state_->clear();
}

void session::regenerate() {
    state_->regenerate();
}

session context::session() {
    if (!session_state().available()) {
        throw std::logic_error("session capability is not bound for this request");
    }
    return ruvia::session(session_state());
}

task<void> detail::session_access::commit(context& context_value) {
    auto& state_value = context_value.session_state();
    if (!state_value.begin_commit()) {
        co_return;
    }
    try {
#ifdef RUVIA_ENABLE_REDIS
        if (const auto* owner = state_value.owner()) {
            co_await owner->commit(context_value);
        }
#endif
        state_value.finish_commit();
    } catch (...) {
        state_value.fail_commit(std::current_exception());
        throw;
    }
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/redis/redis_handle.h"

#include "http/secure_token.h"

namespace ruvia {
namespace {

struct session_key_buffer final {
    static constexpr std::size_t inline_capacity = 128;
    std::array<char, inline_capacity> inline_{};
    std::pmr::string heap_;
    std::size_t size_{0};

    session_key_buffer(std::string_view prefix, std::string_view id, std::pmr::memory_resource* resource)
        : heap_(resource) {
        size_ = prefix.size() + id.size();
        if (size_ <= inline_capacity) {
            prefix.copy(inline_.data(), prefix.size());
            id.copy(inline_.data() + prefix.size(), id.size());
        } else {
            heap_.reserve(size_);
            heap_.append(prefix);
            heap_.append(id);
        }
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return heap_.empty() ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }
};

}  // namespace

const session_config& session_middleware::validate(const session_config& source_value) {
    if (source_value.redis_alias_.empty()) {
        throw std::invalid_argument("session Redis alias must not be empty");
    }
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("session cookie name must be a valid HTTP token");
    }
    if (source_value.key_prefix_.empty()) {
        throw std::invalid_argument("session key prefix must not be empty");
    }
    if (source_value.ttl_.count() <= 0) {
        throw std::invalid_argument("session TTL must be greater than zero");
    }
    // New sessions expire through SET PX in milliseconds; rotation sends the
    // same TTL as EX seconds, which Redis likewise converts to milliseconds.
    if (source_value.ttl_ > std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max())) {
        throw std::invalid_argument("session TTL must be representable in milliseconds");
    }
    return source_value;
}

session_middleware::session_middleware(
    const session_config& validated, std::pmr::memory_resource* resource)
    : redis_alias_(validated.redis_alias_, resource),
      cookie_name_(validated.cookie_name_, resource),
      key_prefix_(validated.key_prefix_, resource),
      ttl_(validated.ttl_) {}

session_middleware::session_middleware()
    : session_middleware(session_config{}) {}

session_middleware::session_middleware(const session_config& config)
    : session_middleware(validate(config), detail::registration_resource()) {}

task<void> session_middleware::handle(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    const auto cookie = c.req().cookie(cookie_name_);
    if (cookie && detail::is_valid_session_id(*cookie)) {
        detail::session_access::observe_presented_id(c, *cookie);
        auto get_task = [&] {
            const session_key_buffer key(key_prefix_, *cookie, c.pool());
            return c.redis(redis_alias_).get(key.view());
        }();
        if (auto stored = co_await std::move(get_task)) {
            detail::session_access::load(c, *stored);
        }
    }

    co_await next_value();
    co_await detail::session_access::commit(c);
}

task<void> session_middleware::commit(context& c) const {
    const auto& state_value = detail::session_access::state(c);
    if (state_value.untouched() != nullptr || state_value.unrecognized() != nullptr ||
        state_value.loaded() != nullptr) {
        co_return;
    }

    const auto connection = c.conn();
    const bool secure = connection.scheme() == http_scheme::https;
    // Once a final response exists, the cookie must also enter its metadata
    // overlay: an outer middleware's respond() or an error after next() builds
    // its replacement from that overlay alone, and Redis has already changed.
    // Both blocks are staged before Redis is touched; publication after
    // successful storage is an allocation-free move of complete header state.
    auto& response = detail::context_access::response_storage(c);
    auto* const overlay = detail::context_access::final_response_metadata(c);
    auto staged = response.headers().clone(1);
    std::optional<http_response_headers> staged_overlay;
    if (overlay != nullptr) {
        staged_overlay.emplace(overlay->headers().clone(1));
    }
    const auto publish = [&]() noexcept {
        response.commit_headers(std::move(staged));
        if (overlay != nullptr) {
            overlay->commit_headers(std::move(*staged_overlay));
        }
    };
    if (const auto* cleared = state_value.cleared()) {
        detail::append_expired_session_cookie_header(staged, cookie_name_, secure);
        if (staged_overlay) {
            detail::append_expired_session_cookie_header(*staged_overlay, cookie_name_, secure);
        }
        if (cleared->old_id_.has_value()) {
            auto del_task = [&] {
                const session_key_buffer key(key_prefix_, *cleared->old_id_, c.pool());
                return c.redis(redis_alias_).del(key.view());
            }();
            (void)(co_await std::move(del_task));
        }
        publish();
        co_return;
    }

    std::string_view data;
    std::string_view old_id;
    if (const auto* rotated = state_value.rotate()) {
        data = rotated->data_;
        old_id = rotated->old_id_;
    } else {
        data = state_value.persist_new()->data_;
    }

    std::array<char, 64> id_buffer;
    const auto token = detail::generate_secure_token(id_buffer);
    if (!token) {
        throw http_error({.status_ = ruvia::http_status::internal_server_error,
            .code_ = "secure_random_failed",
            .message_ = "secure token generation failed"});
    }
    const auto new_id = *token;
    detail::append_session_cookie_header(staged, cookie_name_, new_id, secure);
    if (staged_overlay) {
        detail::append_session_cookie_header(*staged_overlay, cookie_name_, new_id, secure);
    }

    const session_key_buffer key(key_prefix_, new_id, c.pool());
    bool applied = false;
    if (!old_id.empty()) {
        auto eval_task = [&] {
            const session_key_buffer old_key(key_prefix_, old_id, c.pool());
            // session_middleware::validate requires a positive TTL.
            std::array<char, http_max_unsigned_decimal_size> ttl_buffer;
            const auto ttl = http_format_unsigned(ttl_buffer, static_cast<std::uint64_t>(ttl_.count()));
            const std::array<std::string_view, 2> keys{old_key.view(), key.view()};
            const std::array<std::string_view, 2> args{data, ttl};
            // Redis executes the entire transition atomically. A stale request may
            // neither recreate a revoked identifier nor mint a successor from it.
            return c.redis(redis_alias_).eval(detail::session_rotation_script, keys, args);
        }();
        const auto result_value = co_await std::move(eval_task);
        if (result_value.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, result_value.error());
        }
        applied = result_value.integer() == 1;
    } else {
        auto set_task = [&] {
            redis_set_options options;
            options.condition_ = redis_set_condition::if_absent;
            options.expiration_ = redis_set_expiration::expires_after(ttl_);
            return c.redis(redis_alias_).set(key.view(), data, std::move(options));
        }();
        const auto result_value = co_await std::move(set_task);
        applied = result_value.applied();
    }
    if (!applied) {
        throw http_error({.status_ = http_status::conflict,
            .code_ = "session_conflict",
            .message_ = "session expired or was revoked; retry with a new session"});
    }
    publish();
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
