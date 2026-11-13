#include "redis/redis_pool.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <openssl/crypto.h>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/model.h"

#include "integration/operation_interrupt.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const worker_ref& require_redis_worker(const worker_ref& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("redis pool requires a valid worker");
    }
    return worker_value;
}

template <typename container_type>
void release_redis_source_storage(container_type& source) noexcept {
    container_type empty(source.get_allocator());
    source.swap(empty);
}

void encode_and_release_redis_source(
    std::string& write_buffer, redis_owned_command& source, bool) {
    if (source.wire_.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    write_buffer.assign(source.wire_);
    release_redis_source_storage(source.wire_);
}

void encode_and_release_redis_source(std::string& write_buffer,
    std::vector<redis_owned_command>& source, bool wrap_transaction) {
    constexpr std::string_view multi_command = "*1\r\n$5\r\nMULTI\r\n";
    constexpr std::string_view exec_command = "*1\r\n$4\r\nEXEC\r\n";
    std::size_t serialized_bytes =
        wrap_transaction ? multi_command.size() + exec_command.size() : 0U;
    for (const auto& command : source) {
        if (command.wire_.empty()) {
            throw std::invalid_argument("redis command must not be empty");
        }
        if (command.wire_.size() > std::numeric_limits<std::size_t>::max() - serialized_bytes) {
            throw std::length_error("redis RESP request is too large");
        }
        serialized_bytes += command.wire_.size();
    }
    write_buffer.reserve(serialized_bytes);
    if (wrap_transaction) {
        write_buffer.append(multi_command);
    }
    for (const auto& command : source) {
        write_buffer.append(command.wire_);
    }
    if (wrap_transaction) {
        write_buffer.append(exec_command);
    }
    release_redis_source_storage(source);
}

void encode_and_release_redis_source(
    std::string& write_buffer, std::vector<std::string>& source, bool) {
    append_resp_command(write_buffer, source);
    release_redis_source_storage(source);
}

void encode_and_release_redis_source(
    std::string& write_buffer, redis_command_args_view source, bool) {
    append_resp_command(write_buffer, source.args_);
}

}  // namespace

redis_pool::redis_pool(asio::io_context& io_context, const redis_config_storage& config,
    asio::ssl::context& tls_context, std::optional<timeout_duration> command_timeout,
    std::size_t pool_size, const worker_ref& worker_value)
    : config_(config),
      command_timeout_(command_timeout),
      slots_(*this, pool_size, require_redis_worker(worker_value)),
      connections_() {
    connections_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        connections_.push_back(std::make_unique<redis_connection>(io_context, tls_context, slots_.worker(), config_, wire_counters_));
    }
}

redis_pool::~redis_pool() {
    close_now();
}

task<void> redis_pool::connect() {
    for (auto& connection : connections_) {
        if (!connection->connected()) {
            co_await connection->connect(nullptr, command_timeout_);
        }
    }
    co_return;
}

void redis_pool::close_now() noexcept {
    if (!slots_.close()) {
        return;
    }
    for (auto& connection : connections_) {
        connection->abort(client_abort_reason::closing);
    }
}

void redis_pool::slot_policy::throw_acquire_failure(const redis_pool&, worker_wait_status status) {
    switch (status) {
        case worker_wait_status::timed_out:
            throw redis_error(redis_error::code_type::timeout, "redis connection pool acquire timed out");
        case worker_wait_status::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
        case worker_wait_status::closed:
        case worker_wait_status::worker_stopping:
            throw redis_error(redis_error::code_type::closing, "redis pool is closing");
        case worker_wait_status::value:
            break;
    }
    std::terminate();
}

void redis_pool::slot_policy::throw_aborted(const redis_pool&, pool_slot_abort_reason reason) {
    switch (reason) {
        case pool_slot_abort_reason::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
        case pool_slot_abort_reason::closing:
            throw redis_error(redis_error::code_type::closing, "redis pool is closing");
        case pool_slot_abort_reason::none:
            break;
    }
    std::terminate();
}

void redis_pool::slot_policy::cancel_slot(redis_pool& pool, std::size_t slot) noexcept {
    pool.connections_[slot]->abort(client_abort_reason::cancelled);
}

task<redis_value> redis_pool::execute_owned(redis_owned_command command, operation_options options,
    std::optional<timeout_duration> reply_timeout, request_io_metrics* io_metrics,
    std::optional<operation_deadline> deadline) {
    return execute_commands<redis_value>(
        std::move(command), std::move(options), reply_timeout, deadline, io_metrics);
}

task<redis_value> redis_pool::execute_owned(std::vector<std::string> args, operation_options options,
    std::optional<timeout_duration> reply_timeout, request_io_metrics* io_metrics,
    std::optional<operation_deadline> deadline) {
    return execute_commands<redis_value>(
        std::move(args), std::move(options), reply_timeout, deadline, io_metrics);
}

task<redis_value> redis_pool::execute_borrowed(redis_command_args_view args, operation_options options, operation_deadline deadline) {
    return execute_commands<redis_value>(args, std::move(options), std::nullopt, deadline);
}

// Own one lease, cancellation registration and failure cleanup for both a
// single command and a pipeline. Borrowed sources remain owned by their
// enclosing caller; owned commands can be released after encoding.
template <typename result_type, typename command_source_type>
task<result_type> redis_pool::execute_commands(command_source_type source, operation_options options,
    std::optional<timeout_duration> reply_timeout,
    std::optional<operation_deadline> deadline, request_io_metrics* io_metrics, bool wrap_transaction) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    constexpr bool single_reply = std::is_same_v<result_type, redis_value>;
    const auto reply_count = [&]() -> std::size_t {
        if constexpr (single_reply) {
            return 1U;
        } else {
            return source.size() + (wrap_transaction ? 2U : 0U);
        }
    }();
    result_type result;
    if constexpr (!single_reply) {
        if (reply_count == 0) {
            co_return result;
        }
    }

    // Ordinary commands start their budget here; an ORM retry retains the
    // deadline established when its enclosing operation started.
    const operation_deadline operation_timeout_value = deadline ? *deadline : operation_deadline(options.timeout_);
    const auto slot = co_await slots_.acquire(operation_timeout_value, config_.acquire_timeout_, options.stop_token_);
    const slot_pool_type::lease_guard lease(slots_, slot);
    slot_pool_type::cancellation_guard cancellation(slots_, slot, options.stop_token_);
    auto& connection = *connections_[slot];
    try {
        co_await connection.connect(&operation_timeout_value, command_timeout_);
        slots_.throw_if_aborted(slot);

        auto& write_buffer = connection.write_buffer();
        write_buffer.clear();
        encode_and_release_redis_source(write_buffer, source, wrap_transaction);

        const auto deadline_value = operation_timeout_value.constrained_by(command_timeout_).constrained_by(reply_timeout);
        co_await connection.write_request(deadline_value);
        if constexpr (single_reply) {
            result = co_await connection.read_reply(deadline_value);
            slots_.throw_if_aborted(slot);
        } else {
            result.reserve(reply_count);
            while (result.size() < reply_count) {
                result.emplace_back(co_await connection.read_reply(deadline_value));
                slots_.throw_if_aborted(slot);
            }
        }
        co_return result;
    } catch (...) {
        cancellation.finish();
        connection.close();
        throw;
    }
}

task<std::vector<redis_value>> redis_pool::execute_pipeline(
    std::vector<redis_owned_command> commands, operation_options options, bool wrap_transaction) {
    return execute_commands<std::vector<redis_value>>(
        std::move(commands), std::move(options), std::nullopt, std::nullopt, nullptr, wrap_transaction);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// Redis has no ALPN identifier, so the TLS handshake offers none.
[[nodiscard]] client_tcp_endpoint redis_endpoint(const redis_config_storage& config) noexcept {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_ = config.tls_.mode_ == client_tls_mode::disabled ? nullptr : &config.tls_,
        .tcp_ = config.tcp_,
        .alpn_ = {}};
}

[[noreturn]] void throw_redis_abort(client_abort_reason reason, std::string_view timeout_message) {
    switch (reason) {
        case client_abort_reason::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
        case client_abort_reason::closing:
            throw redis_error(redis_error::code_type::closing, "redis connection is closing");
        case client_abort_reason::timeout:
        case client_abort_reason::none:
            break;
    }
    throw redis_error(redis_error::code_type::timeout, timeout_message);
}

[[noreturn]] void throw_redis_connect_error(const client_transport_result& result) {
    switch (result.status_) {
        case client_transport_status::aborted:
            throw_redis_abort(result.abort_, "redis connect timed out");
        case client_transport_status::resolve_failed:
            throw redis_error(redis_error::code_type::connect_failed, "redis resolve failed: " + error_code_text(result.error_));
        case client_transport_status::tls_failed:
            throw redis_error(
                redis_error::code_type::connect_failed, "redis TLS handshake failed: " + error_code_text(result.error_));
        case client_transport_status::ok:
        case client_transport_status::eof:
        case client_transport_status::connect_failed:
        case client_transport_status::io_failed:
            break;
    }
    throw redis_error(redis_error::code_type::connect_failed, "redis connect failed: " + error_code_text(result.error_));
}

[[noreturn]] void throw_redis_io_error(const client_transport_result& result) {
    if (result.status_ == client_transport_status::aborted) {
        throw_redis_abort(result.abort_, "redis command timed out");
    }
    if (result.status_ == client_transport_status::eof) {
        throw redis_error(redis_error::code_type::io_error, "redis server closed the connection");
    }
    throw redis_error(redis_error::code_type::io_error, error_code_text(result.error_));
}

}  // namespace

void redis_reader_deleter::operator()(redisReader* reader_value) const noexcept {
    if (reader_value != nullptr) {
        redisReaderFree(reader_value);
    }
}

redis_connection::redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context,
    const worker_ref& worker_value, const redis_config_storage& config, client_wire_counters& counters)
    : config_(config),
      transport_(io_context, tls_context, worker_value, redis_endpoint(config), counters),
      write_buffer_() {}

redis_connection::~redis_connection() = default;

void redis_connection::close() noexcept {
    transport_.close();
    connected_ = false;
    reader_.reset();
    if (reply_builder_ != nullptr) {
        reply_builder_->clear();
    }
    reply_bytes_ = 0;
}

void redis_connection::abort(client_abort_reason reason) noexcept {
    transport_.abort(reason);
    close();
}

void redis_connection::throw_if_aborted() const {
    const auto reason = transport_.abort_reason();
    if (reason == client_abort_reason::cancelled || reason == client_abort_reason::closing) {
        throw_redis_abort(reason, {});
    }
}

void redis_connection::ensure_reader() {
    if (reply_builder_ == nullptr) {
        reply_builder_ =
            std::make_unique<redis_reply_builder>(config_.max_reply_bytes_, config_.max_array_depth_);
    }
    if (reader_ == nullptr) {
        reader_.reset(reply_builder_->create_reader());
    }
    reply_bytes_ = 0;
}

task<void> redis_connection::write_request(const operation_deadline& timeout, redis_request_content content) {
    struct request_buffer_release final {
        std::string& buffer_;
        redis_request_content content_;

        ~request_buffer_release() noexcept {
            if (content_ == redis_request_content::credential) {
                OPENSSL_cleanse(buffer_.data(), buffer_.size());
            }
            http_clear_string_retaining_small(buffer_, redis_retained_write_buffer_bytes);
        }
    } release{write_buffer_, content};
    // Closing the socket does not end an in-flight write's borrow. Retire the
    // buffer only after the transport task completes, including on exception.
    const auto written = co_await transport_.write(write_buffer_, timeout);
    throw_if_aborted();
    if (!written.ok()) {
        throw_redis_io_error(written);
    }
}

task<redis_value> redis_connection::read_reply(const operation_deadline& timeout,
    std::chrono::steady_clock::time_point* input_time) {
    if (!connected_) {
        throw redis_error(redis_error::code_type::io_error, "redis connection is closed");
    }
    ensure_reader();
    reply_builder_->begin();
    for (;;) {
        const auto unread_before = reader_->len - reader_->pos;
        void* raw_reply = nullptr;
        if (redisReaderGetReply(reader_.get(), &raw_reply) != REDIS_OK) {
            reply_builder_->throw_if_rejected();
            throw redis_error(redis_error::code_type::protocol_error, hiredis_reader_error(*reader_));
        }
        std::optional<redis_value> reply;
        if (raw_reply != nullptr) {
            reply.emplace(redis_reply_builder::take(raw_reply));
        }
        const auto unread_after = reader_->len - reader_->pos;
        const auto consumed = unread_before - unread_after;
        // One socket read can contain multiple replies. Only the bytes parsed for
        // this reply count against its limit; if incomplete, any unread bytes
        // still belong to it and must also stay within the limit.
        if (config_.max_reply_bytes_.has_value()) {
            const auto limit = *config_.max_reply_bytes_;
            if (consumed > limit || reply_bytes_ > limit - consumed ||
                (!reply && unread_after >= limit - (reply_bytes_ + consumed))) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "redis reply exceeds configured limit");
            }
        }
        reply_bytes_ += consumed;
        if (reply) {
            // hiredis normally frees an oversized empty buffer only on the
            // next feed. Release it now, without touching pipelined bytes or
            // changing the successful reply if the shrink allocation fails.
            if (reader_->len == reader_->pos && reader_->maxbuf != 0 &&
                sdsalloc(reader_->buf) > reader_->maxbuf) {
                sdsclear(reader_->buf);
                reader_->len = reader_->pos = 0;
                if (auto* compacted = sdsRemoveFreeSpace(reader_->buf)) {
                    reader_->buf = compacted;
                }
            }
            reply_bytes_ = 0;
            co_return std::move(*reply);
        }

        auto read_capacity = read_buffer_.size();
        if (config_.max_reply_bytes_.has_value()) {
            const auto unread = reader_->len - reader_->pos;
            read_capacity = std::min(read_capacity, *config_.max_reply_bytes_ - reply_bytes_ - unread);
        }
        const auto received = co_await transport_.read_some(
            std::span<char>(read_buffer_.data(), read_capacity), timeout);
        // A cancellation or shutdown closed the connection and released its
        // reader; report it before touching the reader again. A close without
        // an owner reason (a failed concurrent write) can still deliver a read
        // that had already completed, so the reader itself is checked too.
        throw_if_aborted();
        if (reader_ == nullptr) {
            throw redis_error(redis_error::code_type::io_error, "redis connection is closed");
        }
        if (!received.ok()) {
            throw_redis_io_error(received);
        }
        if (input_time != nullptr) {
            *input_time = std::chrono::steady_clock::now();
        }
        if (redisReaderFeed(reader_.get(), read_buffer_.data(), received.bytes_) != REDIS_OK) {
            throw redis_error(
                redis_error::code_type::protocol_error, hiredis_reader_error(*reader_));
        }
    }
}

task<void> redis_connection::connect(const operation_deadline* operation_timeout_value,
    std::optional<timeout_duration> command_timeout) {
    if (connected_) {
        // A peer that closed the idle connection or sent unsolicited bytes
        // would desynchronize the next reply; reconnect instead.
        const bool reader_idle = reader_ == nullptr || reader_->len == reader_->pos;
        if (reader_idle && transport_.idle_status() == client_idle_transport_status::reusable) {
            co_return;
        }
        close();
    }
    const auto deadline_value = operation_timeout_value != nullptr
                                    ? operation_timeout_value->constrained_by(config_.connect_timeout_)
                                    : operation_deadline(config_.connect_timeout_);
    try {
        // An expired deadline of the retired connection must not abort this one.
        transport_.reset_abort();
        const auto connected = co_await transport_.connect(deadline_value);
        throw_if_aborted();
        if (!connected.ok()) {
            throw_redis_connect_error(connected);
        }
        connected_ = true;
        ensure_reader();
        co_await authenticate(deadline_value, command_timeout);
    } catch (...) {
        close();
        throw;
    }
}

task<void> redis_connection::authenticate(const operation_deadline& connect_timeout,
    std::optional<timeout_duration> command_timeout) {
    const auto deadline_value = connect_timeout.constrained_by(command_timeout);
    // An ACL user must authenticate by name even without a password (a
    // nopass user accepts any); skipping AUTH would connect as "default".
    if (!config_.username_.empty() || !config_.password_.empty()) {
        const std::string_view password(config_.password_);
        const std::array<std::string_view, 3> user_args{"AUTH", std::string_view(config_.username_), password};
        const std::array<std::string_view, 2> password_args{"AUTH", password};
        const auto auth_args = config_.username_.empty() ? std::span<const std::string_view>(password_args)
                                                         : std::span<const std::string_view>(user_args);
        const auto reply =
            co_await exchange_control_command(auth_args, deadline_value, redis_request_content::credential);
        if (reply.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::auth_failed, reply.error());
        }
    }

    if (config_.database_ != 0) {
        std::array<char, http_max_unsigned_decimal_size> database_digits;
        const std::array<std::string_view, 2> args{
            "SELECT", http_format_unsigned(database_digits, config_.database_)};
        const auto reply = co_await exchange_control_command(args, deadline_value, redis_request_content::ordinary);
        if (reply.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, reply.error());
        }
    }
}

task<redis_value> redis_connection::exchange_control_command(
    std::span<const std::string_view> args, operation_deadline deadline, redis_request_content content) {
    write_buffer_.clear();
    append_resp_command(write_buffer_, args);
    co_await write_request(deadline, content);
    co_return co_await read_reply(deadline);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t resp_decimal_size(std::size_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("redis RESP length is too large");
    }
    return http_unsigned_decimal_size(static_cast<std::uint64_t>(value));
}

template <typename args_type>
[[nodiscard]] std::size_t resp_serialized_size(const args_type& args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    std::size_t size = resp_header_serialized_size(args.size());
    for (const auto& arg : args) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(static_cast<std::size_t>(arg.size())));
    }
    return size;
}

// Writes the RESP multi-bulk encoding of `args` to `output`. Callers reserve
// the required capacity up front (per command in append_resp_command(), or once
// for the whole batch in redis_pool::execute_commands()).
template <typename args_type>
void serialize_resp_command_impl(std::string& output, const args_type& args) {
    append_resp_header(output, args.size());
    for (const auto& arg : args) {
        append_resp_bulk(output, std::string_view(arg.data(), static_cast<std::size_t>(arg.size())));
    }
}

template <typename args_type>
void append_resp_command_impl(std::string& output, const args_type& args) {
    const auto serialized_size = resp_serialized_size(args);
    if (serialized_size > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.reserve(output.size() + serialized_size);
    serialize_resp_command_impl(output, args);
}

[[nodiscard]] redis_reply_builder& builder_of(const redisReadTask* task_value) noexcept {
    return *static_cast<redis_reply_builder*>(task_value->privdata);
}

}  // namespace

void add_resp_serialized_size(std::size_t& current, std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - current) {
        throw std::length_error("redis RESP command is too large");
    }
    current += amount;
}

std::size_t resp_header_serialized_size(std::size_t arg_count) {
    if (arg_count == 0) {
        throw std::invalid_argument("redis command must not be empty");
    }
    std::size_t size = 0;
    add_resp_serialized_size(size, 1);
    add_resp_serialized_size(size, resp_decimal_size(arg_count));
    add_resp_serialized_size(size, 2);
    return size;
}

std::size_t resp_bulk_serialized_size(std::size_t arg_bytes) {
    std::size_t size = 0;
    add_resp_serialized_size(size, 1);
    add_resp_serialized_size(size, resp_decimal_size(arg_bytes));
    add_resp_serialized_size(size, 2);
    add_resp_serialized_size(size, arg_bytes);
    add_resp_serialized_size(size, 2);
    return size;
}

void append_resp_header(std::string& output, std::size_t arg_count) {
    output.push_back('*');
    http_append_unsigned(output, static_cast<std::uint64_t>(arg_count));
    output.append("\r\n", 2);
}

void append_resp_bulk(std::string& output, std::string_view arg) {
    output.push_back('$');
    http_append_unsigned(output, static_cast<std::uint64_t>(arg.size()));
    output.append("\r\n", 2);
    if (!arg.empty()) {
        output.append(arg.data(), arg.size());
    }
    output.append("\r\n", 2);
}

void serialize_resp_command(std::string& output, std::span<const std::string_view> args) {
    serialize_resp_command_impl(output, args);
}

void serialize_resp_command(std::string& output, std::span<const std::string> args) {
    serialize_resp_command_impl(output, args);
}

void serialize_resp_command(
    std::string& output, std::string_view first, std::span<const std::string_view> rest) {
    if (rest.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("redis RESP command is too large");
    }
    append_resp_header(output, rest.size() + 1U);
    append_resp_bulk(output, first);
    for (const auto arg : rest) {
        append_resp_bulk(output, arg);
    }
}

void append_resp_command(std::string& output, std::span<const std::string_view> args) {
    append_resp_command_impl(output, args);
}

void append_resp_command(std::string& output, std::span<const std::string> args) {
    append_resp_command_impl(output, args);
}

void append_resp_command(
    std::string& output, std::string_view first, std::span<const std::string_view> rest) {
    const auto serialized_size = resp_command_serialized_size(first, rest);
    if (serialized_size > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.reserve(output.size() + serialized_size);
    serialize_resp_command(output, first, rest);
}

std::size_t resp_command_serialized_size(std::span<const std::string_view> args) {
    return resp_serialized_size(args);
}

std::size_t resp_command_serialized_size(std::span<const std::string> args) {
    return resp_serialized_size(args);
}

std::size_t resp_command_serialized_size(
    std::string_view first, std::span<const std::string_view> rest) {
    if (rest.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("redis RESP command is too large");
    }
    std::size_t size = resp_header_serialized_size(rest.size() + 1U);
    add_resp_serialized_size(size, resp_bulk_serialized_size(first.size()));
    for (const auto arg : rest) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(arg.size()));
    }
    return size;
}

const char* hiredis_reader_error(const redisReader& reader_value) noexcept {
    return reader_value.errstr[0] == '\0' ? "redis protocol error" : reader_value.errstr;
}

redisReplyObjectFunctions redis_reply_builder::functions_{
    &redis_reply_builder::create_string,
    &redis_reply_builder::create_array,
    &redis_reply_builder::create_integer,
    &redis_reply_builder::create_double,
    &redis_reply_builder::create_nil,
    &redis_reply_builder::create_bool,
    &redis_reply_builder::free_object,
};

redis_reply_builder::redis_reply_builder(std::optional<std::size_t> max_reply_bytes, std::size_t max_depth) noexcept
    : max_reply_bytes_(max_reply_bytes),
      maximum_elements_(max_reply_bytes.has_value() ? *max_reply_bytes / 3 : std::numeric_limits<std::size_t>::max()),
      remaining_elements_(maximum_elements_),
      max_depth_(max_depth) {}

redisReader* redis_reply_builder::create_reader() {
    auto* reader_value = redisReaderCreateWithFunctions(&functions_);
    if (reader_value == nullptr) {
        throw redis_error(redis_error::code_type::protocol_error, "failed to create redis reader");
    }
    reader_value->privdata = this;
    if (max_reply_bytes_.has_value()) {
        // Rejects an oversized array header before any element arrives.
        // Every element requires at least three RESP bytes (e.g. "+\r\n").
        const auto elements = std::max<std::size_t>(1, *max_reply_bytes_ / 3);
        reader_value->maxelements = static_cast<long long>(
            std::min(elements, static_cast<std::size_t>(REDIS_READER_MAX_ARRAY_ELEMENTS)));
    }
    reader_ = reader_value;
    return reader_value;
}

void redis_reply_builder::begin() noexcept {
    if (root_) {
        std::terminate();
    }
    remaining_elements_ = maximum_elements_;
    rejection_ = rejection_type::none;
    failure_ = nullptr;
}

void redis_reply_builder::throw_if_rejected() {
    const auto rejection = rejection_;
    auto failure = std::move(failure_);
    if (rejection != rejection_type::none) {
        clear();
    }
    switch (rejection) {
        case rejection_type::none:
            return;
        case rejection_type::elements:
            throw redis_error(redis_error::code_type::protocol_error, "redis reply exceeds configured element limit");
        case rejection_type::depth:
            throw redis_error(redis_error::code_type::protocol_error, "redis array nesting is too deep");
        case rejection_type::failed:
            std::rethrow_exception(failure);
    }
}

void redis_reply_builder::clear() noexcept {
    root_.reset();
    remaining_elements_ = maximum_elements_;
    rejection_ = rejection_type::none;
    failure_ = nullptr;
}

redis_value redis_reply_builder::take(void* reply) noexcept {
    auto* root = static_cast<root_slot*>(reply);
    auto* owner = root->owner_;
    redis_value value(std::move(root->value_));
    owner->root_.reset();
    return value;
}

// hiredis creates the elements of an array in index order, so each child is
// appended to its parent. Only the innermost open array grows and hiredis
// keeps no pointer to its earlier, completed elements, so a reallocation never
// invalidates a node the reader still refers to. The root stays in this
// connection's builder and hiredis only passes that root to free_object().
template <typename factory_type>
void* redis_reply_builder::place(const redisReadTask* task_value, factory_type&& factory) noexcept {
    try {
        if (task_value->parent != nullptr) {
            auto* parent = task_value->parent->parent == nullptr
                               ? &static_cast<root_slot*>(task_value->parent->obj)->value_
                               : static_cast<redis_value*>(task_value->parent->obj);
            auto& siblings = redis_types_access::elements(*parent);
            siblings.push_back(factory());
            return &siblings.back();
        }
        root_.emplace(*this, factory());
        return &*root_;
    } catch (...) {
        rejection_ = rejection_type::failed;
        failure_ = std::current_exception();
        return nullptr;
    }
}

void* redis_reply_builder::create_string(const redisReadTask* task_value, char* data, std::size_t size) noexcept {
    auto& builder = builder_of(task_value);
    std::string_view text(data, size);
    if (task_value->type == REDIS_REPLY_VERB) {
        // The reader has verified the "fmt:" prefix; the value excludes it.
        text.remove_prefix(4);
    }
    return builder.place(task_value, [&] {
        return task_value->type == REDIS_REPLY_ERROR ? redis_types_access::error_value(text)
                                                     : redis_types_access::string_value(text);
    });
}

void* redis_reply_builder::create_array(const redisReadTask* task_value, std::size_t elements) noexcept {
    auto& builder = builder_of(task_value);
    std::size_t depth = 0;
    for (auto* parent_value = task_value; parent_value != nullptr; parent_value = parent_value->parent) {
        if (++depth > builder.max_depth_) {
            builder.rejection_ = rejection_type::depth;
            return nullptr;
        }
    }
    if (elements > builder.remaining_elements_) {
        builder.rejection_ = rejection_type::elements;
        return nullptr;
    }
    // Reserve only what the buffered bytes can already describe; a header
    // announcing more elements grows the array as they actually arrive.
    const auto buffered = builder.reader_->len - builder.reader_->pos;
    const auto capacity = std::min(elements, buffered / 3);
    auto* node = builder.place(task_value, [&] { return redis_types_access::array_value(capacity); });
    if (node != nullptr) {
        builder.remaining_elements_ -= elements;
    }
    return node;
}

void* redis_reply_builder::create_integer(const redisReadTask* task_value, long long value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::integer_value(static_cast<std::int64_t>(value));
    });
}

void* redis_reply_builder::create_double(const redisReadTask* task_value, double, char* data, std::size_t size) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::string_value(std::string_view(data, size));
    });
}

void* redis_reply_builder::create_nil(const redisReadTask* task_value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] { return redis_types_access::null_value(); });
}

void* redis_reply_builder::create_bool(const redisReadTask* task_value, int value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] { return redis_types_access::integer_value(value == 0 ? 0 : 1); });
}

void redis_reply_builder::free_object(void* reply) noexcept {
    auto* root = static_cast<root_slot*>(reply);
    root->owner_->root_.reset();
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] std::optional<redis_scan_cursor> parse_redis_cursor(std::string_view value) {
    std::uint64_t cursor_value = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), cursor_value);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid redis scan cursor");
    }
    if (cursor_value == 0) {
        return std::nullopt;
    }
    return redis_types_access::scan_cursor(cursor_value);
}

}  // namespace

std::string_view redis_value_string(const redis_value& value) {
    throw_if_redis_error(value);
    if (value.kind() != redis_value::kind_type::string) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not a string");
    }
    return value.string();
}

std::string take_redis_string(redis_value&& value) {
    (void)redis_value_string(value);
    return std::move(redis_types_access::text(value));
}

std::int64_t redis_value_integer(const redis_value& value) {
    throw_if_redis_error(value);
    if (value.kind() != redis_value::kind_type::integer) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not an integer");
    }
    return value.integer();
}

bool redis_value_integer_bool(const redis_value& value) {
    const auto integer = redis_value_integer(value);
    if (integer == 0) {
        return false;
    }
    if (integer == 1) {
        return true;
    }
    throw redis_error(redis_error::code_type::protocol_error, "redis boolean reply is not 0 or 1");
}

std::span<const redis_value> redis_value_array(const redis_value& value) {
    throw_if_redis_error(value);
    if (value.kind() != redis_value::kind_type::array) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not an array");
    }
    return value.array();
}

std::span<redis_value> redis_value_array(redis_value& value) {
    (void)redis_value_array(std::as_const(value));
    return redis_types_access::elements(value);
}

void throw_if_redis_error(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, value.error());
    }
}

void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index) {
    if (value.kind() != redis_value::kind_type::error) {
        return;
    }
    std::array<char, http_max_unsigned_decimal_size> index_digits;
    const auto index_text = http_format_unsigned(index_digits, index);
    std::string message;
    message.reserve(26 + index_text.size() + value.error().size());
    message.append("redis transaction reply ");
    message.append(index_text);
    message.append(": ");
    message.append(value.error());
    throw redis_error(redis_error::code_type::command_error, message);
}

std::optional<std::string> parse_redis_optional_string(redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    return take_redis_string(std::move(value));
}

std::int64_t parse_redis_integer(redis_value&& value) {
    return redis_value_integer(value);
}

bool parse_redis_integer_bool(redis_value&& value) {
    return redis_value_integer_bool(value);
}

void parse_redis_ok(redis_value&& value) {
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis status reply");
    }
}

void parse_redis_pong(redis_value&& value) {
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "PONG")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis ping reply");
    }
}

std::vector<bool> parse_redis_bool_array(redis_value&& value) {
    const auto items = redis_value_array(std::as_const(value));
    std::vector<bool> output;
    output.reserve(items.size());
    for (const auto& item : items) {
        output.emplace_back(redis_value_integer_bool(item));
    }
    return output;
}

std::vector<std::optional<std::string>> parse_redis_optional_string_array(
    redis_value&& value) {
    const auto items = redis_value_array(value);
    std::vector<std::optional<std::string>> output;
    output.reserve(items.size());
    for (auto& item : items) {
        output.emplace_back(parse_redis_optional_string(std::move(item)));
    }
    return output;
}

std::optional<double> parse_redis_optional_double(redis_value&& value, std::string_view context_value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    return parse_redis_double(redis_value_string(value), context_value);
}

redis_set_result parse_redis_set_reply(
    redis_value&& value, const redis_set_options& options) {
    throw_if_redis_error(value);
    const auto returns_previous = redis_set_returns_previous(options.previous_value_);
    if (value.null()) {
        const bool applied =
            returns_previous &&
            (!options.condition_.has_value() || *options.condition_ == redis_set_condition::if_absent);
        return redis_types_access::set_result(applied);
    }
    if (!returns_previous) {
        if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis set reply");
        }
        return redis_types_access::set_result(true);
    }
    const bool applied =
        !options.condition_.has_value() || *options.condition_ != redis_set_condition::if_absent;
    return redis_types_access::set_result(applied, take_redis_string(std::move(value)));
}

redis_ttl parse_redis_ttl(redis_value&& value, bool seconds_precision) {
    const auto integer = redis_value_integer(value);
    if (integer == -2) {
        return redis_types_access::ttl(redis_ttl_state::missing);
    }
    if (integer == -1) {
        return redis_types_access::ttl(redis_ttl_state::persistent);
    }
    if (integer < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid redis TTL reply");
    }
    using milliseconds_type = std::chrono::milliseconds;
    auto milliseconds = integer;
    if (seconds_precision) {
        constexpr auto scale = milliseconds_type(std::chrono::seconds(1)).count();
        if (integer > milliseconds_type::max().count() / scale) {
            throw redis_error(redis_error::code_type::protocol_error, "redis TTL reply exceeds milliseconds range");
        }
        milliseconds *= scale;
    }
    return redis_types_access::ttl(redis_ttl_state::expiring, milliseconds_type(milliseconds));
}

// Redis replies non-finite doubles textually: scores added as +inf/-inf come
// back as "inf"/"-inf", and RESP3-style doubles may also be "nan". Every other
// spelling must be a plain finite decimal.
double parse_redis_double(std::string_view value, std::string_view context_value) {
    if (value == "inf" || value == "+inf") {
        return std::numeric_limits<double>::infinity();
    }
    if (value == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    if (value == "nan") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto output = detail::parse_decimal_number(value);
    if (!output || !std::isfinite(*output)) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    return *output;
}

std::vector<redis_key_value> parse_redis_key_value_array(
    redis_value&& value, std::string_view context_value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    std::vector<redis_key_value> result;
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        result.push_back(redis_types_access::key_value(
            take_redis_string(std::move(values[i])),
            take_redis_string(std::move(values[i + 1]))));
    }
    return result;
}

std::vector<redis_scored_value> parse_redis_scored_array(
    redis_value&& value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis scored array reply");
    }
    std::vector<redis_scored_value> result;
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto score_text = redis_value_string(values[i + 1]);
        result.push_back(redis_types_access::scored_value(
            take_redis_string(std::move(values[i])),
            parse_redis_double(score_text, "invalid redis score")));
    }
    return result;
}

std::vector<std::string> parse_redis_string_array(
    redis_value&& value) {
    const auto items = redis_value_array(value);
    std::vector<std::string> output;
    output.reserve(items.size());
    for (auto& item : items) {
        output.emplace_back(take_redis_string(std::move(item)));
    }
    return output;
}

namespace {

// SCAN-family replies share one envelope: [cursor, payload]. The payload is
// handed to the matching array parser.
template <typename result_type, typename payload_parser_type>
[[nodiscard]] result_type parse_redis_scan_envelope(redis_value&& value, result_type result_value,
    std::string_view context_value, payload_parser_type parse_payload) {
    const auto root = redis_value_array(value);
    if (root.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    redis_types_access::next_cursor(result_value) = parse_redis_cursor(redis_value_string(root[0]));
    parse_payload(result_value, std::move(root[1]));
    return result_value;
}

}  // namespace

redis_scan_result parse_redis_scan_result(redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::scan_result(), "unexpected redis scan reply",
        [](redis_scan_result& result_value, redis_value&& payload) {
            redis_types_access::values(result_value) = parse_redis_string_array(std::move(payload));
        });
}

redis_hash_scan_result parse_redis_hash_scan_result(
    redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::hash_scan_result(), "unexpected redis hscan reply", [](redis_hash_scan_result& result_value, redis_value&& payload) {
        redis_types_access::entries(result_value) =
            parse_redis_key_value_array(std::move(payload), "unexpected redis hscan entry count");
    });
}

redis_z_scan_result parse_redis_z_scan_result(
    redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::z_scan_result(), "unexpected redis zscan reply", [](redis_z_scan_result& result_value, redis_value&& payload) {
        redis_types_access::entries(result_value) = parse_redis_scored_array(std::move(payload));
    });
}

std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = redis_value_array(value);
    if (items.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis blocking pop reply");
    }
    return redis_types_access::key_value(
        take_redis_string(std::move(items[0])),
        take_redis_string(std::move(items[1])));
}

std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }

    auto result_value = redis_types_access::xread_group_result();
    const auto stream_values = redis_value_array(value);
    auto& streams = redis_types_access::streams(result_value);
    streams.reserve(stream_values.size());
    for (auto& stream_value : stream_values) {
        const auto stream_parts = redis_value_array(stream_value);
        if (stream_parts.size() != 2) {
            throw redis_error(
                redis_error::code_type::protocol_error, "unexpected redis xreadgroup stream reply");
        }
        auto stream =
            redis_types_access::stream_read_result(take_redis_string(std::move(stream_parts[0])));
        const auto entry_values = redis_value_array(stream_parts[1]);
        auto& entries = redis_types_access::entries(stream);
        entries.reserve(entry_values.size());
        for (auto& entry_value : entry_values) {
            const auto entry_parts = redis_value_array(entry_value);
            if (entry_parts.size() != 2) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "unexpected redis xreadgroup entry reply");
            }
            auto parsed_entry = redis_types_access::stream_entry(take_redis_string(std::move(entry_parts[0])));
            if (!entry_parts[1].null()) {
                redis_types_access::fields(parsed_entry) = parse_redis_key_value_array(
                    std::move(entry_parts[1]), "unexpected redis xreadgroup field reply");
            }
            entries.emplace_back(std::move(parsed_entry));
        }
        streams.emplace_back(std::move(stream));
    }
    return result_value;
}

// RESP2 subscribed-mode pushes are arrays led by their kind: confirmations
// carry the name (nil when UNSUBSCRIBE finds nothing to leave) and the
// remaining subscription count; messages carry the channel and payload, with
// the matched pattern first for a pattern subscription.
redis_pubsub_event parse_redis_pubsub_event(const redis_value& value) {
    constexpr std::string_view malformed = "unexpected redis pub/sub push";
    const auto items = redis_value_array(value);
    if (items.size() < 3 || items.size() > 4) {
        throw redis_error(redis_error::code_type::protocol_error, malformed);
    }
    const auto kind = redis_value_string(items[0]);
    const auto optional_name = [&](const redis_value& item) -> std::optional<std::string_view> {
        return item.null() ? std::nullopt : std::optional(redis_value_string(item));
    };
    if (items.size() == 3) {
        if (kind == "message") {
            return redis_types_access::pubsub_message(
                redis_value_string(items[1]), std::nullopt, redis_value_string(items[2]));
        }
        const bool pattern = kind == "psubscribe" || kind == "punsubscribe";
        const bool subscribed = kind == "subscribe" || kind == "psubscribe";
        if (!subscribed && kind != "unsubscribe" && kind != "punsubscribe") {
            throw redis_error(redis_error::code_type::protocol_error, malformed);
        }
        const auto name = optional_name(items[1]);
        const auto confirmation_kind = pattern
                                           ? (subscribed ? redis_pubsub_confirmation_kind::pattern_subscribed
                                                         : redis_pubsub_confirmation_kind::pattern_unsubscribed)
                                           : (subscribed ? redis_pubsub_confirmation_kind::subscribed
                                                         : redis_pubsub_confirmation_kind::unsubscribed);
        return redis_types_access::pubsub_confirmation(
            confirmation_kind, name, redis_value_integer(items[2]));
    }
    if (kind != "pmessage") {
        throw redis_error(redis_error::code_type::protocol_error, malformed);
    }
    return redis_types_access::pubsub_message(redis_value_string(items[2]),
        redis_value_string(items[1]), redis_value_string(items[3]));
}

// RESP2 answers PING on a subscribed connection with the push-shaped
// ["pong", <argument>]; once the last subscription is gone the connection is
// back in normal mode and answers +PONG, which is also the RESP3 reply. An
// argument-less PING echoes an empty argument.
bool is_redis_subscription_pong(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::string) {
        return http_ascii_equals_ignore_case(value.string(), "PONG");
    }
    if (value.kind() != redis_value::kind_type::array) {
        return false;
    }
    const auto items = value.array();
    return items.size() == 2 && items[0].kind() == redis_value::kind_type::string &&
           http_ascii_equals_ignore_case(items[0].string(), "pong") &&
           items[1].kind() == redis_value::kind_type::string && items[1].string().empty();
}

}  // namespace ruvia::detail
