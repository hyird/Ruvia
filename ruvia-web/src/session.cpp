#include "session.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "context.h"
#include "context_access.h"
#include "decimal_text.h"
#include "http_error.h"
#include "http_header.h"
#include "http_request.h"
#include "inline_storage.h"
#include "middleware.h"
#include "redis_orm.h"
#include "secret_encoding.h"
#include "task.h"

namespace ruvia {
namespace detail {

void context_session_state::bind(const session_middleware* owner_value) {
    if (available_) {
        throw std::logic_error("session middleware is already bound");
    }
    require_mutable();
    owner_ = owner_value;
    available_ = true;
}

void context_session_state::require_mutable() const {
    if (phase_ != phase_type::active) {
        throw std::logic_error("session cannot change after response commit begins");
    }
}

bool context_session_state::try_commit_sync() {
    if (!available_ || phase_ == phase_type::committed) {
        return true;
    }
    if (phase_ == phase_type::failed) {
        std::rethrow_exception(failure_);
    }
    require_mutable();
    if (untouched() != nullptr || unrecognized() != nullptr || loaded() != nullptr) {
        phase_ = phase_type::committed;
        return true;
    }
    return false;
}

bool context_session_state::begin_commit() {
    if (try_commit_sync()) {
        return false;
    }
    phase_ = phase_type::committing;
    return true;
}

void context_session_state::finish_commit() noexcept {
    phase_ = phase_type::committed;
}

void context_session_state::fail_commit(std::exception_ptr exception) noexcept {
    failure_ = std::move(exception);
    phase_ = phase_type::failed;
}

void context_session_state::observe_presented_id(std::string_view id) {
    require_mutable();
    value_.template emplace<session_unrecognized>(std::string(id));
}

namespace {

template <typename payload_type>
void load_recognized_session(std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new, session_rotate, session_clear>& value, payload_type&& payload) {
    auto* presented = std::get_if<session_unrecognized>(&value);
    if (presented == nullptr) {
        throw std::logic_error("recognized session requires a presented id");
    }
    std::string owned_payload(std::forward<payload_type>(payload));
    auto id = std::move(presented->id_);
    value.template emplace<session_loaded>(std::move(id), std::move(owned_payload));
}

template <typename payload_type>
void assign_session_payload(std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new, session_rotate, session_clear>& value, payload_type&& payload) {
    if (auto* loaded_state = std::get_if<session_loaded>(&value)) {
        std::string owned_payload(std::forward<payload_type>(payload));
        auto id = std::move(loaded_state->id_);
        value.template emplace<session_rotate>(std::move(id), std::move(owned_payload));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value)) {
        if constexpr (std::is_same_v<payload_type&&, std::string&&>) {
            rotated->payload_ = std::move(payload);
        } else {
            rotated->payload_.assign(payload);
        }
        return;
    }
    if (auto* fresh = std::get_if<session_persist_new>(&value)) {
        if constexpr (std::is_same_v<payload_type&&, std::string&&>) {
            fresh->payload_ = std::move(payload);
        } else {
            fresh->payload_.assign(payload);
        }
        return;
    }
    if (auto* cleared = std::get_if<session_clear>(&value)) {
        if (!cleared->old_id_.has_value()) {
            value.template emplace<session_persist_new>(std::string(std::forward<payload_type>(payload)));
            return;
        }
        std::string owned_payload(std::forward<payload_type>(payload));
        auto old_id = std::move(*cleared->old_id_);
        value.template emplace<session_rotate>(std::move(old_id), std::move(owned_payload));
        return;
    }
    value.template emplace<session_persist_new>(std::string(std::forward<payload_type>(payload)));
}

}  // namespace

void context_session_state::load_recognized(std::string_view payload) {
    require_mutable();
    load_recognized_session(value_, payload);
}

void context_session_state::load_recognized(std::string&& payload) {
    require_mutable();
    load_recognized_session(value_, std::move(payload));
}

void context_session_state::set(std::string_view payload) {
    require_mutable();
    if (payload.empty()) {
        clear();
        return;
    }
    assign_session_payload(value_, payload);
}

void context_session_state::set(std::string&& payload) {
    require_mutable();
    if (payload.empty()) {
        clear();
        return;
    }
    assign_session_payload(value_, std::move(payload));
}

void context_session_state::clear() {
    require_mutable();
    if (std::get_if<session_clear>(&value_) != nullptr) {
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        std::optional<std::string> id(std::move(loaded_state->id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        std::optional<std::string> id(std::move(rotated->old_id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    value_.template emplace<session_clear>(std::nullopt);
}

void context_session_state::regenerate() {
    require_mutable();
    auto* loaded_state = std::get_if<session_loaded>(&value_);
    if (loaded_state == nullptr) {
        return;
    }
    auto old_id = std::move(loaded_state->id_);
    auto payload = std::move(loaded_state->payload_);
    if (payload.empty()) {
        value_.template emplace<session_clear>(std::optional<std::string>(std::move(old_id)));
    } else {
        value_.template emplace<session_rotate>(std::move(old_id), std::move(payload));
    }
}

std::string_view context_session_state::payload() const& noexcept {
    if (const auto* state = std::get_if<session_loaded>(&value_)) {
        return state->payload_;
    }
    if (const auto* state = std::get_if<session_persist_new>(&value_)) {
        return state->payload_;
    }
    if (const auto* state = std::get_if<session_rotate>(&value_)) {
        return state->payload_;
    }
    return {};
}

}  // namespace detail

std::string_view session::payload() const& noexcept {
    return state_->payload();
}

void session::set(std::string_view payload) {
    state_->set(payload);
}

void session::set_owned(std::string&& payload) {
    state_->set(std::move(payload));
}

void session::clear() {
    state_->clear();
}

void session::regenerate() {
    state_->regenerate();
}

session context::session() & {
    if (!session_state().available()) {
        throw std::logic_error("session capability is not bound for this request");
    }
    return ruvia::session(session_state());
}

detail::context_session_state& context::session_state() noexcept {
    return request_storage().session_state_;
}

const detail::context_session_state& context::session_state() const noexcept {
    return request_storage().session_state_;
}

namespace {

// "<key prefix><session id>", inline for typical key sizes.
using session_key = detail::inline_text_concat<128>;

}  // namespace

task<void> detail::session_access::commit(context& context_value) {
    auto& state_value = context_value.session_state();
    if (!state_value.begin_commit()) {
        co_return;
    }
    try {
        if (const auto* owner = state_value.owner()) {
            const auto connection = context_value.conn();
            const bool secure = connection.scheme() == http_scheme::https;
            // Session Cookie 属于明确的跨替换响应策略，只在最终交付/流提交时应用。
            // Redis 成功后通过无分配移动发布完整字段，既有 commit guard 保证只持久化一次。
            auto& response = detail::context_access::response_policy(context_value);
            auto staged = response.headers().clone(1);
            const auto publish = [&]() noexcept {
                response.commit_headers(std::move(staged));
            };
            if (const auto* cleared = state_value.cleared()) {
                detail::append_expired_session_cookie_header(staged, owner->cookie_name_, secure);
                if (cleared->old_id_.has_value()) {
                    auto del_task = [&] {
                        const session_key key({owner->key_prefix_, *cleared->old_id_});
                        return context_value.redis(owner->redis_alias_).del(key.view());
                    }();
                    (void)(co_await std::move(del_task));
                }
                publish();
            } else {
                std::string_view payload;
                std::string_view old_id;
                if (const auto* rotated = state_value.rotate()) {
                    payload = rotated->payload_;
                    old_id = rotated->old_id_;
                } else {
                    payload = state_value.persist_new()->payload_;
                }

                std::array<char, detail::secure_token_size> id_buffer;
                const auto new_id = detail::generate_secure_token(id_buffer);
                detail::append_session_cookie_header(staged, owner->cookie_name_, new_id, secure);

                const session_key key({owner->key_prefix_, new_id});
                bool applied = false;
                if (!old_id.empty()) {
                    static const redis_script rotation_script(detail::session_rotation_script);
                    auto eval_task = [&] {
                        const session_key old_key({owner->key_prefix_, old_id});
                        // session_middleware::validate requires a positive TTL.
                        std::array<char, max_decimal_size> ttl_buffer;
                        const auto ttl = format_decimal(ttl_buffer, static_cast<std::uint64_t>(owner->ttl_.count()));
                        const std::array<std::string_view, 2> keys{old_key.view(), key.view()};
                        const std::array<std::string_view, 2> args{payload, ttl};
                        // Redis executes the entire transition atomically. A stale request may
                        // neither recreate a revoked identifier nor mint a successor from it.
                        return context_value.redis(owner->redis_alias_).eval_cached(rotation_script, keys, args);
                    }();
                    const auto result_value = co_await std::move(eval_task);
                    if (result_value.kind() == redis_value::kind_type::error) {
                        throw redis_error(backend_error_code::command_failed, result_value.error());
                    }
                    applied = result_value.integer() == 1;
                } else {
                    auto set_task = [&] {
                        redis_set_options options;
                        options.condition_ = redis_set_condition::if_absent;
                        options.expiration_ = redis_set_expiration::expires_after(owner->ttl_);
                        return context_value.redis(owner->redis_alias_).set(key.view(), payload, std::move(options));
                    }();
                    const auto result_value = co_await std::move(set_task);
                    applied = result_value.applied();
                }
                if (!applied) {
                    throw http_error({.status_ = http_status::conflict, .code_ = "session_conflict", .message_ = "session expired or was revoked; retry with a new session"});
                }
                publish();
            }
        }
        state_value.finish_commit();
    } catch (...) {
        state_value.fail_commit(std::current_exception());
        throw;
    }
    co_return;
}

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

session_middleware::session_middleware() :
    session_middleware(session_config{}) {}

session_middleware::session_middleware(const session_config& config) :
    redis_alias_(validate(config).redis_alias_),
    cookie_name_(config.cookie_name_),
    key_prefix_(config.key_prefix_),
    ttl_(config.ttl_) {}

task<void> session_middleware::invoke(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    // A repeated session cookie is treated as absent: loading either occurrence
    // would let an injected cookie fixate or shadow the session.
    const auto cookie = c.req().unique_cookie(cookie_name_);
    if (cookie.status_ == unique_lookup_status::found && detail::is_valid_session_id(cookie.value_)) {
        detail::session_access::observe_presented_id(c, cookie.value_);
        auto get_task = [&] {
            const session_key key({key_prefix_, cookie.value_});
            return c.redis(redis_alias_).get(key.view());
        }();
        if (auto stored = co_await std::move(get_task)) {
            detail::session_access::load(c, std::move(*stored));
        }
    }

    co_await next_value();
    if (!detail::session_access::try_commit_sync(c)) {
        co_await detail::session_access::commit(c);
    }
}

}  // namespace ruvia
