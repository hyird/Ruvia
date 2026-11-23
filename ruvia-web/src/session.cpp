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
#include "crypto.h"
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

const session_config& validate_session_config(const session_config& source_value) {
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
    if (source_value.ttl_ > std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max())) {
        throw std::invalid_argument("session TTL must be representable in milliseconds");
    }
    const cookie_options check_attrs{
        .path_ = source_value.path_,
        .domain_ = source_value.domain_,
        .same_site_ = source_value.same_site_,
        .http_only_ = cookie_attribute_policy::emit,
        .secure_ = source_value.secure_.value_or(true) ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
    };
    (void)set_cookie_plan(source_value.cookie_name_, "000000000000000000000000000000000000000000000000", check_attrs);
    return source_value;
}

}  // namespace

session_store::session_store(const session_config& config) :
    redis_alias_(validate_session_config(config).redis_alias_),
    cookie_name_(config.cookie_name_),
    key_prefix_(config.key_prefix_),
    ttl_(config.ttl_),
    path_(config.path_),
    domain_(config.domain_),
    same_site_(config.same_site_),
    secure_(config.secure_),
    secret_(config.secret_) {}

std::optional<std::string_view> session_store::verify_cookie_id(std::string_view cookie_value) const noexcept {
    const auto candidate = secret_.empty() ? std::optional<std::string_view>(cookie_value) : verify_signed_cookie_value(secret_, cookie_name_, cookie_value);
    if (!candidate.has_value() || !detail::is_valid_session_id(*candidate)) {
        return std::nullopt;
    }
    return *candidate;
}

std::string session_store::format_cookie_value(std::string_view session_id) const {
    if (!detail::is_valid_session_id(session_id)) {
        throw std::invalid_argument("session ID must be 48 lowercase hexadecimal characters");
    }
    if (secret_.empty()) {
        return std::string(session_id);
    }
    return sign_cookie_value(secret_, cookie_name_, session_id);
}

cookie_options session_store::cookie_attributes(bool connection_secure) const noexcept {
    return cookie_options{
        .path_ = path_,
        .domain_ = domain_,
        .same_site_ = same_site_,
        .http_only_ = cookie_attribute_policy::emit,
        .secure_ = secure_.value_or(connection_secure) ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
    };
}

cookie_options session_store::deletion_cookie_attributes(bool connection_secure) const noexcept {
    auto attrs = cookie_attributes(connection_secure);
    attrs.expires_ = cookie_deletion_expires;
    return attrs;
}

task<std::string> session_store::create(const redis_executor& redis, std::string_view payload, operation_options options) const {
    if (payload.empty()) {
        throw std::invalid_argument("session payload must not be empty");
    }
    std::array<char, detail::secure_token_size> id_buffer;
    std::string new_id(detail::generate_secure_token(id_buffer));
    auto set_task = [&] {
        const session_key key({key_prefix_, new_id});
        redis_set_options set_opts;
        set_opts.condition_ = redis_set_condition::if_absent;
        set_opts.expiration_ = redis_set_expiration::expires_after(ttl_);
        return redis.with_options(std::move(options)).set(key.view(), payload, std::move(set_opts));
    }();
    const auto result_value = co_await std::move(set_task);
    if (!result_value.applied()) {
        throw http_error({.status_ = http_status::conflict, .code_ = "session_conflict", .message_ = "session expired or was revoked; retry with a new session"});
    }
    co_return new_id;
}

task<std::optional<std::string>> session_store::load(const redis_executor& redis, std::string_view session_id, operation_options options) const {
    if (!detail::is_valid_session_id(session_id)) {
        co_return std::nullopt;
    }
    auto get_task = [&] {
        const session_key key({key_prefix_, session_id});
        return redis.with_options(std::move(options)).get(key.view());
    }();
    co_return co_await std::move(get_task);
}

task<std::optional<std::string>> session_store::rotate(const redis_executor& redis, std::string_view old_session_id, std::string_view payload, operation_options options) const {
    if (!detail::is_valid_session_id(old_session_id) || payload.empty()) {
        co_return std::nullopt;
    }
    std::array<char, detail::secure_token_size> id_buffer;
    std::string new_id(detail::generate_secure_token(id_buffer));
    static const redis_script rotation_script(detail::session_rotation_script);
    auto eval_task = [&] {
        const session_key old_key({key_prefix_, old_session_id});
        const session_key new_key({key_prefix_, new_id});
        std::array<char, max_decimal_size> ttl_buffer;
        const auto ttl_text = format_decimal(ttl_buffer, static_cast<std::uint64_t>(ttl_.count()));
        const std::array<std::string_view, 2> keys{old_key.view(), new_key.view()};
        const std::array<std::string_view, 2> args{payload, ttl_text};
        return redis.with_options(std::move(options)).eval_script(rotation_script, keys, args);
    }();
    const auto result_value = co_await std::move(eval_task);
    if (result_value.kind() == redis_value::kind_type::error) {
        throw redis_error(backend_error_code::command_failed, result_value.error());
    }
    if (result_value.integer() != 1) {
        co_return std::nullopt;
    }
    co_return new_id;
}

task<bool> session_store::destroy(const redis_executor& redis, std::string_view session_id, operation_options options) const {
    if (!detail::is_valid_session_id(session_id)) {
        co_return false;
    }
    auto del_task = [&] {
        const session_key key({key_prefix_, session_id});
        return redis.with_options(std::move(options)).del(key.view());
    }();
    co_return (co_await std::move(del_task)) > 0;
}

task<void> detail::session_access::commit(context& context_value) {
    auto& state_value = context_value.session_state();
    if (!state_value.begin_commit()) {
        co_return;
    }
    try {
        if (const auto* owner = state_value.owner()) {
            const auto& store = owner->store_;
            const auto connection = context_value.connection();
            const bool secure = connection.scheme() == http_scheme::https;
            // Session Cookie 属于明确的跨替换响应策略，只在最终交付/流提交时应用。
            // Redis 成功后通过无分配移动发布完整字段，既有 commit guard 保证只持久化一次。
            auto& response = detail::context_access::response_policy(context_value);
            auto staged = response.headers().clone(1);
            const auto publish = [&]() noexcept {
                response.commit_headers(std::move(staged));
            };
            if (const auto* cleared = state_value.cleared()) {
                detail::set_session_cookie(staged, store.cookie_name_, "", store.deletion_cookie_attributes(secure));
                if (cleared->old_id_.has_value()) {
                    (void)(co_await store.destroy(context_value.redis(store.redis_alias_), *cleared->old_id_));
                }
                publish();
            } else if (const auto* rotated = state_value.rotate()) {
                const auto new_id = co_await store.rotate(context_value.redis(store.redis_alias_), rotated->old_id_, rotated->payload_);
                if (!new_id.has_value()) {
                    throw http_error({.status_ = http_status::conflict, .code_ = "session_conflict", .message_ = "session expired or was revoked; retry with a new session"});
                }
                const auto wire_value = store.format_cookie_value(*new_id);
                detail::set_session_cookie(staged, store.cookie_name_, wire_value, store.cookie_attributes(secure));
                publish();
            } else {
                const auto new_id = co_await store.create(context_value.redis(store.redis_alias_), state_value.persist_new()->payload_);
                const auto wire_value = store.format_cookie_value(new_id);
                detail::set_session_cookie(staged, store.cookie_name_, wire_value, store.cookie_attributes(secure));
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

session_middleware::session_middleware() :
    session_middleware(session_config{}) {}

session_middleware::session_middleware(const session_config& config) :
    store_(config) {}

task<void> session_middleware::invoke(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    // A repeated session cookie is treated as absent: loading either occurrence
    // would let an injected cookie fixate or shadow the session.
    const auto cookie = c.request().unique_cookie(store_.cookie_name_);
    if (cookie.status_ == unique_lookup_status::found) {
        if (const auto verified_id = store_.verify_cookie_id(cookie.value_)) {
            detail::session_access::observe_presented_id(c, *verified_id);
            if (auto stored = co_await store_.load(c.redis(store_.redis_alias_), *verified_id)) {
                detail::session_access::load(c, std::move(*stored));
            }
        }
    }

    co_await next_value();
    if (!detail::session_access::try_commit_sync(c)) {
        co_await detail::session_access::commit(c);
    }
}

}  // namespace ruvia
