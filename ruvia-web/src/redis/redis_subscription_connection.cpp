#include "redis/redis_subscription_connection.h"

#include <exception>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/core/operation_timeout.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_protocol.h"
#include "redis/redis_types_access.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {

redis_subscription_connection::redis_subscription_connection(redis_subscription_set& owner, operation_options options)
    : owner_(owner),
      options_(std::move(options)),
      connection_(owner.io_context_, owner.tls_context_, owner.worker_, owner.config_, owner.wire_counters_,
          owner.resource_),
      current_reply_(redis_types_access::null_value(owner.resource_)),
      cancellation_(owner.cancellation_target_, cancellation_id_) {
    owner_.link(*this);
    if (owner_.closing_) {
        closed_ = true;
        closed_reason_ = client_abort_reason::closing;
    }
    // An already-requested stop is delivered inline, so the connection must
    // be linked and addressable by its id first.
    cancellation_.arm(options_.stop_token_);
}

redis_subscription_connection::~redis_subscription_connection() {
    if (has_active_operations() || !owner_.worker_.is_current()) {
        std::terminate();
    }
    owner_.unlink(*this);
}

std::pmr::memory_resource* redis_subscription_connection::resource() const noexcept {
    return owner_.resource_;
}

const worker_handle& redis_subscription_connection::worker() const noexcept {
    return owner_.worker_;
}

void redis_subscription_connection::throw_if_closed() const {
    if (!closed_) {
        return;
    }
    switch (closed_reason_) {
        case client_abort_reason::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis subscription cancelled");
        case client_abort_reason::closing:
        case client_abort_reason::timeout:
        case client_abort_reason::none:
            break;
    }
    throw redis_error(redis_error::code_type::closing, "redis subscription is closed");
}

void redis_subscription_connection::fail() noexcept {
    if (!closed_) {
        closed_ = true;
        connection_.close();
    }
}

void redis_subscription_connection::abort(client_abort_reason reason) noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    closed_reason_ = reason;
    connection_.abort(reason);
}

task<void> redis_subscription_connection::send(std::pmr::vector<std::pmr::string> args) {
    auto lease = claim_operation_lane(write_active_, "redis subscription command is already in progress");
    throw_if_closed();
    const operation_timeout operation_timeout_value(options_.timeout_);
    try {
        // A subscribed connection receives pushes at any time, so the pool's
        // idle probe cannot judge it: connect only while still closed.
        if (!connection_.connected()) {
            co_await connection_.connect(&operation_timeout_value, owner_.config_.command_timeout_);
            ready_ = true;
        }
        auto& write_buffer = connection_.write_buffer();
        write_buffer.clear();
        write_buffer.reserve(resp_command_serialized_size(args));
        append_resp_command(write_buffer, args);
        co_await connection_.write_request(operation_timeout_value.constrained_by(owner_.config_.command_timeout_));
    } catch (...) {
        fail();
        throw;
    }
}

task<std::optional<redis_pubsub_event>> redis_subscription_connection::read() {
    auto lease = claim_operation_lane(read_active_, "redis subscription read is already in progress");
    if (closed_) {
        co_return std::nullopt;
    }
    if (!ready_) {
        throw std::logic_error("redis subscription has no connection: subscribe first");
    }
    // The previous event's views end here; its storage returns to the pool
    // before the next reply is allocated.
    current_reply_ = redis_types_access::null_value(owner_.resource_);
    try {
        current_reply_ = co_await connection_.read_reply(operation_timeout(options_.timeout_), owner_.resource_);
    } catch (const redis_error& error) {
        fail();
        // The owner ended the subscription: a normal end of the event stream.
        if (error.code() == redis_error::code_type::cancelled || error.code() == redis_error::code_type::closing) {
            co_return std::nullopt;
        }
        throw;
    }
    // A rejected command (for example a denied channel) leaves the stream in
    // step: nothing was subscribed, and later pushes still parse.
    if (current_reply_.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, current_reply_.error());
    }
    try {
        co_return parse_redis_pubsub_event(current_reply_);
    } catch (...) {
        fail();
        throw;
    }
}

redis_subscription_set::redis_subscription_set(asio::io_context& io_context, asio::ssl::context& tls_context,
    const redis_config_storage& config, const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      tls_context_(tls_context),
      config_(config),
      worker_(worker_value),
      resource_(pmr_resource_or_default(resource)),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)) {
    if (!worker_.valid()) {
        throw std::invalid_argument("redis subscriptions require a valid worker");
    }
    static_assert(worker_cancellation_post_is_inline<worker_cancellation_target<redis_subscription_set>>);
}

redis_subscription_set::~redis_subscription_set() {
    if (head_ != nullptr) {
        std::terminate();
    }
    cancellation_target_->detach(*this);
}

redis_subscription_set::connection_owner_type redis_subscription_set::open(operation_options options) {
    return make_pmr_object<redis_subscription_connection>(resource_, *this, std::move(options));
}

void redis_subscription_set::close_now() noexcept {
    closing_ = true;
    for (auto* connection = head_; connection != nullptr; connection = connection->next_) {
        connection->abort(client_abort_reason::closing);
    }
}

void redis_subscription_set::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    for (auto* connection = head_; connection != nullptr; connection = connection->next_) {
        if (connection->cancellation_id_ == cancellation_id) {
            connection->abort(client_abort_reason::cancelled);
            return;
        }
    }
}

void redis_subscription_set::link(redis_subscription_connection& connection) noexcept {
    connection.previous_ = nullptr;
    connection.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &connection;
    }
    head_ = &connection;
}

void redis_subscription_set::unlink(redis_subscription_connection& connection) noexcept {
    if (connection.previous_ != nullptr) {
        connection.previous_->next_ = connection.next_;
    } else {
        head_ = connection.next_;
    }
    if (connection.next_ != nullptr) {
        connection.next_->previous_ = connection.previous_;
    }
    connection.previous_ = nullptr;
    connection.next_ = nullptr;
}

}  // namespace ruvia::detail
