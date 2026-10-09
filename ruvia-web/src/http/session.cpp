#include "ruvia/web/session.h"

#include <stdexcept>

#include "ruvia/web/context.h"
#include "ruvia/web/next.h"

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

std::optional<session> context::try_session() noexcept {
    if (!session_state().available()) {
        return std::nullopt;
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
#include <charconv>
#include <chrono>
#include <memory_resource>

#include "ruvia/http/http_header.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/redis/redis_handle.h"

#include "http/secure_token.h"

namespace ruvia {

session_middleware::config_storage_type::validated_config_type session_middleware::config_storage_type::validate(
    const session_config& source_value) {
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
    return validated_config_type{.source_ = &source_value};
}

session_middleware::config_storage_type::config_storage_type(
    const session_config& source_value, std::pmr::memory_resource* resource)
    : config_storage_type(validate(source_value), resource) {}

session_middleware::config_storage_type::config_storage_type(
    validated_config_type validated, std::pmr::memory_resource* resource)
    : redis_alias_(validated.source_->redis_alias_, resource),
      cookie_name_(validated.source_->cookie_name_, resource),
      key_prefix_(validated.source_->key_prefix_, resource),
      ttl_(validated.source_->ttl_) {}

session_middleware::session_middleware()
    : session_middleware(session_config{}) {}

session_middleware::session_middleware(const session_config& config)
    : config_(config, detail::registration_resource()) {}

task<void> session_middleware::handle(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    const auto cookie = c.req().cookie(config_.cookie_name_);
    if (cookie && detail::is_valid_session_id(*cookie)) {
        detail::session_access::observe_presented_id(c, *cookie);
        std::pmr::string key(c.pool());
        key.append(config_.key_prefix_);
        key.append(cookie->data(), cookie->size());
        if (auto stored = co_await c.redis(config_.redis_alias_).get(key)) {
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
    if (const auto* cleared = state_value.cleared()) {
        auto& response = detail::context_access::response_storage(c);
        auto staged = response.clone_headers_for_transaction(1);
        detail::append_expired_session_cookie_header(staged, c.pool(), config_.cookie_name_, secure);
        if (cleared->old_id_.has_value()) {
            std::pmr::string key(c.pool());
            key.append(config_.key_prefix_);
            key.append(cleared->old_id_->data(), cleared->old_id_->size());
            (void)(co_await c.redis(config_.redis_alias_).del(key));
        }
        response.commit_headers_from(std::move(staged));
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
    const auto token_result = detail::generate_secure_token(id_buffer);
    const auto* token = token_result.ready();
    if (token == nullptr) {
        throw http_error({.status_ = ruvia::http_status::internal_server_error,
            .code_ = "secure_random_failed",
            .message_ = "secure token generation failed"});
    }
    const auto new_id = token->value();

    // Allocate and validate the cookie before changing Redis. Publication after
    // successful storage is an allocation-free move of the complete header state.
    auto& response = detail::context_access::response_storage(c);
    auto staged = response.clone_headers_for_transaction(1);
    detail::append_session_cookie_header(staged, c.pool(), config_.cookie_name_, new_id, secure);

    std::pmr::string key(c.pool());
    key.append(config_.key_prefix_).append(new_id);
    bool applied = false;
    if (!old_id.empty()) {
        std::pmr::string old_key(c.pool());
        old_key.append(config_.key_prefix_).append(old_id);
        std::array<char, 32> ttl_buffer{};
        const auto [ttl_end, ttl_error] = std::to_chars(ttl_buffer.data(), ttl_buffer.data() + ttl_buffer.size(), config_.ttl_.count());
        if (ttl_error != std::errc{}) {
            throw std::logic_error("session TTL serialization failed");
        }
        const std::array<std::string_view, 2> keys{old_key, key};
        const std::array<std::string_view, 2> args{data, std::string_view(ttl_buffer.data(), ttl_end)};
        // Redis executes the entire transition atomically. A stale request may
        // neither recreate a revoked identifier nor mint a successor from it.
        const auto result_value = co_await c.redis(config_.redis_alias_).eval(detail::session_rotation_script, keys, args);
        if (result_value.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, result_value.error());
        }
        applied = result_value.integer() == 1;
    } else {
        redis_set_options options;
        options.condition_ = redis_set_condition::if_absent;
        options.expiration_ = redis_set_expiration::expires_after(config_.ttl_);
        const auto result_value = co_await c.redis(config_.redis_alias_).set(key, data, std::move(options));
        applied = result_value.applied();
    }
    if (!applied) {
        throw http_error({.status_ = http_status::conflict,
            .code_ = "session_conflict",
            .message_ = "session expired or was revoked; retry with a new session"});
    }
    response.commit_headers_from(std::move(staged));
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
