#include "redis/redis_pool.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
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

#include <openssl/crypto.h>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_number_format.h"

#include "integration/request_io_metrics.h"
#include "redis/redis_protocol.h"

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
