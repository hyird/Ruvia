#include "ruvia/web/session.h"

#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/security_headers.h"

#include "context/context_access.h"
#include "http/session_access.h"

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

bool context_session_state::begin_commit() {
    if (!available_ || phase_ == phase_type::committed) {
        return false;
    }
    if (phase_ == phase_type::failed) {
        std::rethrow_exception(failure_);
    }
    require_mutable();
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

std::pmr::string context_session_state::copy(std::string_view value) const {
    return std::pmr::string(value, resource_);
}

void context_session_state::observe_presented_id(std::string_view id) {
    require_mutable();
    value_.template emplace<session_unrecognized>(copy(id));
}

void context_session_state::load_recognized(std::string_view data) {
    require_mutable();
    auto* presented = std::get_if<session_unrecognized>(&value_);
    if (presented == nullptr) {
        throw std::logic_error("recognized session requires a presented id");
    }
    auto data_copy = copy(data);
    auto id = std::move(presented->id_);
    value_.template emplace<session_loaded>(std::move(id), std::move(data_copy));
}

void context_session_state::set(std::string_view data) {
    require_mutable();
    if (data.empty()) {
        clear();
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        auto data_copy = copy(data);
        auto id = std::move(loaded_state->id_);
        value_.template emplace<session_rotate>(std::move(id), std::move(data_copy));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        rotated->data_.assign(data);
        return;
    }
    if (auto* fresh = std::get_if<session_persist_new>(&value_)) {
        fresh->data_.assign(data);
        return;
    }
    if (auto* cleared = std::get_if<session_clear>(&value_)) {
        if (!cleared->old_id_.has_value()) {
            value_.template emplace<session_persist_new>(copy(data));
            return;
        }
        auto data_copy = copy(data);
        auto old_id = std::move(*cleared->old_id_);
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data_copy));
        return;
    }
    value_.template emplace<session_persist_new>(copy(data));
}

void context_session_state::clear() {
    require_mutable();
    if (std::get_if<session_clear>(&value_) != nullptr) {
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        std::optional<std::pmr::string> id(std::move(loaded_state->id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        std::optional<std::pmr::string> id(std::move(rotated->old_id_));
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
    auto data = std::move(loaded_state->data_);
    if (data.empty()) {
        value_.template emplace<session_clear>(
            std::optional<std::pmr::string>(std::move(old_id)));
    } else {
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data));
    }
}

std::string_view context_session_state::data() const& noexcept {
    if (const auto* state = std::get_if<session_loaded>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_persist_new>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_rotate>(&value_)) {
        return state->data_;
    }
    return {};
}

}  // namespace detail

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

namespace {

constexpr std::string_view content_security_policy_name = "Content-Security-Policy";
constexpr std::string_view referrer_policy_name = "Referrer-Policy";
constexpr std::string_view permissions_policy_name = "Permissions-Policy";

[[nodiscard]] bool emits_default_security_header(default_security_header_policy policy) {
    switch (policy) {
        case default_security_header_policy::emit_default:
            return true;
        case default_security_header_policy::omit:
            return false;
        default:
            throw std::invalid_argument("default security header policy is invalid");
    }
}

[[nodiscard]] bool replaces_existing_security_header(security_header_conflict_policy policy) {
    switch (policy) {
        case security_header_conflict_policy::preserve_existing:
            return false;
        case security_header_conflict_policy::replace_existing:
            return true;
        default:
            throw std::invalid_argument("security header conflict policy is invalid");
    }
}

}  // namespace

security_headers_middleware::security_headers_middleware(
    const security_headers_config& source_value, std::pmr::memory_resource* resource)
    : emit_content_type_options_(emits_default_security_header(source_value.content_type_options_header_)),
      emit_frame_options_(emits_default_security_header(source_value.frame_options_header_)),
      emit_strict_transport_security_(
          emits_default_security_header(source_value.strict_transport_security_header_)),
      emit_disabled_xss_protection_([&] {
          switch (source_value.xss_protection_header_) {
              case xss_protection_header_policy::emit_disabled:
                  return true;
              case xss_protection_header_policy::omit:
                  return false;
              default:
                  throw std::invalid_argument("X-XSS-Protection header policy is invalid");
          }
      }()),
      replace_existing_(replaces_existing_security_header(source_value.existing_headers_)),
      content_security_policy_(resource),
      referrer_policy_(resource),
      permissions_policy_(resource),
      custom_headers_(resource) {
    const auto validate_value = [](std::string_view value) {
        if (!value.empty() && !is_valid_http_header_value(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validate_value(source_value.content_security_policy_);
    validate_value(source_value.referrer_policy_);
    validate_value(source_value.permissions_policy_);
    for (const auto& header : source_value.custom_headers_) {
        if (!is_valid_http_header_name(header.name_)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!is_valid_http_header_value(header.value_)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    content_security_policy_.assign(source_value.content_security_policy_);
    referrer_policy_.assign(source_value.referrer_policy_);
    permissions_policy_.assign(source_value.permissions_policy_);
    custom_headers_.reserve(source_value.custom_headers_.size());
    for (const auto& header : source_value.custom_headers_) {
        custom_headers_.emplace_back(header.name_, header.value_, resource);
    }
}

security_headers_middleware::security_headers_middleware()
    : security_headers_middleware(security_headers_config{}) {}

security_headers_middleware::security_headers_middleware(const security_headers_config& config)
    : security_headers_middleware(config, process_resource()) {}

task<void> security_headers_middleware::handle(context& context_value, next& next_value) {
    const bool secure_transport = context_value.conn().scheme() == http_scheme::https;
    const auto set_header = [&context_value, replace_existing = replace_existing_](
                                std::string_view name, std::string_view value, bool skip_empty) {
        if (skip_empty && value.empty()) {
            return;
        }
        detail::context_access::security_header(context_value, name, value, replace_existing);
    };

    if (emit_content_type_options_) {
        set_header("X-Content-Type-Options", "nosniff", true);
    }
    if (emit_frame_options_) {
        set_header("X-Frame-Options", "DENY", true);
    }
    if (emit_strict_transport_security_ && secure_transport) {
        set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (emit_disabled_xss_protection_) {
        set_header("X-XSS-Protection", "0", true);
    }

    set_header(content_security_policy_name, content_security_policy_, true);
    set_header(referrer_policy_name, referrer_policy_, true);
    set_header(permissions_policy_name, permissions_policy_, true);

    for (const auto& header : custom_headers_) {
        if (!secure_transport &&
            http_ascii_equals_ignore_case(header.name(), "Strict-Transport-Security")) {
            continue;
        }
        set_header(header.name(), header.value(), false);
    }
    co_await next_value();
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/error.h"
#include "ruvia/web/redis/redis_handle.h"

#include "tls/tls_random.h"
#include "util/inline_text_concat.h"

namespace ruvia {
namespace {

// "<key prefix><session id>", inline for typical key sizes.
using session_key = detail::inline_text_concat<128>;

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
    : session_middleware(validate(config), process_resource()) {}

task<void> session_middleware::handle(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    const auto cookie = c.req().cookie(cookie_name_);
    if (cookie && detail::is_valid_session_id(*cookie)) {
        detail::session_access::observe_presented_id(c, *cookie);
        auto get_task = [&] {
            const session_key key({key_prefix_, *cookie}, ruvia::process_resource());
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
    // Session Cookie 属于明确的跨替换响应策略，只在最终交付/流提交时应用。
    // Redis 成功后通过无分配移动发布完整字段，既有 commit guard 保证只持久化一次。
    auto& response = detail::context_access::response_policy(c);
    auto staged = response.headers().clone(1);
    const auto publish = [&]() noexcept {
        response.commit_headers(std::move(staged));
    };
    if (const auto* cleared = state_value.cleared()) {
        detail::append_expired_session_cookie_header(staged, cookie_name_, secure);
        if (cleared->old_id_.has_value()) {
            auto del_task = [&] {
                const session_key key({key_prefix_, *cleared->old_id_}, ruvia::process_resource());
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

    std::array<char, detail::secure_token_size> id_buffer;
    const auto new_id = detail::generate_secure_token(id_buffer);
    detail::append_session_cookie_header(staged, cookie_name_, new_id, secure);

    const session_key key({key_prefix_, new_id}, ruvia::process_resource());
    bool applied = false;
    if (!old_id.empty()) {
        auto eval_task = [&] {
            const session_key old_key({key_prefix_, old_id}, ruvia::process_resource());
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
