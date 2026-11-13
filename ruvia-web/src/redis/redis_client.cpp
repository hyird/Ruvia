#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <limits>
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
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/redis/redis_session.h"

#include "client/client_tcp_transport.h"
#include "http/operation_interrupt.h"
#include "redis/redis_pool.h"

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

namespace ruvia::detail {
namespace {

// RediSearch NUMERIC fields are doubles: 2^53 - 1 is the largest magnitude
// below which every integer is stored exactly, so predicate literals and
// written values both stay within it.
constexpr double redis_max_exact_integer = 9007199254740991.0;
constexpr std::string_view present_value = "1";

constexpr std::string_view tag_shadow = "__ruvia_tag_";
constexpr std::string_view sort_shadow = "__ruvia_sort_";
constexpr std::string_view present_shadow = "__ruvia_present_";

using node_kind = redis_predicate_node::kind_type;

[[noreturn]] void throw_inexact_redis_number() {
    throw std::invalid_argument(
        "numeric search values must be finite and within the exact integer range of Redis Search");
}

void require_redis_exact_number(double value) {
    if (!std::isfinite(value) || std::fabs(value) > redis_max_exact_integer) {
        throw_inexact_redis_number();
    }
}

[[nodiscard]] bool is_safe_identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!(ch == '_' || (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_numeric_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::signed_value ||
           kind == redis_column_kind::unsigned_value ||
           kind == redis_column_kind::float32 ||
           kind == redis_column_kind::float64;
}

void require_index_fits_column(redis_index_kind index_kind, redis_column_kind column_kind) {
    switch (index_kind) {
        case redis_index_kind::tag:
            if (column_kind != redis_column_kind::text && column_kind != redis_column_kind::bool_value) {
                throw std::invalid_argument("Redis TAG indexes require string or boolean columns");
            }
            return;
        case redis_index_kind::text:
            if (column_kind != redis_column_kind::text) {
                throw std::invalid_argument("Redis TEXT indexes require string columns");
            }
            return;
        case redis_index_kind::numeric:
            if (!is_numeric_column_kind(column_kind)) {
                throw std::invalid_argument("Redis NUMERIC indexes require numeric columns");
            }
            return;
        case redis_index_kind::none:
            break;
    }
    throw std::invalid_argument("Redis index kind must be TAG, TEXT or NUMERIC");
}

[[nodiscard]] std::string redis_index_name(std::string_view prefix) {
    std::string result_value;
    result_value.reserve(prefix.size() + 4U);
    result_value.append(prefix);
    result_value.append(":idx");
    return result_value;
}

// Shadow hash fields share the reserved __ruvia_ namespace with the entity
// marker. Index columns are safe identifiers (normalize_redis_mapping), and
// queries only name shadows of index columns; entity writes use the declared
// column names as they are.
[[nodiscard]] std::string shadow_field(
    std::string_view shadow, std::string_view field) {
    std::string result_value;
    result_value.reserve(shadow.size() + field.size());
    result_value.append(shadow);
    result_value.append(field);
    return result_value;
}

[[nodiscard]] const redis_column_info& find_column_info(
    std::span<const redis_column_info> columns, std::string_view name) {
    const auto column = std::ranges::find(columns, name, &redis_column_info::name_);
    if (column == columns.end()) {
        throw std::invalid_argument("unknown Redis query column");
    }
    return *column;
}

void validate_entity_prefix(std::string_view entity_prefix, std::string_view table_value) {
    if (!table_value.empty() && table_value != entity_prefix) {
        throw std::invalid_argument("Redis predicate belongs to another entity prefix");
    }
}

// Bound text shares encode_redis_number with entity writes and never narrows a
// literal. A float operand keeps float's encoding for a float32 column and is
// widened for other numeric columns; double operands always keep their width.
// The optional exclusive marker '(' precedes the number in the same buffer.
using redis_numeric_bound_buffer = std::array<char, redis_scalar_scratch_size + 1>;

[[nodiscard]] std::string_view format_canonical_numeric(redis_numeric_bound_buffer& buffer,
    const redis_literal& value, redis_column_kind column_kind, bool exclusive = false) {
    const std::span<char, redis_scalar_scratch_size> scratch(buffer.data() + 1, redis_scalar_scratch_size);
    std::string_view number;
    switch (value.kind()) {
        case redis_literal_kind::signed_integer:
            require_redis_exact_number(static_cast<double>(value.signed_value()));
            number = encode_redis_number(value.signed_value(), scratch);
            break;
        case redis_literal_kind::unsigned_integer:
            require_redis_exact_number(static_cast<double>(value.unsigned_value()));
            number = encode_redis_number(value.unsigned_value(), scratch);
            break;
        case redis_literal_kind::float32: {
            const float real = value.float32_value();
            require_redis_exact_number(real);
            number = column_kind == redis_column_kind::float32
                         ? encode_redis_number(real, scratch)
                         : encode_redis_number(static_cast<double>(real), scratch);
            break;
        }
        case redis_literal_kind::real: {
            const double real = value.real_value();
            require_redis_exact_number(real);
            number = encode_redis_number(real, scratch);
            break;
        }
        default:
            throw std::invalid_argument("Redis numeric predicate requires a numeric value");
    }
    if (!exclusive) {
        return number;
    }
    // The number lies inside scratch, which starts one byte into the buffer.
    const auto offset = static_cast<std::size_t>(number.data() - buffer.data());
    buffer[offset - 1] = '(';
    return std::string_view(buffer.data() + offset - 1, number.size() + 1);
}

// TAG indexes only cover text and boolean columns.
[[nodiscard]] std::string_view raw_tag_value(
    const redis_literal& value, redis_column_kind column_kind) {
    if (value.kind() == redis_literal_kind::string && column_kind == redis_column_kind::text) {
        return value.text();
    }
    if (value.kind() == redis_literal_kind::boolean && column_kind == redis_column_kind::bool_value) {
        return value.boolean_value() ? std::string_view("1") : std::string_view("0");
    }
    throw std::invalid_argument("Redis TAG predicate value type does not match column");
}

// Primary keys are text or integer columns (is_redis_entity_primary_key_scalar).
[[nodiscard]] std::string_view canonical_primary_key(
    const redis_literal& value, redis_column_kind column_kind,
    std::span<char, redis_scalar_scratch_size> scratch) {
    if (column_kind == redis_column_kind::text) {
        if (value.kind() != redis_literal_kind::string) {
            throw std::invalid_argument("Redis TEXT predicate requires a string value");
        }
        return value.text();
    }
    constexpr auto max_signed = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    switch (value.kind()) {
        case redis_literal_kind::signed_integer:
            if (column_kind == redis_column_kind::unsigned_value && value.signed_value() < 0) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(value.signed_value(), scratch);
        case redis_literal_kind::unsigned_integer:
            if (column_kind == redis_column_kind::signed_value && value.unsigned_value() > max_signed) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(value.unsigned_value(), scratch);
        case redis_literal_kind::float32:
        case redis_literal_kind::real: {
            const double number = value.kind() == redis_literal_kind::float32
                                      ? static_cast<double>(value.float32_value())
                                      : value.real_value();
            if (!std::isfinite(number) || std::trunc(number) != number ||
                std::fabs(number) > redis_max_exact_integer ||
                (column_kind == redis_column_kind::unsigned_value && number < 0.0)) {
                throw std::invalid_argument("Redis integer primary key is invalid");
            }
            return encode_redis_number(static_cast<std::int64_t>(number), scratch);
        }
        default:
            throw std::invalid_argument("Redis primary key requires a string or integer value");
    }
}

[[nodiscard]] redis_index_kind index_kind(const redis_mapping& mapping, std::string_view field) {
    const auto kind = mapping.index_kind(field);
    if (kind == redis_index_kind::none) {
        throw std::invalid_argument("Redis query column is not indexed");
    }
    return kind;
}

// The column of a comparison, IN or BETWEEN node and its first value operand:
// a literal, the IN list or the BETWEEN lower bound.
struct redis_column_comparison final {
    const redis_column_info& column_;
    const redis_predicate_node& operand_;
};

[[nodiscard]] redis_column_comparison column_comparison(const redis_mapping& mapping,
    std::span<const redis_predicate_node> nodes, const redis_predicate_node& node) {
    if (node.operands_.size() < 2U || nodes[node.operands_[0]].kind_ != node_kind::field) {
        throw std::logic_error("invalid Redis predicate expression");
    }
    const auto& field = nodes[node.operands_[0]];
    validate_entity_prefix(mapping.entity_prefix_, field.entity_prefix_);
    return {find_column_info(mapping.columns_, field.field_), nodes[node.operands_[1]]};
}

// `@shadow_field:{value}`
void append_shadow_tag_term(
    std::string& output, std::string_view shadow, std::string_view field, std::string_view value) {
    output.reserve(output.size() + 4U + shadow.size() + field.size() + value.size());
    output.push_back('@');
    output.append(shadow);
    output.append(field);
    output.append(":{");
    output.append(value);
    output.push_back('}');
}

// `@field:{x<hex>}`
void append_encoded_tag_term(
    std::string& output, std::string_view field, std::string_view raw_value) {
    output.reserve(output.size() + 5U + field.size() + raw_value.size() * 2U);
    output.push_back('@');
    output.append(field);
    output.append(":{x");
    http_append_lower_hex(output, raw_value);
    output.push_back('}');
}

// `@field:[low high]`; a bound prefixed with '(' is exclusive.
void append_numeric_range(
    std::string& output, std::string_view field, std::string_view low, std::string_view high) {
    output.reserve(output.size() + 5U + field.size() + low.size() + high.size());
    output.push_back('@');
    output.append(field);
    output.append(":[");
    output.append(low);
    output.push_back(' ');
    output.append(high);
    output.push_back(']');
}

void append_presence(std::string& output, std::string_view field) {
    // Presence markers are written as the raw value 1. They are indexed as a
    // TAG shadow field, but are not passed through the user TAG encoding.
    append_shadow_tag_term(output, present_shadow, field, present_value);
}

// `field IS NULL` / `field IS NOT NULL` through the presence shadow.
void append_null_comparison(std::string& output, std::string_view field,
    redis_binary_operator operation, const char* unsupported) {
    if (operation == redis_binary_operator::equal) {
        output.push_back('-');
        append_presence(output, field);
    } else if (operation == redis_binary_operator::not_equal) {
        append_presence(output, field);
    } else {
        throw std::invalid_argument(unsupported);
    }
}

void append_tag_comparison(std::string& output, std::string_view field, const redis_literal& value,
    redis_column_kind column_kind, redis_binary_operator operation) {
    if (value.kind() == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL TAG comparison");
        return;
    }
    const auto raw = raw_tag_value(value, column_kind);
    switch (operation) {
        case redis_binary_operator::equal:
            append_encoded_tag_term(output, field, raw);
            return;
        case redis_binary_operator::not_equal:
            output.reserve(
                output.size() + 14U + present_shadow.size() + field.size() * 2U + raw.size() * 2U);
            output.push_back('(');
            append_presence(output, field);
            output.append(" -");
            append_encoded_tag_term(output, field, raw);
            output.push_back(')');
            return;
        default:
            throw std::invalid_argument("Redis TAG indexes only support equality predicates");
    }
}

void append_numeric_comparison(std::string& output, std::string_view field, const redis_literal& value,
    redis_column_kind column_kind, redis_binary_operator operation) {
    if (value.kind() == redis_literal_kind::null) {
        append_null_comparison(output, field, operation, "unsupported NULL NUMERIC comparison");
        return;
    }
    redis_numeric_bound_buffer number_buffer;
    switch (operation) {
        case redis_binary_operator::equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            append_numeric_range(output, field, number, number);
            return;
        }
        case redis_binary_operator::not_equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            output.reserve(output.size() + 23U + field.size() * 2U + number.size() * 2U);
            output.push_back('(');
            append_numeric_range(output, field, "-inf", "+inf");
            output.append(" -");
            append_numeric_range(output, field, number, number);
            output.push_back(')');
            return;
        }
        case redis_binary_operator::less: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind, true);
            append_numeric_range(output, field, "-inf", number);
            return;
        }
        case redis_binary_operator::less_equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            append_numeric_range(output, field, "-inf", number);
            return;
        }
        case redis_binary_operator::greater: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind, true);
            append_numeric_range(output, field, number, "+inf");
            return;
        }
        case redis_binary_operator::greater_equal: {
            const auto number = format_canonical_numeric(number_buffer, value, column_kind);
            append_numeric_range(output, field, number, "+inf");
            return;
        }
        default:
            throw std::invalid_argument("unsupported Redis NUMERIC comparison");
    }
}

// Compiles one predicate tree against one repository mapping.
class redis_query_compiler final {
public:
    redis_query_compiler(const redis_mapping& mapping) noexcept
        : mapping_(mapping) {}

    void append_predicate(std::string& output, const redis_predicate_storage& predicate) const {
        append_expression(output, predicate.nodes_, predicate.nodes_[predicate.root_], 0U);
    }

    void validate_sort_field(std::string_view field) const {
        (void)find_column_info(mapping_.columns_, field);
        if (!mapping_.sortable(field)) {
            throw std::invalid_argument("Redis order column is not sortable");
        }
    }

    [[nodiscard]] std::string sort_field(std::string_view field) const {
        validate_sort_field(field);
        if (mapping_.index_kind(field) == redis_index_kind::tag) {
            return shadow_field(sort_shadow, field);
        }
        return std::string(field);
    }

private:
    using nodes_type = std::span<const redis_predicate_node>;

    void append_expression(std::string& output, nodes_type nodes, const redis_predicate_node& node,
        std::size_t depth) const {
        constexpr std::size_t max_depth = 64U;
        if (depth > max_depth) {
            throw std::invalid_argument("Redis predicate nesting is too deep");
        }
        if (node.kind_ == node_kind::between) {
            append_between(output, nodes, node);
            return;
        }
        if (node.kind_ == node_kind::logical_not) {
            if (node.operands_.empty()) {
                throw std::logic_error("invalid Redis predicate expression");
            }
            output.append("-(");
            append_expression(output, nodes, nodes[node.operands_[0]], depth + 1U);
            output.push_back(')');
            return;
        }
        switch (node.binary_) {
            case redis_binary_operator::logical_and:
            case redis_binary_operator::logical_or:
                if (node.operands_.size() < 2U) {
                    throw std::logic_error("invalid Redis predicate expression");
                }
                output.push_back('(');
                append_expression(output, nodes, nodes[node.operands_[0]], depth + 1U);
                output.push_back(node.binary_ == redis_binary_operator::logical_and ? ' ' : '|');
                append_expression(output, nodes, nodes[node.operands_[1]], depth + 1U);
                output.push_back(')');
                return;
            case redis_binary_operator::in:
            case redis_binary_operator::not_in:
                append_in_list(output, nodes, node);
                return;
            default:
                append_comparison(output, nodes, node);
                return;
        }
    }

    void append_comparison(std::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& value = comparison.operand_.literal_;
        if (comparison.column_.primary_key_ && comparison.column_.kind_ == redis_column_kind::text &&
            value.kind() == redis_literal_kind::string && value.text().empty()) {
            throw std::invalid_argument("Redis primary key cannot be empty");
        }
        switch (index_kind(mapping_, field)) {
            case redis_index_kind::tag:
                append_tag_comparison(output, field, value, comparison.column_.kind_, node.binary_);
                return;
            case redis_index_kind::numeric:
                append_numeric_comparison(output, field, value, comparison.column_.kind_, node.binary_);
                return;
            default:
                break;
        }
        // A TEXT index only answers presence tests.
        if (value.kind() == redis_literal_kind::null &&
            (node.binary_ == redis_binary_operator::equal || node.binary_ == redis_binary_operator::not_equal)) {
            append_null_comparison(output, field, node.binary_, {});
            return;
        }
        throw std::invalid_argument("Redis TEXT predicates are not exposed by redis_predicate");
    }

    void append_in_list(std::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        const auto field = comparison.column_.name_;
        const auto& items = comparison.operand_.operands_;
        const bool negated = node.binary_ == redis_binary_operator::not_in;
        const auto kind = index_kind(mapping_, field);
        if (kind == redis_index_kind::text) {
            throw std::invalid_argument("Redis TEXT predicates are not supported");
        }
        if (items.empty() && negated) {
            append_presence(output, field);
            return;
        }
        if (items.empty()) {
            // Presence markers only ever hold 1, so this matches nothing.
            append_shadow_tag_term(output, present_shadow, field, "x00");
            return;
        }
        output.push_back('(');
        if (negated) {
            append_presence(output, field);
            output += " -(";
        }
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (index != 0U) {
                output.push_back('|');
            }
            const auto& value = nodes[items[index]].literal_;
            if (value.kind() == redis_literal_kind::null) {
                throw std::invalid_argument("Redis IN list requires non-null literal values");
            }
            if (comparison.column_.primary_key_ && comparison.column_.kind_ == redis_column_kind::text &&
                value.kind() == redis_literal_kind::string && value.text().empty()) {
                throw std::invalid_argument("Redis primary key cannot be empty");
            }
            if (kind == redis_index_kind::tag) {
                append_encoded_tag_term(
                    output, field, raw_tag_value(value, comparison.column_.kind_));
            } else {
                redis_numeric_bound_buffer number_buffer;
                const auto number =
                    format_canonical_numeric(number_buffer, value, comparison.column_.kind_);
                append_numeric_range(output, field, number, number);
            }
        }
        output.push_back(')');
        if (negated) {
            output.push_back(')');
        }
    }

    void append_between(std::string& output, nodes_type nodes, const redis_predicate_node& node) const {
        const auto comparison = column_comparison(mapping_, nodes, node);
        if (node.operands_.size() < 3U) {
            throw std::logic_error("invalid Redis predicate expression");
        }
        const auto field = comparison.column_.name_;
        const auto& lower = comparison.operand_.literal_;
        const auto& upper = nodes[node.operands_[2]].literal_;
        if (index_kind(mapping_, field) != redis_index_kind::numeric ||
            lower.kind() == redis_literal_kind::null || upper.kind() == redis_literal_kind::null) {
            throw std::invalid_argument("Redis BETWEEN requires a NUMERIC indexed column");
        }
        redis_numeric_bound_buffer low_buffer;
        redis_numeric_bound_buffer high_buffer;
        const auto low = format_canonical_numeric(low_buffer, lower, comparison.column_.kind_);
        const auto high = format_canonical_numeric(high_buffer, upper, comparison.column_.kind_);
        append_numeric_range(output, field, low, high);
    }

    const redis_mapping& mapping_;
};

// Written values are our own encodings of numeric columns, so this only
// enforces the exact range RediSearch can index.
void require_exact_numeric_text(std::string_view value) {
    double number = 0.0;
    const auto parsed_value = std::from_chars(value.data(), value.data() + value.size(), number,
        std::chars_format::general);
    if (parsed_value.ec != std::errc{} || parsed_value.ptr != value.data() + value.size()) {
        throw_inexact_redis_number();
    }
    require_redis_exact_number(number);
}

template <typename field_type, typename value_type>
void append_set(redis_orm_arguments_type& args, field_type&& field, value_type&& value) {
    args.emplace_back("s");
    args.emplace_back(std::forward<field_type>(field));
    args.emplace_back(std::forward<value_type>(value));
}

template <typename field_type>
void append_delete(redis_orm_arguments_type& args, field_type&& field) {
    args.emplace_back("d");
    args.emplace_back(std::forward<field_type>(field));
    args.emplace_back();
}

}  // namespace

redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns, std::string_view entity_prefix,
    const redis_repository_config& config) {
    const std::string_view prefix = config.prefix_.empty() ? entity_prefix : std::string_view(config.prefix_);
    if (prefix.empty()) {
        throw std::invalid_argument("Redis entity prefix cannot be empty");
    }
    if (prefix.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid Redis entity prefix");
    }
    for (const auto& column : columns) {
        if (column.name_.starts_with("__ruvia_")) {
            throw std::invalid_argument("Redis entity column uses reserved __ruvia_ namespace");
        }
    }
    redis_mapping result_value{columns, std::string(entity_prefix), std::string(prefix),
        redis_entity_storage_prefix(prefix), std::vector<redis_index_definition>{},
        std::vector<redis_index_kind>(columns.size(), redis_index_kind::none)};
    result_value.indexes_.reserve(config.indexes_.size());
    for (const auto& index : config.indexes_) {
        const std::string_view field = index.column_;
        const auto column = std::ranges::find(columns, field, &redis_column_info::name_);
        if (column == columns.end()) {
            throw std::invalid_argument("unknown Redis index column");
        }
        auto& column_index_kind = result_value.column_index_kinds_[static_cast<std::size_t>(column - columns.begin())];
        if (column_index_kind != redis_index_kind::none) {
            throw std::invalid_argument("duplicate Redis index column");
        }
        // Index columns name query fields and shadow hash fields.
        if (!is_safe_identifier(field)) {
            throw std::invalid_argument("invalid Redis index column name");
        }
        require_index_fits_column(index.kind_, column->kind_);
        result_value.indexes_.push_back({std::string(field), index.kind_, index.sortable_});
        column_index_kind = index.kind_;
    }
    return result_value;
}

std::string encode_redis_tag(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 2U + 1U);
    encoded.push_back('x');
    http_append_lower_hex(encoded, value);
    return encoded;
}

}  // namespace ruvia::detail

namespace ruvia {

redis_search_mapping::redis_search_mapping(const redis_search_mapping& other)
    : mapping_{other.mapping_.columns_, std::string(other.mapping_.entity_prefix_),
          std::string(other.mapping_.prefix_), std::string(other.mapping_.storage_prefix_),
          std::vector<detail::redis_index_definition>{},
          std::vector<redis_index_kind>(other.mapping_.column_index_kinds_)} {
    mapping_.indexes_.reserve(other.mapping_.indexes_.size());
    for (const auto& index : other.mapping_.indexes_) {
        mapping_.indexes_.push_back({std::string(index.column_), index.kind_, index.sortable_});
    }
}

redis_search_mapping& redis_search_mapping::operator=(const redis_search_mapping& other) {
    if (this != &other) {
        redis_search_mapping replacement(other);
        swap(replacement);
    }
    return *this;
}

redis_search_mapping& redis_search_mapping::operator=(redis_search_mapping&& other) {
    if (this != &other) {
        swap(other);
    }
    return *this;
}

void redis_search_mapping::swap(redis_search_mapping& other) noexcept {
    std::swap(mapping_.columns_, other.mapping_.columns_);
    mapping_.entity_prefix_.swap(other.mapping_.entity_prefix_);
    mapping_.prefix_.swap(other.mapping_.prefix_);
    mapping_.storage_prefix_.swap(other.mapping_.storage_prefix_);
    mapping_.indexes_.swap(other.mapping_.indexes_);
    mapping_.column_index_kinds_.swap(other.mapping_.column_index_kinds_);
}

void redis_search_mapping::require_valid() const {
    if (mapping_.prefix_.empty()) {
        throw std::logic_error("Redis search mapping has been moved from");
    }
}

redis_compiled_command redis_search_mapping::compile_find(
    const redis_find_options& options, redis_search_mode mode) const {
    require_valid();
    const auto& mapping = mapping_;
    const detail::redis_query_compiler compiler(mapping);
    const auto skip = options.skip_ ? options.skip_->value() : 0U;
    const auto requested_take = options.take_ ? options.take_->value() : redis_find_default_take;
    if (!std::in_range<std::int64_t>(skip) || !std::in_range<std::int64_t>(requested_take)) {
        throw std::invalid_argument("Redis search skip and take must fit in 64-bit signed range");
    }
    std::uint64_t take = requested_take;
    bool count_only = false;
    switch (mode) {
        case redis_search_mode::rows:
            break;
        case redis_search_mode::first:
            take = (std::min)(take, std::uint64_t{1});
            break;
        case redis_search_mode::count:
            count_only = true;
            break;
        default:
            throw std::invalid_argument("invalid Redis search mode");
    }
    std::optional<std::string> sort_field_name;
    if (options.order_.has_value()) {
        const auto& order = *options.order_;
        if (count_only) {
            compiler.validate_sort_field(order.column_);
        } else {
            sort_field_name = compiler.sort_field(order.column_);
        }
        switch (order.direction_) {
            case redis_order_direction::asc:
            case redis_order_direction::desc:
                break;
            default:
                throw std::invalid_argument("invalid Redis order direction");
        }
    }
    std::string query;
    if (const auto* predicate = detail::redis_predicate_access::storage(options.where_)) {
        query.reserve(predicate->nodes_.size() * 24U);
        compiler.append_predicate(query, *predicate);
    } else {
        query = "*";
    }
    std::vector<std::string> args;
    args.reserve(sort_field_name.has_value() ? 11U : 8U);
    args.emplace_back("FT.SEARCH");
    args.push_back(detail::redis_index_name(mapping.prefix_));
    args.emplace_back(std::move(query));
    args.emplace_back("LIMIT");
    if (count_only) {
        args.emplace_back("0");
        args.emplace_back("0");
    } else {
        std::string encoded_skip, encoded_take;
        http_append_unsigned(encoded_skip, skip);
        http_append_unsigned(encoded_take, take);
        args.emplace_back(std::move(encoded_skip));
        args.emplace_back(std::move(encoded_take));
    }
    if (sort_field_name.has_value()) {
        args.emplace_back("SORTBY");
        args.push_back(std::move(*sort_field_name));
        args.emplace_back(options.order_->direction_ == redis_order_direction::asc ? "ASC" : "DESC");
    }
    args.emplace_back("DIALECT");
    args.emplace_back("2");
    return redis_compiled_command(std::move(args));
}

redis_compiled_command redis_search_mapping::compile_create_index() const {
    require_valid();
    const auto& mapping = mapping_;
    // A mapping without indexes is valid for key-based access only.
    if (mapping.indexes_.empty()) {
        throw std::invalid_argument("Redis repository requires at least one index column");
    }
    std::size_t arg_count = 8U;
    for (const auto& definition : mapping.indexes_) {
        if (definition.kind_ == redis_index_kind::tag) {
            arg_count += definition.sortable_ ? 16U : 10U;
        } else {
            arg_count += definition.sortable_ ? 8U : 7U;
        }
    }
    std::vector<std::string> args;
    args.reserve(arg_count);
    args.emplace_back("FT.CREATE");
    args.push_back(detail::redis_index_name(mapping.prefix_));
    args.emplace_back("ON");
    args.emplace_back("HASH");
    args.emplace_back("PREFIX");
    args.emplace_back("1");
    args.emplace_back(mapping.storage_prefix_);
    args.emplace_back("SCHEMA");
    for (const auto& definition : mapping.indexes_) {
        const std::string_view field = definition.column_;
        if (definition.kind_ == redis_index_kind::tag) {
            args.push_back(detail::shadow_field(detail::tag_shadow, field));
            args.emplace_back("AS");
            args.emplace_back(field);
            args.emplace_back("TAG");
            args.emplace_back("CASESENSITIVE");
        } else {
            args.emplace_back(field);
            args.emplace_back(definition.kind_ == redis_index_kind::text ? "TEXT" : "NUMERIC");
            if (definition.sortable_) {
                args.emplace_back("SORTABLE");
            }
        }
        auto presence = detail::shadow_field(detail::present_shadow, field);
        args.emplace_back(presence);
        args.emplace_back("AS");
        args.push_back(std::move(presence));
        args.emplace_back("TAG");
        args.emplace_back("CASESENSITIVE");
        if (definition.kind_ == redis_index_kind::tag && definition.sortable_) {
            args.emplace_back(field);
            args.emplace_back("AS");
            args.push_back(detail::shadow_field(detail::sort_shadow, field));
            args.emplace_back("TAG");
            args.emplace_back("SORTABLE");
            args.emplace_back("NOINDEX");
        }
    }
    return redis_compiled_command(std::move(args));
}

redis_compiled_command redis_search_mapping::compile_drop_index() const {
    require_valid();
    std::vector<std::string> args;
    args.reserve(2U);
    args.emplace_back("FT.DROPINDEX");
    args.push_back(detail::redis_index_name(mapping_.prefix_));
    return redis_compiled_command(std::move(args));
}

}  // namespace ruvia

namespace ruvia::detail {

std::optional<std::string_view> redis_primary_key(
    const redis_mapping& mapping, const redis_predicate& predicate,
    std::span<char, redis_scalar_scratch_size> scratch) {
    const auto* storage = redis_predicate_access::storage(predicate);
    if (storage == nullptr) {
        return std::nullopt;
    }
    const auto& root = storage->nodes_[storage->root_];
    if (root.kind_ != node_kind::binary || root.binary_ != redis_binary_operator::equal) {
        return std::nullopt;
    }
    const auto comparison = column_comparison(mapping, storage->nodes_, root);
    if (!comparison.column_.primary_key_) {
        return std::nullopt;
    }
    const auto& value = comparison.operand_.literal_;
    if (value.kind() == redis_literal_kind::null) {
        throw std::invalid_argument("Redis primary key cannot be NULL");
    }
    const auto result_value = canonical_primary_key(value, comparison.column_.kind_, scratch);
    if (result_value.empty()) {
        throw std::invalid_argument("Redis primary key cannot be empty");
    }
    return result_value;
}

void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field,
    std::optional<std::string_view> value, bool numeric_index, bool tag_shadow_value) {
    if (!value) {
        append_delete(args, field);
        append_delete(args, shadow_field(present_shadow, field));
        if (tag_shadow_value) {
            append_delete(args, shadow_field(tag_shadow, field));
        }
        return;
    }
    if (numeric_index) {
        require_exact_numeric_text(*value);
    }
    append_set(args, field, *value);
    append_set(args, shadow_field(present_shadow, field), present_value);
    if (tag_shadow_value) {
        append_set(args, shadow_field(tag_shadow, field), encode_redis_tag(*value));
    }
}

}  // namespace ruvia::detail
