#include "redis/redis_subscription_connection.h"

#include <algorithm>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/worker_timer.h"

#include "redis/redis_protocol.h"
#include "redis/redis_reply_parsers.h"
#include "redis/redis_types_access.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {
namespace {

// An argument-less PING, answered ["pong", ""] while subscribed.
constexpr std::string_view redis_ping_request = "*1\r\n$4\r\nPING\r\n";

}  // namespace

redis_subscription_connection::redis_subscription_connection(redis_subscription_set& owner, operation_options options)
    : owner_(owner),
      options_(std::move(options)),
      connection_(owner.io_context_, owner.tls_context_, owner.worker_, owner.config_, owner.wire_counters_,
          owner.resource_),
      current_reply_(redis_types_access::null_value(owner.resource_)),
      ping_written_(owner.worker_),
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

task<void> redis_subscription_connection::send(redis_owned_command command) {
    auto lease = claim_operation_lane(write_active_, "redis subscription command is already in progress");
    throw_if_closed();
    const operation_timeout operation_timeout_value(options_.timeout_);
    try {
        // An idle probe's PING may hold the transport's write lane; it ends
        // within one interval or with the connection.
        while (ping_writing_) {
            co_await ping_written_.wait();
        }
        throw_if_closed();
        // A subscribed connection receives pushes at any time, so the pool's
        // idle probe cannot judge it: connect only while still closed.
        if (!connection_.connected()) {
            co_await connection_.connect(&operation_timeout_value, owner_.config_.command_timeout_);
            ready_ = true;
        }
        auto& write_buffer = connection_.write_buffer();
        write_buffer.assign(command.wire_);
        // The serialized request owns the bytes needed by the write.
        std::pmr::string(command.wire_.get_allocator()).swap(command.wire_);
        // Stamped before the write: the reply may arrive before this
        // coroutine resumes and must still count as newer than the request.
        last_request_ = clock_type::now();
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
    const operation_timeout timeout_value(options_.timeout_);
    std::exception_ptr failure;
    task_scope ping_writes(owner_.worker_, {.resource_ = owner_.resource_});
    begin_idle_probe(ping_writes, timeout_value);
    try {
        for (;;) {
            // The previous event's views end here; its storage returns to the
            // pool before the next reply is allocated.
            current_reply_ = redis_types_access::null_value(owner_.resource_);
            current_reply_ = co_await connection_.read_reply(timeout_value, owner_.resource_, &last_input_);
            if (pending_pings_ == 0 || !is_redis_subscription_pong(current_reply_)) {
                break;
            }
            --pending_pings_;
        }
    } catch (...) {
        failure = std::current_exception();
    }
    end_idle_probe();
    // A failed read closes first so an in-flight PING write wakes at once.
    if (failure != nullptr) {
        fail();
    }
    // A PING write never outlives its read: its deadline is the earlier of
    // one interval and this read's own deadline, and any close ends it sooner.
    co_await ping_writes.join();
    // An internal probe failure (allocation, timer) closed the connection; it
    // outranks the closed-connection error the read observed.
    if (probe_failure_ != nullptr) {
        std::rethrow_exception(std::exchange(probe_failure_, nullptr));
    }
    // An unanswered idle probe closed the connection; whatever the pending
    // read observed, the subscription failed by timing out.
    if (closed_ && closed_reason_ == client_abort_reason::timeout) {
        throw redis_error(redis_error::code_type::timeout, "redis subscription idle probe timed out");
    }
    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const redis_error& error) {
            // The owner ended the subscription: a normal end of the event stream.
            if (error.code() == redis_error::code_type::cancelled || error.code() == redis_error::code_type::closing) {
                co_return std::nullopt;
            }
            throw;
        }
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

void redis_subscription_connection::begin_idle_probe(task_scope& ping_writes, const operation_timeout& read_timeout) noexcept {
    const auto& interval = owner_.config_.subscription_ping_interval_;
    if (!interval.has_value()) {
        return;
    }
    ping_writes_ = &ping_writes;
    read_timeout_ = &read_timeout;
    // Only a waiting read observes input, so the idle clock starts with it.
    read_started_ = clock_type::now();
    probe_started_.reset();
    arm_idle_probe(worker_timer_saturating_deadline(read_started_, worker_timer_saturating_duration_cast(*interval)));
}

void redis_subscription_connection::end_idle_probe() noexcept {
    idle_probe_timer_.cancel();
    ping_writes_ = nullptr;
    read_timeout_ = nullptr;
}

void redis_subscription_connection::arm_idle_probe(clock_type::time_point deadline) noexcept {
    idle_probe_timer_.cancel();
    try {
        // The read that armed the timer cancels it before returning, so the
        // completion never outlives this connection.
        (void)owner_.worker_.schedule_timer(idle_probe_timer_, deadline, [this](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                idle_probe_expired();
            }
        });
    } catch (...) {
        // A read the probe cannot bound must not wait unobserved.
        fail_idle_probe();
    }
}

void redis_subscription_connection::fail_idle_probe() noexcept {
    if (probe_failure_ == nullptr) {
        probe_failure_ = std::current_exception();
    }
    fail();
}

// Re-armed lazily: input only records its time, and an expiry that finds
// newer input moves the deadline instead of acting.
void redis_subscription_connection::idle_probe_expired() noexcept {
    if (closed_ || ping_writes_ == nullptr) {
        return;
    }
    const auto interval = worker_timer_saturating_duration_cast(*owner_.config_.subscription_ping_interval_);
    const auto now = clock_type::now();
    const auto idle_since = std::max(last_input_, read_started_);
    if (probe_started_.has_value() && *probe_started_ < idle_since) {
        // Input after the probe began answered it.
        probe_started_.reset();
    }
    if (probe_started_.has_value()) {
        const auto probe_deadline = worker_timer_saturating_deadline(*probe_started_, interval);
        if (now < probe_deadline) {
            arm_idle_probe(probe_deadline);
            return;
        }
        abort(client_abort_reason::timeout);
        return;
    }
    const auto idle_deadline = worker_timer_saturating_deadline(idle_since, interval);
    if (now < idle_deadline) {
        arm_idle_probe(idle_deadline);
        return;
    }
    probe_started_ = now;
    // A request still being written, or written since the last input, owes a
    // reply that proves liveness as well as a PING would.
    if (!write_active_ && !ping_writing_ && last_request_ <= last_input_) {
        ping_writing_ = true;
        last_request_ = now;
        try {
            // The PING must leave within one interval, as its reply must
            // arrive, and never past the deadline of the read joining it.
            ping_writes_->spawn(write_ping(read_timeout_->constrained_by(owner_.config_.subscription_ping_interval_)));
        } catch (...) {
            ping_writing_ = false;
            fail_idle_probe();
        }
    }
    if (!closed_) {
        arm_idle_probe(worker_timer_saturating_deadline(now, interval));
    }
}

task<void> redis_subscription_connection::write_ping(operation_timeout timeout) {
    try {
        connection_.write_buffer().assign(redis_ping_request);
        ++pending_pings_;
        co_await connection_.write_request(timeout);
    } catch (const redis_error& error) {
        // No caller awaits the PING: its failure closes the connection and
        // the pending read reports it. A stalled write is the probe timing out.
        if (error.code() == redis_error::code_type::timeout) {
            abort(client_abort_reason::timeout);
        } else {
            fail();
        }
    } catch (...) {
        fail_idle_probe();
    }
    ping_writing_ = false;
    ping_written_.notify();
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
