#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ssl/context.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/redis/redis_session.h"

#include "client/client_lifecycle.h"
#include "client/client_tls.h"
#include "integration/operation_lane_lease.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"
#include "redis/redis_reply_parsers.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] asio::ssl::context make_redis_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context_value(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context_value.native_handle(), config);
    }
    return context_value;
}

void validate_redis_registry(
    const worker_ref& worker_value, std::span<const redis_definition_type> redis) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("redis registry requires a valid worker");
    }
    validate_capability_aliases(redis, "redis alias must not be empty", "duplicate redis alias");
}

}  // namespace

redis_registry::redis_registry(asio::io_context& io_context,
    std::span<const redis_definition_type> redis, worker_ref worker_value)
    : runtimes_(),
      alias_index_() {
    validate_redis_registry(worker_value, redis);
    alias_index_.build(redis);
    runtimes_.reserve(redis.size());
    for (const auto& definition : redis) {
        runtimes_.push_back(std::make_unique<redis_client_runtime>(io_context, worker_value,
            redis_config_storage(definition.config_)));
    }
}

redis_registry::~redis_registry() = default;

task<void> redis_registry::connect() {
    for (auto& entry : runtimes_) {
        co_await entry->connect();
    }
    co_return;
}

void redis_registry::close_now() noexcept {
    for (auto& entry : runtimes_) {
        entry->close_now();
    }
}

bool redis_registry::empty() const noexcept {
    return runtimes_.empty();
}

redis_session redis_registry::get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw redis_error(redis_error::code_type::not_configured, "default redis is not configured");
    }
    return runtimes_[*default_pool_index]->session(operation_scope, io_metrics);
}

redis_session redis_registry::get(
    std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return runtimes_[*match]->session(operation_scope, io_metrics);
    }
    throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
}

redis_client_runtime::redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value,
    redis_config_storage config)
    : config_(std::move(config)),
      worker_(worker_value),
      tls_context_(make_redis_tls_context(config_.tls_)),
      general_(io_context, config_, tls_context_, config_.command_timeout_,
          config_.pool_size_per_worker_, worker_),
      // Blocking waits have their own deadlines; ordinary command timeouts
      // must not interrupt a finite long wait or an explicitly cancelled wait.
      blocking_(io_context, config_, tls_context_, std::nullopt,
          config_.blocking_pool_size_per_worker_, worker_),
      subscriptions_(io_context, tls_context_, config_, worker_) {}

redis_client_runtime::~redis_client_runtime() = default;

task<void> redis_client_runtime::connect() {
    // Validate ordinary connections at startup; blocking slots and
    // subscriptions connect lazily.
    co_await general_.connect();
}

void redis_client_runtime::close_now() noexcept {
    general_.close_now();
    blocking_.close_now();
    subscriptions_.close_now();
}

redis_session redis_client_runtime::session(::ruvia::operation_scope& scope, request_io_metrics* io_metrics) {
    return redis_session(*this, scope, io_metrics);
}

redis_session redis_client_runtime::session(::ruvia::operation_scope& scope, operation_options options) {
    return redis_session(*this, scope, std::move(options));
}

class redis_client_state final : public std::enable_shared_from_this<redis_client_state> {
public:
    redis_client_state(event_loop loop, const redis_config& config);

    redis_client_state(const redis_client_state&) = delete;
    redis_client_state& operator=(const redis_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] redis_session redis();

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<redis_client_state>;
    static constexpr std::string_view client_name{"redis"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] redis_client_runtime& backend() noexcept {
        return runtime_;
    }

    event_loop loop_;
    worker_ref worker_;
    redis_client_runtime runtime_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<redis_client_state> lifecycle_;
};
redis_client_state::redis_client_state(event_loop loop, const redis_config& config)
    : loop_(client_lifecycle<redis_client_state>::require_loop(std::move(loop))),
      worker_(loop_.worker()),
      runtime_(loop_.io_context(), worker_, redis_config_storage(config)),
      lifecycle_(*this, loop_, worker_) {}

void redis_client_state::throw_not_ready() {
    throw std::logic_error("redis client is not connected");
}

redis_session redis_client_state::redis() {
    lifecycle_.require_ready();
    // Unbounded blocking commands/subscriptions require an explicit lifetime.
    // Use the client's lifetime as the default, not as an extra user source.
    // A with_options() token replaces it; runtime_.close_now() independently
    // cancels all command pools and subscriptions during client shutdown.
    return runtime_.session(lifecycle_.operation_scope(), {.stop_token_ = lifecycle_.token()});
}

}  // namespace ruvia::detail

namespace ruvia {

redis_client::redis_client(event_loop loop, const redis_config& config)
    : state_(std::make_shared<detail::redis_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

redis_client::~redis_client() {
    state_->request_close();
}

task<void> redis_client::connect() & {
    return state_->connect();
}

redis_session redis_client::redis() const& {
    return state_->redis();
}

void redis_client::close() noexcept {
    state_->request_close();
}

task<void> redis_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& redis_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia

namespace ruvia {

redis_error::redis_error(code_type code, std::string_view message)
    : std::runtime_error(std::string(message)),
      code_(code) {}

redis_error::code_type redis_error::code() const noexcept {
    return code_;
}

redis_value::redis_value() noexcept = default;

redis_value& redis_value::operator=(const redis_value& other) {
    if (this != &other) {
        redis_value owned(other);
        string_.swap(owned.string_);
        array_.swap(owned.array_);
        kind_ = owned.kind_;
        integer_ = owned.integer_;
    }
    return *this;
}

redis_value& redis_value::operator=(redis_value&& other) noexcept {
    if (this != &other) {
        redis_value owned(std::move(other));
        string_.swap(owned.string_);
        array_.swap(owned.array_);
        kind_ = owned.kind_;
        integer_ = owned.integer_;
    }
    return *this;
}

redis_value::kind_type redis_value::kind() const noexcept {
    return kind_;
}

bool redis_value::null() const noexcept {
    return kind_ == kind_type::null;
}

std::string_view redis_value::string() const& {
    if (kind_ != kind_type::string) {
        throw std::logic_error("redis value is not a string");
    }
    return string_;
}

std::string_view redis_value::error() const& {
    if (kind_ != kind_type::error) {
        throw std::logic_error("redis value is not an error");
    }
    return string_;
}

std::int64_t redis_value::integer() const {
    if (kind_ != kind_type::integer) {
        throw std::logic_error("redis value is not an integer");
    }
    return integer_;
}

std::span<const redis_value> redis_value::array() const& {
    if (kind_ != kind_type::array) {
        throw std::logic_error("redis value is not an array");
    }
    return array_;
}

redis_value redis_value::null_value() noexcept {
    redis_value value;
    value.kind_ = kind_type::null;
    return value;
}

redis_value redis_value::string_value(std::string_view input) {
    redis_value value;
    value.kind_ = kind_type::string;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::error_value(std::string_view input) {
    redis_value value;
    value.kind_ = kind_type::error;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::integer_value(std::int64_t input) noexcept {
    redis_value value;
    value.kind_ = kind_type::integer;
    value.integer_ = input;
    return value;
}

redis_value redis_value::array_value(std::size_t capacity) {
    redis_value value;
    value.kind_ = kind_type::array;
    value.array_.reserve(capacity);
    return value;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] bool redis_x_read_group_uses_no_ack(redis_x_read_group_acknowledgement_policy policy) {
    switch (policy) {
        case redis_x_read_group_acknowledgement_policy::track_pending:
            return false;
        case redis_x_read_group_acknowledgement_policy::no_ack:
            return true;
        default:
            throw std::invalid_argument("redis xreadgroup acknowledgement policy is invalid");
    }
}

[[nodiscard]] redis_owned_command encode_redis_pair_command(std::string_view command, std::optional<std::string_view> key,
    std::span<const std::pair<std::string_view, std::string_view>> pairs, const char* empty_message) {
    if (pairs.empty()) {
        throw std::invalid_argument(empty_message);
    }
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        if (key.has_value()) {
            emit(*key);
        }
        for (const auto& [first, second] : pairs) {
            emit(first);
            emit(second);
        }
    });
}
}  // namespace

redis_owned_command make_owned_redis_command(
    std::span<const std::string_view> args) {
    redis_owned_command command{std::string()};
    append_resp_command(command.wire_, args);
    return command;
}

redis_owned_command make_owned_redis_command(std::string_view first, std::span<const std::string_view> rest) {
    redis_owned_command command{std::string()};
    append_resp_command(command.wire_, first, rest);
    return command;
}

redis_owned_command make_owned_redis_command(
    std::initializer_list<std::string_view> args) {
    return make_owned_redis_command(std::span<const std::string_view>(args.begin(), args.size()));
}

redis_owned_command make_redis_integer_command(std::string_view command,
    std::string_view key, std::int64_t value) {
    std::array<char, redis_int_scratch_size> value_scratch{};
    return make_owned_redis_command({command, key, format_redis_int(value_scratch, value)});
}

redis_owned_command make_redis_range_command(std::string_view command,
    std::string_view key, std::int64_t start, std::int64_t stop,
    bool with_scores) {
    std::array<char, redis_int_scratch_size> start_scratch{};
    std::array<char, redis_int_scratch_size> stop_scratch{};
    const std::string_view args[]{
        command,
        key,
        format_redis_int(start_scratch, start),
        format_redis_int(stop_scratch, stop),
        "WITHSCORES",
    };
    return make_owned_redis_command(std::span<const std::string_view>(args, with_scores ? 5U : 4U));
}

redis_owned_command make_redis_zadd_command(std::string_view key, double score,
    std::string_view member) {
    std::array<char, redis_score_scratch_size> score_scratch{};
    return make_owned_redis_command({"ZADD", key, format_redis_score(score_scratch, score), member});
}

bool redis_set_returns_previous(redis_set_previous_value_policy policy) {
    switch (policy) {
        case redis_set_previous_value_policy::discard:
            return false;
        case redis_set_previous_value_policy::return_value:
            return true;
        default:
            throw std::invalid_argument("redis set previous value policy is invalid");
    }
}

redis_owned_command redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys) {
    if (keys.empty()) {
        throw std::invalid_argument("redis command requires at least one key");
    }
    return make_owned_redis_command(command, keys);
}

redis_owned_command redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items) {
    return encode_redis_pair_command("MSET", std::nullopt, items, "redis mset requires at least one item");
}

redis_owned_command redis_set_args(std::string_view key, std::string_view value,
    const redis_set_options& options) {
    std::string_view condition_token;
    if (options.condition_) {
        switch (*options.condition_) {
            case redis_set_condition::if_absent:
                condition_token = "NX";
                break;
            case redis_set_condition::if_present:
                condition_token = "XX";
                break;
            default:
                throw std::invalid_argument("redis set condition is invalid");
        }
    }
    const bool returns_previous = redis_set_returns_previous(options.previous_value_);
    const auto* duration = options.expiration_ ? options.expiration_->duration() : nullptr;
    const bool keeps_existing = options.expiration_ && options.expiration_->keeps_existing();
    std::array<char, redis_int_scratch_size> duration_scratch{};
    const std::string_view duration_text =
        duration != nullptr ? format_redis_int(duration_scratch, static_cast<std::int64_t>(duration->count()))
                            : std::string_view();
    return encode_redis_command([&](const auto& emit) {
        emit("SET");
        emit(key);
        emit(value);
        if (duration != nullptr) {
            emit("PX");
            emit(duration_text);
        }
        if (!condition_token.empty()) {
            emit(condition_token);
        }
        if (returns_previous) {
            emit("GET");
        }
        if (keeps_existing) {
            emit("KEEPTTL");
        }
    });
}

redis_owned_command redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value) {
    return encode_redis_pair_command("HSET", key, fields_value, "redis hset requires at least one field");
}

redis_owned_command redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        emit(key);
        for (const auto field : fields_value) {
            emit(field);
        }
    });
}

redis_owned_command redis_eval_args(std::string_view command, std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> argv) {
    std::array<char, http_max_unsigned_decimal_size> key_count_scratch{};
    const auto key_count = http_format_unsigned(key_count_scratch, keys.size());
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        emit(script);
        emit(key_count);
        for (const auto key : keys) {
            emit(key);
        }
        for (const auto arg : argv) {
            emit(arg);
        }
    });
}

redis_owned_command redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    // Seconds with millisecond precision: digits, '.', three fraction digits.
    std::array<char, redis_int_scratch_size + 4> timeout_scratch{};
    std::string_view timeout_text = "0";
    if (const auto duration = wait.duration(); duration.has_value()) {
        const auto milliseconds = duration->count();
        const auto seconds_text = format_redis_int(
            std::span<char, redis_int_scratch_size>(timeout_scratch.data(), redis_int_scratch_size),
            milliseconds / 1000);
        auto len = seconds_text.size();
        const auto remainder = milliseconds % 1000;
        if (remainder != 0) {
            timeout_scratch[len++] = '.';
            timeout_scratch[len++] = static_cast<char>('0' + remainder / 100);
            timeout_scratch[len++] = static_cast<char>('0' + (remainder / 10) % 10);
            timeout_scratch[len++] = static_cast<char>('0' + remainder % 10);
        }
        timeout_text = std::string_view(timeout_scratch.data(), len);
    }
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        for (const auto key : keys) {
            emit(key);
        }
        emit(timeout_text);
    });
}

redis_owned_command redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options) {
    if (group.empty() || consumer.empty()) {
        throw std::invalid_argument("redis xreadgroup requires a group and consumer");
    }
    if (streams.empty()) {
        throw std::invalid_argument("redis xreadgroup requires at least one stream");
    }
    for (const auto& stream : streams) {
        if (stream.stream_.empty() || stream.id_.empty()) {
            throw std::invalid_argument("redis xreadgroup stream and id must not be empty");
        }
    }
    const auto use_no_ack = redis_x_read_group_uses_no_ack(options.acknowledgement_);
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::string_view count_text;
    if (options.count_.has_value()) {
        count_text = http_format_unsigned(count_scratch, options.count_->value());
    }
    std::array<char, redis_int_scratch_size> block_scratch{};
    std::string_view block_text = "0";
    if (options.block_.has_value()) {
        if (const auto duration = options.block_->duration(); duration.has_value()) {
            block_text = format_redis_int(block_scratch, static_cast<std::int64_t>(duration->count()));
        }
    }
    return encode_redis_command([&](const auto& emit) {
        emit("XREADGROUP");
        emit("GROUP");
        emit(group);
        emit(consumer);
        if (options.count_.has_value()) {
            emit("COUNT");
            emit(count_text);
        }
        if (options.block_.has_value()) {
            emit("BLOCK");
            emit(block_text);
        }
        if (use_no_ack) {
            emit("NOACK");
        }
        emit("STREAMS");
        for (const auto& stream : streams) {
            emit(stream.stream_.view());
        }
        for (const auto& stream : streams) {
            emit(stream.id_.view());
        }
    });
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// An argument-less PING, answered ["pong", ""] while subscribed.
constexpr std::string_view redis_ping_request = "*1\r\n$4\r\nPING\r\n";

}  // namespace

redis_subscription_connection::redis_subscription_connection(redis_subscription_set& owner, operation_options options)
    : owner_(owner),
      options_(std::move(options)),
      connection_(owner.io_context_, owner.tls_context_, owner.worker_, owner.config_, owner.wire_counters_),
      current_reply_(redis_types_access::null_value()),
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

const worker_ref& redis_subscription_connection::worker() const noexcept {
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
    const operation_deadline operation_timeout_value(options_.timeout_);
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
        std::string(command.wire_.get_allocator()).swap(command.wire_);
        // Stamped before the write: the reply may arrive before this
        // coroutine resumes and must still count as newer than the request.
        last_request_ = clock_type::now();
        co_await connection_.write_request(operation_timeout_value.constrained_by(owner_.config_.command_timeout_));
    } catch (...) {
        fail();
        throw;
    }
}

task<std::optional<redis_pubsub_event>> redis_subscription_connection::read(operation_options operation) {
    auto lease = claim_operation_lane(read_active_, "redis subscription read is already in progress");
    if (closed_) {
        co_return std::nullopt;
    }
    if (!ready_) {
        throw std::logic_error("redis subscription has no connection: subscribe first");
    }
    std::optional<operation_interrupt> read_stop;
    try {
        read_stop.emplace(read_stops_, owner_.worker_, operation_options{.stop_token_ = operation.stop_token_}, this,
            &redis_subscription_connection::cancel_read, "redis subscription read");
    } catch (const std::system_error&) {
        // An already-stopped token: nothing was read, the subscription stays open.
        throw redis_error(redis_error::code_type::cancelled, "redis subscription read cancelled");
    }
    const operation_deadline timeout_value(override_operation_options(options_, {.timeout_ = operation.timeout_}).timeout_);
    std::exception_ptr failure;
    task_scope ping_writes(owner_.worker_);
    begin_idle_probe(ping_writes, timeout_value);
    try {
        for (;;) {
            // The previous event's views end here; its storage returns to the
            // pool before the next reply is allocated.
            current_reply_ = redis_types_access::null_value();
            current_reply_ = co_await connection_.read_reply(timeout_value, &last_input_);
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
    // This read's own stop request closed the connection: unlike the
    // subscription's token, it is a failed read, not the end of the stream.
    if (read_stop->interrupted() && closed_reason_ == client_abort_reason::cancelled) {
        throw redis_error(redis_error::code_type::cancelled, "redis subscription read cancelled");
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

void redis_subscription_connection::begin_idle_probe(task_scope& ping_writes, const operation_deadline& read_timeout) noexcept {
    const auto& interval = owner_.config_.subscription_ping_interval_;
    if (!interval.has_value()) {
        return;
    }
    ping_writes_ = &ping_writes;
    read_timeout_ = &read_timeout;
    // Only a waiting read observes input, so the idle clock starts with it.
    read_started_ = clock_type::now();
    probe_started_.reset();
    arm_idle_probe(saturating_deadline(read_started_, interval->value()));
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
    const auto interval = saturating_duration_cast(owner_.config_.subscription_ping_interval_->value());
    const auto now = clock_type::now();
    const auto idle_since = std::max(last_input_, read_started_);
    if (probe_started_.has_value() && *probe_started_ <= idle_since) {
        // Input after the probe began answered it.
        probe_started_.reset();
    }
    if (probe_started_.has_value()) {
        const auto probe_deadline = saturating_deadline(*probe_started_, interval);
        if (now < probe_deadline) {
            arm_idle_probe(probe_deadline);
            return;
        }
        abort(client_abort_reason::timeout);
        return;
    }
    const auto idle_deadline = saturating_deadline(idle_since, interval);
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
        arm_idle_probe(saturating_deadline(now, interval));
    }
}

void redis_subscription_connection::cancel_read(void* connection) noexcept {
    static_cast<redis_subscription_connection*>(connection)->abort(client_abort_reason::cancelled);
}

task<void> redis_subscription_connection::write_ping(operation_deadline timeout) {
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
    const redis_config_storage& config, const worker_ref& worker_value)
    : io_context_(io_context),
      tls_context_(tls_context),
      config_(config),
      worker_(worker_value),
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
    return std::make_unique<redis_subscription_connection>(*this, std::move(options));
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

namespace ruvia {

redis_subscription::redis_subscription(detail::redis_subscription_set& subscriptions, operation_scope& scope,
    operation_options options)
    : connection_(subscriptions.open(std::move(options))),
      registration_(scope, this, &redis_subscription::expire_capability) {}

redis_subscription::~redis_subscription() = default;

void redis_subscription::expire_capability(void* target) noexcept {
    static_cast<redis_subscription*>(target)->connection_->abort(detail::client_abort_reason::closing);
}

task<void> redis_subscription::send(
    std::string_view command, std::span<const std::string_view> names, bool require_names) {
    registration_.require_active();
    if (require_names && names.empty()) {
        throw std::invalid_argument("redis subscription requires at least one channel or pattern");
    }
    return make_scoped_operation(registration_.scope(),
        connection_->send(detail::make_owned_redis_command(command, names)));
}

task<void> redis_subscription::subscribe(std::span<const std::string_view> channels) & {
    return send("SUBSCRIBE", channels, true);
}

task<void> redis_subscription::psubscribe(std::span<const std::string_view> patterns) & {
    return send("PSUBSCRIBE", patterns, true);
}

task<void> redis_subscription::unsubscribe(std::span<const std::string_view> channels) & {
    return send("UNSUBSCRIBE", channels, false);
}

task<void> redis_subscription::punsubscribe(std::span<const std::string_view> patterns) & {
    return send("PUNSUBSCRIBE", patterns, false);
}

task<std::optional<redis_pubsub_event>> redis_subscription::read(operation_options operation) & {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), connection_->read(std::move(operation)));
}

void redis_subscription::abort() noexcept {
    if (!connection_->worker().is_current()) {
        std::terminate();
    }
    connection_->abort(detail::client_abort_reason::closing);
}

}  // namespace ruvia
