#include "redis_pool.h"

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
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ssl/context.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "event_loop.h"
#include "http_client_pool.h"
#include "http_header.h"
#include "model.h"
#include "redis.h"
#include "stop_token.h"
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

void encode_and_release_redis_source(std::string& write_buffer, redis_owned_command& source, bool) {
    if (source.wire_.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    write_buffer.assign(source.wire_);
    release_redis_source_storage(source.wire_);
}

void encode_and_release_redis_source(std::string& write_buffer, std::vector<redis_owned_command>& source, bool wrap_transaction) {
    constexpr std::string_view multi_command = "*1\r\n$5\r\nMULTI\r\n";
    constexpr std::string_view exec_command = "*1\r\n$4\r\nEXEC\r\n";
    std::size_t serialized_bytes = wrap_transaction ? multi_command.size() + exec_command.size() : 0U;
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

void encode_and_release_redis_source(std::string& write_buffer, std::vector<std::string>& source, bool) {
    append_resp_command(write_buffer, source);
    release_redis_source_storage(source);
}

void encode_and_release_redis_source(std::string& write_buffer, redis_command_args_view source, bool) {
    append_resp_command(write_buffer, source.args_);
}

}  // namespace

redis_pool::redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context& tls_context, std::optional<timeout_duration> command_timeout, std::size_t pool_size, const worker_ref& worker_value) :
    config_(config),
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

task<redis_value> redis_pool::execute_owned(redis_owned_command command, operation_options options, std::optional<timeout_duration> reply_timeout, request_io_metrics* io_metrics, std::optional<operation_deadline> deadline) {
    return execute_commands<redis_value>(std::move(command), std::move(options), reply_timeout, deadline, io_metrics);
}

task<redis_value> redis_pool::execute_owned(std::vector<std::string> args, operation_options options, std::optional<timeout_duration> reply_timeout, request_io_metrics* io_metrics, std::optional<operation_deadline> deadline) {
    return execute_commands<redis_value>(std::move(args), std::move(options), reply_timeout, deadline, io_metrics);
}

task<redis_value> redis_pool::execute_borrowed(redis_command_args_view args, operation_options options, operation_deadline deadline) {
    return execute_commands<redis_value>(args, std::move(options), std::nullopt, deadline);
}

// Own one lease, cancellation registration and failure cleanup for both a
// single command and a pipeline. Borrowed sources remain owned by their
// enclosing caller; owned commands can be released after encoding.
template <typename result_type, typename command_source_type>
task<result_type> redis_pool::execute_commands(command_source_type source, operation_options options, std::optional<timeout_duration> reply_timeout, std::optional<operation_deadline> deadline, request_io_metrics* io_metrics, bool wrap_transaction) {
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

task<std::vector<redis_value>> redis_pool::execute_pipeline(std::vector<redis_owned_command> commands, operation_options options, bool wrap_transaction) {
    return execute_commands<std::vector<redis_value>>(std::move(commands), std::move(options), std::nullopt, std::nullopt, nullptr, wrap_transaction);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// Redis has no ALPN identifier, so the TLS handshake offers none.
[[nodiscard]] client_tcp_endpoint redis_endpoint(const redis_config_storage& config) noexcept {
    return {.host_ = config.host_, .port_ = config.port_, .tls_ = config.tls_.mode_ == client_tls_mode::disabled ? nullptr : &config.tls_, .tcp_ = config.tcp_, .alpn_ = {}};
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
            throw redis_error(redis_error::code_type::connect_failed, "redis TLS handshake failed: " + error_code_text(result.error_));
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

redis_connection::redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context, const worker_ref& worker_value, const redis_config_storage& config, client_wire_counters& counters) :
    config_(config),
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
        reply_builder_ = std::make_unique<redis_reply_builder>(config_.max_reply_bytes_, config_.max_array_depth_);
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

task<redis_value> redis_connection::read_reply(const operation_deadline& timeout, std::chrono::steady_clock::time_point* input_time) {
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
            if (consumed > limit || reply_bytes_ > limit - consumed || (!reply && unread_after >= limit - (reply_bytes_ + consumed))) {
                throw redis_error(redis_error::code_type::protocol_error, "redis reply exceeds configured limit");
            }
        }
        reply_bytes_ += consumed;
        if (reply) {
            // hiredis normally frees an oversized empty buffer only on the
            // next feed. Release it now, without touching pipelined bytes or
            // changing the successful reply if the shrink allocation fails.
            if (reader_->len == reader_->pos && reader_->maxbuf != 0 && sdsalloc(reader_->buf) > reader_->maxbuf) {
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
        const auto received = co_await transport_.read_some(std::span<char>(read_buffer_.data(), read_capacity), timeout);
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
            throw redis_error(redis_error::code_type::protocol_error, hiredis_reader_error(*reader_));
        }
    }
}

task<void> redis_connection::connect(const operation_deadline* operation_timeout_value, std::optional<timeout_duration> command_timeout) {
    if (connected_) {
        // A peer that closed the idle connection or sent unsolicited bytes
        // would desynchronize the next reply; reconnect instead.
        const bool reader_idle = reader_ == nullptr || reader_->len == reader_->pos;
        if (reader_idle && transport_.idle_status() == client_idle_transport_status::reusable) {
            co_return;
        }
        close();
    }
    const auto deadline_value = operation_timeout_value != nullptr ? operation_timeout_value->constrained_by(config_.connect_timeout_) : operation_deadline(config_.connect_timeout_);
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

task<void> redis_connection::authenticate(const operation_deadline& connect_timeout, std::optional<timeout_duration> command_timeout) {
    const auto deadline_value = connect_timeout.constrained_by(command_timeout);
    // An ACL user must authenticate by name even without a password (a
    // nopass user accepts any); skipping AUTH would connect as "default".
    if (!config_.username_.empty() || !config_.password_.empty()) {
        const std::string_view password(config_.password_);
        const std::array<std::string_view, 3> user_args{"AUTH", std::string_view(config_.username_), password};
        const std::array<std::string_view, 2> password_args{"AUTH", password};
        const auto auth_args = config_.username_.empty() ? std::span<const std::string_view>(password_args) : std::span<const std::string_view>(user_args);
        const auto reply = co_await exchange_control_command(auth_args, deadline_value, redis_request_content::credential);
        if (reply.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::auth_failed, reply.error());
        }
    }

    if (config_.database_ != 0) {
        std::array<char, http_max_unsigned_decimal_size> database_digits;
        const std::array<std::string_view, 2> args{"SELECT", http_format_unsigned(database_digits, config_.database_)};
        const auto reply = co_await exchange_control_command(args, deadline_value, redis_request_content::ordinary);
        if (reply.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, reply.error());
        }
    }
}

task<redis_value> redis_connection::exchange_control_command(std::span<const std::string_view> args, operation_deadline deadline, redis_request_content content) {
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

void serialize_resp_command(std::string& output, std::string_view first, std::span<const std::string_view> rest) {
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

void append_resp_command(std::string& output, std::string_view first, std::span<const std::string_view> rest) {
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

std::size_t resp_command_serialized_size(std::string_view first, std::span<const std::string_view> rest) {
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

redis_reply_builder::redis_reply_builder(std::optional<std::size_t> max_reply_bytes, std::size_t max_depth) noexcept :
    max_reply_bytes_(max_reply_bytes),
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
        reader_value->maxelements = static_cast<long long>(std::min(elements, static_cast<std::size_t>(REDIS_READER_MAX_ARRAY_ELEMENTS)));
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
            auto* parent = task_value->parent->parent == nullptr ? &static_cast<root_slot*>(task_value->parent->obj)->value_ : static_cast<redis_value*>(task_value->parent->obj);
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
        return task_value->type == REDIS_REPLY_ERROR ? redis_types_access::error_value(text) : redis_types_access::string_value(text);
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
    auto* node = builder.place(task_value, [&] {
        return redis_types_access::array_value(capacity);
    });
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
    return builder.place(task_value, [&] {
        return redis_types_access::null_value();
    });
}

void* redis_reply_builder::create_bool(const redisReadTask* task_value, int value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::integer_value(value == 0 ? 0 : 1);
    });
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

std::vector<std::optional<std::string>> parse_redis_optional_string_array(redis_value&& value) {
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

redis_set_result parse_redis_set_reply(redis_value&& value, const redis_set_options& options) {
    throw_if_redis_error(value);
    const auto returns_previous = redis_set_returns_previous(options.previous_value_);
    if (value.null()) {
        const bool applied = returns_previous && (!options.condition_.has_value() || *options.condition_ == redis_set_condition::if_absent);
        return redis_types_access::set_result(applied);
    }
    if (!returns_previous) {
        if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis set reply");
        }
        return redis_types_access::set_result(true);
    }
    const bool applied = !options.condition_.has_value() || *options.condition_ != redis_set_condition::if_absent;
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

std::vector<redis_key_value> parse_redis_key_value_array(redis_value&& value, std::string_view context_value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, context_value);
    }
    std::vector<redis_key_value> result;
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        result.push_back(redis_types_access::key_value(take_redis_string(std::move(values[i])), take_redis_string(std::move(values[i + 1]))));
    }
    return result;
}

std::vector<redis_scored_value> parse_redis_scored_array(redis_value&& value) {
    const auto values = redis_value_array(value);
    if (values.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis scored array reply");
    }
    std::vector<redis_scored_value> result;
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto score_text = redis_value_string(values[i + 1]);
        result.push_back(redis_types_access::scored_value(take_redis_string(std::move(values[i])), parse_redis_double(score_text, "invalid redis score")));
    }
    return result;
}

std::vector<std::string> parse_redis_string_array(redis_value&& value) {
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
[[nodiscard]] result_type parse_redis_scan_envelope(redis_value&& value, result_type result_value, std::string_view context_value, payload_parser_type parse_payload) {
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
    return parse_redis_scan_envelope(std::move(value), redis_types_access::scan_result(), "unexpected redis scan reply", [](redis_scan_result& result_value, redis_value&& payload) {
        redis_types_access::values(result_value) = parse_redis_string_array(std::move(payload));
    });
}

redis_hash_scan_result parse_redis_hash_scan_result(redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::hash_scan_result(), "unexpected redis hscan reply", [](redis_hash_scan_result& result_value, redis_value&& payload) {
        redis_types_access::entries(result_value) = parse_redis_key_value_array(std::move(payload), "unexpected redis hscan entry count");
    });
}

redis_z_scan_result parse_redis_z_scan_result(redis_value&& value) {
    return parse_redis_scan_envelope(std::move(value), redis_types_access::z_scan_result(), "unexpected redis zscan reply", [](redis_z_scan_result& result_value, redis_value&& payload) {
        redis_types_access::entries(result_value) = parse_redis_scored_array(std::move(payload));
    });
}

std::optional<redis_key_value> parse_redis_blocking_pop_reply(redis_value&& value) {
    throw_if_redis_error(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = redis_value_array(value);
    if (items.size() != 2) {
        throw redis_error(redis_error::code_type::protocol_error, "unexpected redis blocking pop reply");
    }
    return redis_types_access::key_value(take_redis_string(std::move(items[0])), take_redis_string(std::move(items[1])));
}

std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(redis_value&& value) {
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
            throw redis_error(redis_error::code_type::protocol_error, "unexpected redis xreadgroup stream reply");
        }
        auto stream = redis_types_access::stream_read_result(take_redis_string(std::move(stream_parts[0])));
        const auto entry_values = redis_value_array(stream_parts[1]);
        auto& entries = redis_types_access::entries(stream);
        entries.reserve(entry_values.size());
        for (auto& entry_value : entry_values) {
            const auto entry_parts = redis_value_array(entry_value);
            if (entry_parts.size() != 2) {
                throw redis_error(redis_error::code_type::protocol_error, "unexpected redis xreadgroup entry reply");
            }
            auto parsed_entry = redis_types_access::stream_entry(take_redis_string(std::move(entry_parts[0])));
            if (!entry_parts[1].null()) {
                redis_types_access::fields(parsed_entry) = parse_redis_key_value_array(std::move(entry_parts[1]), "unexpected redis xreadgroup field reply");
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
            return redis_types_access::pubsub_message(redis_value_string(items[1]), std::nullopt, redis_value_string(items[2]));
        }
        const bool pattern = kind == "psubscribe" || kind == "punsubscribe";
        const bool subscribed = kind == "subscribe" || kind == "psubscribe";
        if (!subscribed && kind != "unsubscribe" && kind != "punsubscribe") {
            throw redis_error(redis_error::code_type::protocol_error, malformed);
        }
        const auto name = optional_name(items[1]);
        const auto confirmation_kind = pattern ? (subscribed ? redis_pubsub_confirmation_kind::pattern_subscribed : redis_pubsub_confirmation_kind::pattern_unsubscribed) : (subscribed ? redis_pubsub_confirmation_kind::subscribed : redis_pubsub_confirmation_kind::unsubscribed);
        return redis_types_access::pubsub_confirmation(confirmation_kind, name, redis_value_integer(items[2]));
    }
    if (kind != "pmessage") {
        throw redis_error(redis_error::code_type::protocol_error, malformed);
    }
    return redis_types_access::pubsub_message(redis_value_string(items[2]), redis_value_string(items[1]), redis_value_string(items[3]));
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
    return items.size() == 2 && items[0].kind() == redis_value::kind_type::string && http_ascii_equals_ignore_case(items[0].string(), "pong") && items[1].kind() == redis_value::kind_type::string && items[1].string().empty();
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Classifies a raw pooled command before it is encoded. Connection-stateful
// commands are rejected; WAIT/WAITAOF are accepted only where a barrier is
// allowed. Returns whether the command needs the blocking pool; a blocking
// command throws when `allow_blocking` is false. An XREAD/XREADGROUP option
// this classifier does not know throws instead of being guessed non-blocking.
[[nodiscard]] bool validate_redis_pooled_command(std::span<const std::string_view> args, bool allow_blocking, bool allow_barrier = false);

// A wait the server may hold open indefinitely (a blocking command, a barrier,
// a subscription) must be bounded by its owner: a stop token or a finite
// operation timeout. Throws std::invalid_argument with `message` otherwise.
void require_bounded_redis_wait(const operation_options& options, const char* message);

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_redis_token(std::string_view value, std::string_view token) {
    return http_ascii_equals_ignore_case(value, token);
}

enum class redis_stream_read_kind : bool { read,
    read_group };

// Scans the options before STREAMS:
//   XREAD [COUNT n] [BLOCK ms] STREAMS ...
//   XREADGROUP GROUP g c [COUNT n] [BLOCK ms] [CLAIM min-idle-time] [NOACK] STREAMS ...
// https://redis.io/docs/latest/commands/xreadgroup/
// A token outside this grammar may change how the server waits, so it is
// rejected rather than routed as non-blocking.
[[nodiscard]] bool stream_read_options_block(std::span<const std::string_view> args, std::size_t first_option, redis_stream_read_kind kind) {
    const bool group = kind == redis_stream_read_kind::read_group;
    bool blocking = false;
    for (std::size_t i = first_option; i < args.size();) {
        const auto token = args[i];
        if (is_redis_token(token, "STREAMS")) {
            const auto remaining = args.size() - (i + 1);
            if (remaining < 2 || (remaining & 1U) != 0) {
                throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
            }
            return blocking;
        }
        if (is_redis_token(token, "BLOCK")) {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
            }
            blocking = true;
            i += 2;
        } else if (is_redis_token(token, "COUNT") || (group && is_redis_token(token, "CLAIM"))) {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
            }
            i += 2;
        } else if (group && is_redis_token(token, "NOACK")) {
            ++i;
        } else {
            throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
        }
    }
    throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
}

[[nodiscard]] bool xread_group_command_is_blocking(std::span<const std::string_view> args) {
    if (args.size() < 4 || !is_redis_token(args[1], "GROUP")) {
        throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
    }
    return stream_read_options_block(args, 4, redis_stream_read_kind::read_group);
}

}  // namespace

bool validate_redis_pooled_command(std::span<const std::string_view> args, bool allow_blocking, bool allow_barrier) {
    if (args.empty() || args.front().empty()) {
        throw std::invalid_argument("redis command requires a command name");
    }
    const auto command = args.front();
    // Redis barriers use the current connection's last write offset, not the
    // client's other pooled connections: https://redis.io/docs/latest/commands/wait/
    if (is_redis_token(command, "WAIT") || is_redis_token(command, "WAITAOF")) {
        if (!allow_barrier) {
            throw std::invalid_argument("redis WAIT and WAITAOF require a non-transaction pipeline containing the writes");
        }
        return true;
    }
    const auto reject_stateful = [] {
        throw std::invalid_argument("stateful redis commands are not allowed through pooled command()");
    };
    bool blocking = false;
    switch (command.size()) {
        case 4:
            if (is_redis_token(command, "AUTH") || is_redis_token(command, "EXEC") || is_redis_token(command, "QUIT") || is_redis_token(command, "SYNC")) {
                reject_stateful();
            }
            break;
        case 5:
            if (is_redis_token(command, "HELLO") || is_redis_token(command, "MULTI") || is_redis_token(command, "PSYNC") || is_redis_token(command, "RESET") || is_redis_token(command, "WATCH")) {
                reject_stateful();
            }
            if (is_redis_token(command, "BLPOP") || is_redis_token(command, "BRPOP")) {
                blocking = true;
            } else if (is_redis_token(command, "XREAD")) {
                blocking = stream_read_options_block(args, 1, redis_stream_read_kind::read);
            }
            break;
        case 6:
            if (is_redis_token(command, "ASKING") || is_redis_token(command, "CLIENT") || is_redis_token(command, "SELECT")) {
                reject_stateful();
            }
            blocking = is_redis_token(command, "BLMOVE") || is_redis_token(command, "BLMPOP") || is_redis_token(command, "BZMPOP");
            break;
        case 7:
            if (is_redis_token(command, "DISCARD") || is_redis_token(command, "MONITOR") || is_redis_token(command, "UNWATCH")) {
                reject_stateful();
            }
            break;
        case 8:
            if (is_redis_token(command, "READONLY") || is_redis_token(command, "REPLCONF")) {
                reject_stateful();
            }
            blocking = is_redis_token(command, "BZPOPMIN") || is_redis_token(command, "BZPOPMAX");
            break;
        case 9:
            if (is_redis_token(command, "READWRITE") || is_redis_token(command, "SUBSCRIBE")) {
                reject_stateful();
            }
            break;
        case 10:
            if (is_redis_token(command, "PSUBSCRIBE") || is_redis_token(command, "SSUBSCRIBE")) {
                reject_stateful();
            }
            if (is_redis_token(command, "BRPOPLPUSH")) {
                blocking = true;
            } else if (is_redis_token(command, "XREADGROUP")) {
                blocking = xread_group_command_is_blocking(args);
            }
            break;
        case 11:
            if (is_redis_token(command, "UNSUBSCRIBE")) {
                reject_stateful();
            }
            break;
        case 12:
            if (is_redis_token(command, "PUNSUBSCRIBE") || is_redis_token(command, "SUNSUBSCRIBE")) {
                reject_stateful();
            }
            break;
        default:
            return false;
    }
    if (blocking && !allow_blocking) {
        throw std::invalid_argument("blocking redis command is not allowed in a pipeline or transaction");
    }
    return blocking;
}

void require_bounded_redis_wait(const operation_options& options, const char* message) {
    if (!options.stop_token_.stoppable() && !options.timeout_.has_value()) {
        throw std::invalid_argument(message);
    }
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

template <typename duration_type>
void require_positive_redis_ttl(duration_type ttl, const char* message) {
    if (ttl.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

[[nodiscard]] detail::redis_owned_command make_redis_scan_command(std::string_view command, std::optional<std::string_view> key, const redis_scan_options& options) {
    std::array<char, http_max_unsigned_decimal_size> cursor_scratch{};
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::array<std::string_view, 7> args{};
    std::size_t count = 0;
    args[count++] = command;
    if (key.has_value()) {
        args[count++] = *key;
    }
    args[count++] = http_format_unsigned(cursor_scratch, options.cursor_.has_value() ? detail::redis_types_access::cursor_value(*options.cursor_) : 0);
    if (options.match_.has_value()) {
        args[count++] = "MATCH";
        args[count++] = *options.match_;
    }
    if (options.count_.has_value()) {
        args[count++] = "COUNT";
        args[count++] = http_format_unsigned(count_scratch, options.count_->value());
    }
    return detail::make_owned_redis_command(std::span<const std::string_view>(args.data(), count));
}

// The server-side wait starts when the command is written, so the reply
// deadline is armed at write time: slot acquisition and a lazy connect must not
// consume the block window. The grace covers the reply's network latency.
[[nodiscard]] std::optional<timeout_duration> redis_block_reply_timeout(const operation_options& options, redis_block_wait wait) {
    if (const auto duration = wait.duration(); duration.has_value()) {
        constexpr auto grace = std::chrono::seconds(1);
        if (*duration > std::chrono::milliseconds::max() - grace) {
            return std::chrono::milliseconds::max();
        }
        return *duration + grace;
    }
    detail::require_bounded_redis_wait(options, "infinite redis block requires a stop_token or finite operation timeout");
    return std::nullopt;
}

}  // namespace

redis_session::redis_session(detail::redis_client_runtime& runtime, ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics) noexcept :
    runtime_(&runtime),
    io_metrics_(io_metrics),
    registration_(operation_scope, this, &redis_session::expire_capability) {}

redis_session::redis_session(detail::redis_client_runtime& runtime, ::ruvia::operation_scope& operation_scope, operation_options options) noexcept :
    runtime_(&runtime),
    operation_options_(std::move(options)),
    registration_(operation_scope, this, &redis_session::expire_capability) {}

redis_session::redis_session(const redis_session& other) noexcept :
    runtime_(other.runtime_),
    io_metrics_(other.io_metrics_),
    operation_options_(other.operation_options_),
    registration_(other.registration_, this) {}

redis_session redis_session::with_options(operation_options options) const {
    registration_.require_active();
    redis_session configured(*this);
    configured.operation_options_ = override_operation_options(operation_options_, std::move(options));
    return configured;
}

const worker_ref& redis_session::worker() const& {
    registration_.require_active();
    return runtime_->worker();
}

detail::redis_pool& redis_session::pool() const noexcept {
    return runtime_->general();
}

detail::redis_pool& redis_session::blocking_pool() const noexcept {
    return runtime_->blocking();
}

task<redis_value> redis_session::execute(detail::redis_pool& pool_value, detail::redis_owned_command&& command, std::optional<timeout_duration> reply_timeout) const {
    return pool_value.execute_owned(std::move(command), operation_options_, reply_timeout, io_metrics_);
}

void redis_session::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<redis_session*>(target);
    handle.runtime_ = nullptr;
    handle.io_metrics_ = nullptr;
    handle.operation_options_ = {};
}

task<redis_value> redis_session::command(std::span<const std::string_view> args) const {
    registration_.require_active();
    const bool blocking = detail::validate_redis_pooled_command(args, true);
    if (blocking) {
        detail::require_bounded_redis_wait(operation_options_, "raw blocking redis command requires a stop_token or finite operation timeout");
    }
    return scoped(execute(blocking ? blocking_pool() : pool(), detail::make_owned_redis_command(args)));
}

task<redis_value> redis_session::command_owned(std::vector<std::string>&& args) const {
    registration_.require_active();
    return pool().execute_owned(std::vector<std::string>(args.begin(), args.end()), operation_options_, std::nullopt, io_metrics_);
}

task<redis_value> redis_session::script_owned(const detail::redis_orm_script& script, std::vector<std::string>&& args) const {
    registration_.require_active();
    return detail::execute_redis_orm_script(pool(), script, std::move(args), io_metrics_, operation_options_);
}

task<void> redis_session::ping() const {
    return mapped<void>({"PING"}, detail::parse_redis_pong);
}

task<std::string> redis_session::ping(std::string_view message) const {
    return mapped<std::string>({"PING", message}, detail::take_redis_string);
}

task<std::optional<std::string>> redis_session::get(std::string_view key) const {
    return mapped<std::optional<std::string>>({"GET", key}, detail::parse_redis_optional_string);
}

task<std::vector<std::optional<std::string>>> redis_session::mget(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::optional<std::string>>>(pool(), detail::redis_command_with_keys("MGET", keys), detail::parse_redis_optional_string_array);
}

task<redis_set_result> redis_session::set(std::string_view key, std::string_view value, redis_set_options options) const {
    registration_.require_active();
    auto command = detail::redis_set_args(key, value, options);
    return execute_mapped<redis_set_result>(pool(), std::move(command), [options = std::move(options)](redis_value&& reply) {
        return detail::parse_redis_set_reply(std::move(reply), options);
    });
}

task<void> redis_session::mset(std::span<const std::pair<std::string_view, std::string_view>> items) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::redis_mset_args(items), detail::parse_redis_ok);
}

task<std::optional<std::string>> redis_session::get_del(std::string_view key) const {
    return mapped<std::optional<std::string>>({"GETDEL", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::append(std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"APPEND", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::strlen(std::string_view key) const {
    return mapped<std::int64_t>({"STRLEN", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::incr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_integer_command("INCRBY", key, value), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::decr(std::string_view key) const {
    return mapped<std::int64_t>({"DECR", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::decr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_integer_command("DECRBY", key, value), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::del(std::string_view key) const {
    return mapped<std::int64_t>({"DEL", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::del(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_command_with_keys("DEL", keys), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::unlink(std::string_view key) const {
    return mapped<std::int64_t>({"UNLINK", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::unlink(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_command_with_keys("UNLINK", keys), detail::parse_redis_integer);
}

task<bool> redis_session::exists(std::string_view key) const {
    return mapped<bool>({"EXISTS", key}, detail::parse_redis_integer_bool);
}

task<bool> redis_session::touch(std::string_view key) const {
    return mapped<bool>({"TOUCH", key}, detail::parse_redis_integer_bool);
}

task<std::string> redis_session::type(std::string_view key) const {
    return mapped<std::string>({"TYPE", key}, detail::take_redis_string);
}

task<void> redis_session::rename(std::string_view key, std::string_view new_key) const {
    return mapped<void>({"RENAME", key, new_key}, detail::parse_redis_ok);
}

task<bool> redis_session::rename_nx(std::string_view key, std::string_view new_key) const {
    return mapped<bool>({"RENAMENX", key, new_key}, detail::parse_redis_integer_bool);
}

task<bool> redis_session::expire(std::string_view key, std::chrono::seconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis expire TTL must be greater than zero");
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("EXPIRE", key, ttl.count()), detail::parse_redis_integer_bool);
}

task<bool> redis_session::pexpire(std::string_view key, std::chrono::milliseconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis pexpire TTL must be greater than zero");
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("PEXPIRE", key, ttl.count()), detail::parse_redis_integer_bool);
}

task<bool> redis_session::expire_at(std::string_view key, std::chrono::system_clock::time_point expires_at) const {
    registration_.require_active();
    const auto expires_ms = std::chrono::ceil<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("PEXPIREAT", key, expires_ms), detail::parse_redis_integer_bool);
}

task<bool> redis_session::persist(std::string_view key) const {
    return mapped<bool>({"PERSIST", key}, detail::parse_redis_integer_bool);
}

task<redis_ttl> redis_session::ttl(std::string_view key) const {
    return mapped<redis_ttl>({"TTL", key}, [](redis_value&& reply) {
        return detail::parse_redis_ttl(std::move(reply), true);
    });
}

task<redis_ttl> redis_session::pttl(std::string_view key) const {
    return mapped<redis_ttl>({"PTTL", key}, [](redis_value&& reply) {
        return detail::parse_redis_ttl(std::move(reply), false);
    });
}

task<std::int64_t> redis_session::incr(std::string_view key) const {
    return mapped<std::int64_t>({"INCR", key}, detail::parse_redis_integer);
}

task<std::optional<std::string>> redis_session::hget(std::string_view key, std::string_view field) const {
    return mapped<std::optional<std::string>>({"HGET", key, field}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::hset(std::string_view key, std::string_view field, std::string_view value) const {
    return mapped<std::int64_t>({"HSET", key, field, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_hset_fields_args(key, fields_value), detail::parse_redis_integer);
}

task<std::vector<std::optional<std::string>>> redis_session::hmget(std::string_view key, std::span<const std::string_view> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::optional<std::string>>>(pool(), detail::redis_command_with_key_fields("HMGET", key, fields_value), detail::parse_redis_optional_string_array);
}

task<std::vector<redis_key_value>> redis_session::hget_all(std::string_view key) const {
    return mapped<std::vector<redis_key_value>>({"HGETALL", key}, [](redis_value&& reply) {
        return detail::parse_redis_key_value_array(std::move(reply), "unexpected redis hgetall reply");
    });
}

task<std::int64_t> redis_session::hdel(std::string_view key, std::string_view field) const {
    return mapped<std::int64_t>({"HDEL", key, field}, detail::parse_redis_integer);
}

task<bool> redis_session::hexists(std::string_view key, std::string_view field) const {
    return mapped<bool>({"HEXISTS", key, field}, detail::parse_redis_integer_bool);
}

task<std::int64_t> redis_session::hlen(std::string_view key) const {
    return mapped<std::int64_t>({"HLEN", key}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::hkeys(std::string_view key) const {
    return mapped<std::vector<std::string>>({"HKEYS", key}, detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::hvals(std::string_view key) const {
    return mapped<std::vector<std::string>>({"HVALS", key}, detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::hincr_by(std::string_view key, std::string_view field, std::int64_t value) const {
    std::array<char, detail::redis_int_scratch_size> value_scratch{};
    return mapped<std::int64_t>({"HINCRBY", key, field, detail::format_redis_int(value_scratch, value)}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::lpush(std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"LPUSH", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::rpush(std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"RPUSH", key, value}, detail::parse_redis_integer);
}

task<std::optional<std::string>> redis_session::lpop(std::string_view key) const {
    return mapped<std::optional<std::string>>({"LPOP", key}, detail::parse_redis_optional_string);
}

task<std::optional<std::string>> redis_session::rpop(std::string_view key) const {
    return mapped<std::optional<std::string>>({"RPOP", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::llen(std::string_view key) const {
    return mapped<std::int64_t>({"LLEN", key}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::lrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::make_redis_range_command("LRANGE", key, start, stop), detail::parse_redis_string_array);
}

task<std::optional<std::string>> redis_session::lindex(std::string_view key, std::int64_t index) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::string>>(pool(), detail::make_redis_integer_command("LINDEX", key, index), detail::parse_redis_optional_string);
}

task<void> redis_session::lset(std::string_view key, std::int64_t index, std::string_view value) const {
    std::array<char, detail::redis_int_scratch_size> index_scratch{};
    return mapped<void>({"LSET", key, detail::format_redis_int(index_scratch, index), value}, detail::parse_redis_ok);
}

task<void> redis_session::ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::make_redis_range_command("LTRIM", key, start, stop), detail::parse_redis_ok);
}

task<std::int64_t> redis_session::lrem(std::string_view key, std::int64_t count, std::string_view value) const {
    std::array<char, detail::redis_int_scratch_size> count_scratch{};
    return mapped<std::int64_t>({"LREM", key, detail::format_redis_int(count_scratch, count), value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::sadd(std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SADD", key, member}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::srem(std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SREM", key, member}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::smembers(std::string_view key) const {
    return mapped<std::vector<std::string>>({"SMEMBERS", key}, detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::scard(std::string_view key) const {
    return mapped<std::int64_t>({"SCARD", key}, detail::parse_redis_integer);
}

task<bool> redis_session::sismember(std::string_view key, std::string_view member) const {
    return mapped<bool>({"SISMEMBER", key, member}, detail::parse_redis_integer_bool);
}

task<std::optional<std::string>> redis_session::spop(std::string_view key) const {
    return mapped<std::optional<std::string>>({"SPOP", key}, detail::parse_redis_optional_string);
}

task<std::optional<std::string>> redis_session::srand_member(std::string_view key) const {
    return mapped<std::optional<std::string>>({"SRANDMEMBER", key}, detail::parse_redis_optional_string);
}

task<std::vector<std::string>> redis_session::sinter(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::redis_command_with_keys("SINTER", keys), detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::sunion(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::redis_command_with_keys("SUNION", keys), detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::sdiff(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::redis_command_with_keys("SDIFF", keys), detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::zadd(std::string_view key, double score, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_zadd_command(key, score, member), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::zrem(std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"ZREM", key, member}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::zrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::make_redis_range_command("ZRANGE", key, start, stop), detail::parse_redis_string_array);
}

task<std::vector<redis_scored_value>> redis_session::zrange_with_scores(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto command = detail::make_redis_range_command("ZRANGE", key, start, stop, true);
    return execute_mapped<std::vector<redis_scored_value>>(pool(), std::move(command), detail::parse_redis_scored_array);
}

task<std::optional<double>> redis_session::zscore(std::string_view key, std::string_view member) const {
    return mapped<std::optional<double>>({"ZSCORE", key, member}, [](redis_value&& reply) {
        return detail::parse_redis_optional_double(std::move(reply), "invalid redis zscore reply");
    });
}

task<std::int64_t> redis_session::zcard(std::string_view key) const {
    return mapped<std::int64_t>({"ZCARD", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::zcount(std::string_view key, double min_value, double max_value) const {
    registration_.require_active();
    std::array<char, detail::redis_score_scratch_size> min_scratch{};
    std::array<char, detail::redis_score_scratch_size> max_scratch{};
    const std::string_view args[]{
        "ZCOUNT",
        key,
        detail::format_redis_score(min_scratch, min_value),
        detail::format_redis_score(max_scratch, max_value),
    };
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(args), detail::parse_redis_integer);
}

task<redis_scan_result> redis_session::scan(redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(pool(), make_redis_scan_command("SCAN", std::nullopt, options), detail::parse_redis_scan_result);
}

task<redis_hash_scan_result> redis_session::hscan(std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_hash_scan_result>(pool(), make_redis_scan_command("HSCAN", key, options), detail::parse_redis_hash_scan_result);
}

task<redis_scan_result> redis_session::sscan(std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(pool(), make_redis_scan_command("SSCAN", key, options), detail::parse_redis_scan_result);
}

task<redis_z_scan_result> redis_session::zscan(std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_z_scan_result>(pool(), make_redis_scan_command("ZSCAN", key, options), detail::parse_redis_z_scan_result);
}

task<redis_value> redis_session::eval(std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVAL", script, keys, args)));
}

task<redis_value> redis_session::eval_sha(std::string_view sha1, std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVALSHA", sha1, keys, args)));
}

task<std::string> redis_session::script_load(std::string_view script) const {
    return mapped<std::string>({"SCRIPT", "LOAD", script}, detail::take_redis_string);
}

task<std::vector<bool>> redis_session::script_exists(std::span<const std::string_view> sha1s) const {
    registration_.require_active();
    return execute_mapped<std::vector<bool>>(pool(), detail::redis_command_with_key_fields("SCRIPT", "EXISTS", sha1s), detail::parse_redis_bool_array);
}

task<std::optional<redis_key_value>> redis_session::blpop(std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(), detail::redis_blocking_pop_args("BLPOP", keys, wait), detail::parse_redis_blocking_pop_reply, reply_timeout);
}

task<std::optional<redis_key_value>> redis_session::brpop(std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(), detail::redis_blocking_pop_args("BRPOP", keys, wait), detail::parse_redis_blocking_pop_reply, reply_timeout);
}

task<std::optional<redis_x_read_group_result>> redis_session::xread_group(std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams, redis_x_read_group_options options) const {
    registration_.require_active();
    auto args = detail::redis_x_read_group_args(group, consumer, streams, options);
    auto* selected_pool = &pool();
    std::optional<timeout_duration> reply_timeout;
    if (options.block_.has_value()) {
        selected_pool = &blocking_pool();
        reply_timeout = redis_block_reply_timeout(operation_options_, *options.block_);
    }
    return execute_mapped<std::optional<redis_x_read_group_result>>(*selected_pool, std::move(args), detail::parse_redis_x_read_group_reply, reply_timeout);
}

redis_pipeline redis_session::pipeline() const {
    registration_.require_active();
    return redis_pipeline(pool(), operation_options_, registration_.scope(), io_metrics_);
}

redis_transaction redis_session::transaction() const {
    registration_.require_active();
    return redis_transaction(pool(), operation_options_, registration_.scope(), io_metrics_);
}

task<std::int64_t> redis_session::publish(std::string_view channel, std::string_view payload) const {
    return mapped<std::int64_t>({"PUBLISH", channel, payload}, detail::parse_redis_integer);
}

redis_subscription redis_session::subscription() const {
    registration_.require_active();
    detail::require_bounded_redis_wait(operation_options_, "redis subscription requires a stop_token or finite operation timeout");
    return redis_subscription(runtime_->subscriptions(), registration_.scope(), operation_options_);
}

namespace detail {

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_pool& pool, operation_options options, operation_scope& scope, request_io_metrics* io_metrics) noexcept :
    pool_(&pool),
    options_(std::move(options)),
    commands_(),
    io_metrics_(io_metrics),
    registration_(scope, this, &redis_batch::expire_capability) {}

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_batch&& other) noexcept :
    pool_(std::exchange(other.pool_, nullptr)),
    options_(std::move(other.options_)),
    commands_(std::move(other.commands_)),
    io_metrics_(other.io_metrics_),
    registration_(std::move(other.registration_), this) {}

template <redis_batch_mode mode>
void redis_batch<mode>::require_ready() const {
    registration_.require_active();
    if (pool_ == nullptr) {
        throw std::logic_error("redis command batch has already been consumed or expired");
    }
}

template <redis_batch_mode mode>
redis_command_payload redis_batch<mode>::consume() {
    require_ready();
    auto& pool = *std::exchange(pool_, nullptr);
    return {pool, std::move(options_), std::move(commands_)};
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire() noexcept {
    pool_ = nullptr;
    options_ = {};
    io_metrics_ = nullptr;
    std::vector<redis_owned_command> empty;
    commands_.swap(empty);
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire_capability(void* target) noexcept {
    static_cast<redis_batch*>(target)->expire();
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::command(std::span<const std::string_view> args) {
    require_ready();
    const bool barrier = validate_redis_pooled_command(args, false, mode == redis_batch_mode::pipeline);
    if (barrier) {
        require_bounded_redis_wait(options_, "redis barrier pipeline requires a stop_token or finite operation timeout");
    }
    return append_owned(make_owned_redis_command(args));
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::append_owned(redis_owned_command command) {
    commands_.push_back(std::move(command));
    return *this;
}

template <redis_batch_mode mode>
task<std::vector<redis_value>> redis_batch<mode>::execute_owned(request_io_metrics* io_metrics, redis_command_payload payload_value) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    if constexpr (mode == redis_batch_mode::pipeline) {
        co_return co_await payload_value.pool_.get().execute_pipeline(std::move(payload_value.commands_), std::move(payload_value.options_));
    } else {
        auto replies = co_await payload_value.pool_.get().execute_pipeline(std::move(payload_value.commands_), std::move(payload_value.options_), true);
        if (replies.empty()) {
            throw redis_error(redis_error::code_type::protocol_error, "redis transaction returned no replies");
        }
        for (std::size_t i = 0; i < replies.size(); ++i) {
            throw_if_redis_transaction_reply_error(replies[i], i);
        }
        auto exec_reply = std::move(replies.back());
        if (exec_reply.kind() != redis_value::kind_type::array) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis transaction reply");
        }

        // Every reply node lives in `resource`, so the EXEC elements move without copying.
        co_return std::move(redis_types_access::elements(exec_reply));
    }
}

template <redis_batch_mode mode>
task<std::vector<redis_value>> redis_batch<mode>::exec() && {
    auto& scope = registration_.scope();
    auto payload_value = consume();
    return make_scoped_operation(scope, execute_owned(io_metrics_, std::move(payload_value)));
}

template class redis_batch<redis_batch_mode::pipeline>;
template class redis_batch<redis_batch_mode::transaction>;

}  // namespace detail

redis_session redis_session::bind_caller_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) && {
    registration_.require_active();
    redis_session bound(*runtime_, scope, io_metrics);
    bound.operation_options_ = override_operation_options(operation_options_, operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value});
    return bound;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// ARGV: mode, ttl policy, milliseconds, required count, required names,
// followed by mutation triples (set/delete, field, value). All validation
// precedes writes: Redis scripts are atomic but do not roll back runtime errors.
constexpr std::string_view write_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
local exists = kind ~= 'none'
if exists and kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if exists and redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
local required = tonumber(ARGV[4])
local start = 5 + required
local changes = {}
for i = start, #ARGV, 3 do changes[ARGV[i + 1]] = ARGV[i] end
for i = 5, 4 + required do
    local field = ARGV[i]
    if changes[field] == 'd' or
       (changes[field] ~= 's' and (ARGV[1] == 'insert' or (ARGV[1] ~= 'update' and not exists) or (exists and redis.call('HEXISTS', KEYS[1], field) == 0))) then
        return redis.error_reply('required entity field is missing: ' .. field)
    end
end
if ARGV[1] == 'insert' and exists then return 0 end
if ARGV[1] == 'update' and not exists then return 0 end
for i = start, #ARGV, 3 do
    if ARGV[i] == 's' then redis.call('HSET', KEYS[1], ARGV[i + 1], ARGV[i + 2])
    else redis.call('HDEL', KEYS[1], ARGV[i + 1]) end
end
redis.call('HSET', KEYS[1], '__ruvia_entity', '1')
if ARGV[2] == 'expire' then redis.call('PEXPIRE', KEYS[1], ARGV[3])
elseif ARGV[2] == 'persist' then redis.call('PERSIST', KEYS[1]) end
if exists then return 2 else return 1 end
)lua";

constexpr std::string_view delete_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return 0 end
if kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
return redis.call('DEL', KEYS[1])
)lua";

// Owns the digest text the script view borrows; built once per script.
class redis_orm_script_digest final {
public:
    explicit redis_orm_script_digest(std::string_view body) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        std::size_t digest_size = 0;
        if (EVP_Q_digest(nullptr, "SHA1", nullptr, body.data(), body.size(), digest.data(), &digest_size) != 1 || digest_size * 2 != sha1_.size()) {
            throw std::runtime_error("redis entity script digest could not be computed");
        }
        http_encode_lower_hex(sha1_.data(), std::span<const unsigned char>(digest.data(), digest_size));
        script_ = {body, std::string_view(sha1_.data(), sha1_.size())};
    }

    [[nodiscard]] const redis_orm_script& script() const noexcept {
        return script_;
    }

private:
    std::array<char, 40> sha1_{};
    redis_orm_script script_;
};

void append_redis_orm_script_call(redis_orm_arguments_type& args, const redis_orm_script& script) {
    args.emplace_back("EVALSHA");
    args.emplace_back(script.sha1_);
    args.emplace_back("1");
}

}  // namespace

const redis_orm_script& redis_orm_write_script() {
    static const redis_orm_script_digest digest(write_script);
    return digest.script();
}

const redis_orm_script& redis_orm_delete_script() {
    static const redis_orm_script_digest digest(delete_script);
    return digest.script();
}

redis_orm_arguments_type redis_orm_write_arguments(std::string_view storage_prefix, std::string_view id, std::string_view mode, redis_write_options options, std::size_t extra_arguments) {
    if (options.ttl_ && (options.persist_ || options.ttl_->count() <= 0 || options.ttl_->count() > 9007199254740991LL)) {
        throw std::invalid_argument("entity TTL must be positive, at most 2^53-1 milliseconds, and cannot combine with persist");
    }
    redis_orm_arguments_type args;
    args.reserve(7 + extra_arguments);
    append_redis_orm_script_call(args, redis_orm_write_script());
    args.push_back(redis_entity_key(storage_prefix, id));
    args.emplace_back(mode);
    args.emplace_back(options.ttl_ ? "expire" : (options.persist_ ? "persist" : "keep"));
    if (options.ttl_) {
        http_append_signed(args.emplace_back(), static_cast<std::int64_t>(options.ttl_->count()));
    } else {
        args.emplace_back("0");
    }
    return args;
}

redis_orm_arguments_type redis_orm_delete_arguments(std::string_view storage_prefix, std::string_view id) {
    redis_orm_arguments_type args;
    args.reserve(4);
    append_redis_orm_script_call(args, redis_orm_delete_script());
    args.push_back(redis_entity_key(storage_prefix, id));
    return args;
}

task<redis_value> execute_redis_orm_script(redis_pool& pool, const redis_orm_script& script, redis_orm_arguments_type args, request_io_metrics* io_metrics, operation_options options) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline(options.timeout_);
    {
        auto reply = co_await pool.execute_borrowed(redis_command_args_view{args}, options, deadline);
        if (reply.kind() != redis_value::kind_type::error || !reply.error().starts_with("NOSCRIPT")) {
            co_return reply;
        }
        // The error is consumed by this decision; release its storage before
        // the fallback waits for its connection and response.
    }
    // The server's script cache was flushed or never loaded this script:
    // EVAL runs the body and caches it under the same digest for later calls.
    args[0].assign("EVAL");
    args[1].assign(script.body_);
    co_return co_await pool.execute_owned(std::move(args), std::move(options), std::nullopt, nullptr, deadline);
}

redis_write_result redis_orm_exec_result(redis_value&& reply) {
    const auto value = redis_value_integer(reply);
    switch (value) {
        case 0:
            return redis_write_result(0);
        case 1:
        case 2:
            return redis_write_result(1);
        default:
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity write affected-row reply");
    }
}

redis_write_result redis_orm_delete_result(redis_value&& reply) {
    const auto value = redis_value_integer(reply);
    if (value == 0 || value == 1) {
        return redis_write_result(static_cast<std::uint64_t>(value));
    }
    throw redis_error(redis_error::code_type::protocol_error, "invalid entity delete affected-row reply");
}

void redis_orm_status_result(redis_value&& reply) {
    if (redis_value_string(reply) != "OK") {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity index status reply");
    }
}

std::uint64_t redis_orm_count(const redis_value& reply) {
    const auto count = redis_value_integer(reply);
    if (count < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "negative entity result count");
    }
    return static_cast<std::uint64_t>(count);
}

std::uint64_t redis_orm_count_result(redis_value&& reply) {
    const auto values = redis_value_array(reply);
    if (values.size() != 1) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity count reply");
    }
    return redis_orm_count(values.front());
}

bool redis_orm_exists_result(redis_value&& reply) {
    return redis_orm_count_result(std::move(reply)) != 0;
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] asio::ssl::context make_redis_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context_value(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context_value.native_handle(), config);
    }
    return context_value;
}

void validate_redis_registry(const worker_ref& worker_value, std::span<const redis_definition_type> redis) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("redis registry requires a valid worker");
    }
    validate_capability_aliases(redis, "redis alias must not be empty", "duplicate redis alias");
}

}  // namespace

redis_registry::redis_registry(asio::io_context& io_context, std::span<const redis_definition_type> redis, worker_ref worker_value) :
    runtimes_(),
    alias_index_() {
    validate_redis_registry(worker_value, redis);
    alias_index_.build(redis);
    runtimes_.reserve(redis.size());
    for (const auto& definition : redis) {
        runtimes_.push_back(std::make_unique<redis_client_runtime>(io_context, worker_value, redis_config_storage(definition.config_)));
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

redis_session redis_registry::get(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return runtimes_[*match]->session(operation_scope, io_metrics);
    }
    throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
}

redis_client_runtime::redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value, redis_config_storage config) :
    config_(std::move(config)),
    worker_(worker_value),
    tls_context_(make_redis_tls_context(config_.tls_)),
    general_(io_context, config_, tls_context_, config_.command_timeout_, config_.pool_size_per_worker_, worker_),
    // Blocking waits have their own deadlines; ordinary command timeouts
    // must not interrupt a finite long wait or an explicitly cancelled wait.
    blocking_(io_context, config_, tls_context_, std::nullopt, config_.blocking_pool_size_per_worker_, worker_),
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
redis_client_state::redis_client_state(event_loop loop, const redis_config& config) :
    loop_(client_lifecycle<redis_client_state>::require_loop(std::move(loop))),
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

redis_client::redis_client(event_loop loop, const redis_config& config) :
    state_(std::make_shared<detail::redis_client_state>(std::move(loop), config)) {
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

redis_error::redis_error(code_type code, std::string_view message) :
    std::runtime_error(std::string(message)),
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

[[nodiscard]] redis_owned_command encode_redis_pair_command(std::string_view command, std::optional<std::string_view> key, std::span<const std::pair<std::string_view, std::string_view>> pairs, const char* empty_message) {
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

redis_owned_command make_owned_redis_command(std::span<const std::string_view> args) {
    redis_owned_command command{std::string()};
    append_resp_command(command.wire_, args);
    return command;
}

redis_owned_command make_owned_redis_command(std::string_view first, std::span<const std::string_view> rest) {
    redis_owned_command command{std::string()};
    append_resp_command(command.wire_, first, rest);
    return command;
}

redis_owned_command make_owned_redis_command(std::initializer_list<std::string_view> args) {
    return make_owned_redis_command(std::span<const std::string_view>(args.begin(), args.size()));
}

redis_owned_command make_redis_integer_command(std::string_view command, std::string_view key, std::int64_t value) {
    std::array<char, redis_int_scratch_size> value_scratch{};
    return make_owned_redis_command({command, key, format_redis_int(value_scratch, value)});
}

redis_owned_command make_redis_range_command(std::string_view command, std::string_view key, std::int64_t start, std::int64_t stop, bool with_scores) {
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

redis_owned_command make_redis_zadd_command(std::string_view key, double score, std::string_view member) {
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

redis_owned_command redis_command_with_keys(std::string_view command, std::span<const std::string_view> keys) {
    if (keys.empty()) {
        throw std::invalid_argument("redis command requires at least one key");
    }
    return make_owned_redis_command(command, keys);
}

redis_owned_command redis_mset_args(std::span<const std::pair<std::string_view, std::string_view>> items) {
    return encode_redis_pair_command("MSET", std::nullopt, items, "redis mset requires at least one item");
}

redis_owned_command redis_set_args(std::string_view key, std::string_view value, const redis_set_options& options) {
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
    const std::string_view duration_text = duration != nullptr ? format_redis_int(duration_scratch, static_cast<std::int64_t>(duration->count())) : std::string_view();
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

redis_owned_command redis_hset_fields_args(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields_value) {
    return encode_redis_pair_command("HSET", key, fields_value, "redis hset requires at least one field");
}

redis_owned_command redis_command_with_key_fields(std::string_view command, std::string_view key, std::span<const std::string_view> fields_value) {
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

redis_owned_command redis_eval_args(std::string_view command, std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> argv) {
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

redis_owned_command redis_blocking_pop_args(std::string_view command, std::span<const std::string_view> keys, redis_block_wait wait) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    // Seconds with millisecond precision: digits, '.', three fraction digits.
    std::array<char, redis_int_scratch_size + 4> timeout_scratch{};
    std::string_view timeout_text = "0";
    if (const auto duration = wait.duration(); duration.has_value()) {
        const auto milliseconds = duration->count();
        const auto seconds_text = format_redis_int(std::span<char, redis_int_scratch_size>(timeout_scratch.data(), redis_int_scratch_size), milliseconds / 1000);
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

redis_owned_command redis_x_read_group_args(std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams, const redis_x_read_group_options& options) {
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

redis_subscription_connection::redis_subscription_connection(redis_subscription_set& owner, operation_options options) :
    owner_(owner),
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
        read_stop.emplace(read_stops_, owner_.worker_, operation_options{.stop_token_ = operation.stop_token_}, this, &redis_subscription_connection::cancel_read, "redis subscription read");
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

redis_subscription_set::redis_subscription_set(asio::io_context& io_context, asio::ssl::context& tls_context, const redis_config_storage& config, const worker_ref& worker_value) :
    io_context_(io_context),
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

redis_subscription::redis_subscription(detail::redis_subscription_set& subscriptions, operation_scope& scope, operation_options options) :
    connection_(subscriptions.open(std::move(options))),
    registration_(scope, this, &redis_subscription::expire_capability) {}

redis_subscription::~redis_subscription() = default;

void redis_subscription::expire_capability(void* target) noexcept {
    static_cast<redis_subscription*>(target)->connection_->abort(detail::client_abort_reason::closing);
}

task<void> redis_subscription::send(std::string_view command, std::span<const std::string_view> names, bool require_names) {
    registration_.require_active();
    if (require_names && names.empty()) {
        throw std::invalid_argument("redis subscription requires at least one channel or pattern");
    }
    return make_scoped_operation(registration_.scope(), connection_->send(detail::make_owned_redis_command(command, names)));
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
    throw std::invalid_argument("numeric search values must be finite and within the exact integer range of Redis Search");
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
        if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_numeric_column_kind(redis_column_kind kind) noexcept {
    return kind == redis_column_kind::signed_value || kind == redis_column_kind::unsigned_value || kind == redis_column_kind::float32 || kind == redis_column_kind::float64;
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
[[nodiscard]] std::string shadow_field(std::string_view shadow, std::string_view field) {
    std::string result_value;
    result_value.reserve(shadow.size() + field.size());
    result_value.append(shadow);
    result_value.append(field);
    return result_value;
}

[[nodiscard]] const redis_column_info& find_column_info(std::span<const redis_column_info> columns, std::string_view name) {
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

[[nodiscard]] std::string_view format_canonical_numeric(redis_numeric_bound_buffer& buffer, const redis_literal& value, redis_column_kind column_kind, bool exclusive = false) {
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
            number = column_kind == redis_column_kind::float32 ? encode_redis_number(real, scratch) : encode_redis_number(static_cast<double>(real), scratch);
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
[[nodiscard]] std::string_view raw_tag_value(const redis_literal& value, redis_column_kind column_kind) {
    if (value.kind() == redis_literal_kind::string && column_kind == redis_column_kind::text) {
        return value.text();
    }
    if (value.kind() == redis_literal_kind::boolean && column_kind == redis_column_kind::bool_value) {
        return value.boolean_value() ? std::string_view("1") : std::string_view("0");
    }
    throw std::invalid_argument("Redis TAG predicate value type does not match column");
}

// Primary keys are text or integer columns (is_redis_entity_primary_key_scalar).
[[nodiscard]] std::string_view canonical_primary_key(const redis_literal& value, redis_column_kind column_kind, std::span<char, redis_scalar_scratch_size> scratch) {
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
            const double number = value.kind() == redis_literal_kind::float32 ? static_cast<double>(value.float32_value()) : value.real_value();
            if (!std::isfinite(number) || std::trunc(number) != number || std::fabs(number) > redis_max_exact_integer || (column_kind == redis_column_kind::unsigned_value && number < 0.0)) {
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

[[nodiscard]] redis_column_comparison column_comparison(const redis_mapping& mapping, std::span<const redis_predicate_node> nodes, const redis_predicate_node& node) {
    if (node.operands_.size() < 2U || nodes[node.operands_[0]].kind_ != node_kind::field) {
        throw std::logic_error("invalid Redis predicate expression");
    }
    const auto& field = nodes[node.operands_[0]];
    validate_entity_prefix(mapping.entity_prefix_, field.entity_prefix_);
    return {find_column_info(mapping.columns_, field.field_), nodes[node.operands_[1]]};
}

// `@shadow_field:{value}`
void append_shadow_tag_term(std::string& output, std::string_view shadow, std::string_view field, std::string_view value) {
    output.reserve(output.size() + 4U + shadow.size() + field.size() + value.size());
    output.push_back('@');
    output.append(shadow);
    output.append(field);
    output.append(":{");
    output.append(value);
    output.push_back('}');
}

// `@field:{x<hex>}`
void append_encoded_tag_term(std::string& output, std::string_view field, std::string_view raw_value) {
    output.reserve(output.size() + 5U + field.size() + raw_value.size() * 2U);
    output.push_back('@');
    output.append(field);
    output.append(":{x");
    http_append_lower_hex(output, raw_value);
    output.push_back('}');
}

// `@field:[low high]`; a bound prefixed with '(' is exclusive.
void append_numeric_range(std::string& output, std::string_view field, std::string_view low, std::string_view high) {
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
void append_null_comparison(std::string& output, std::string_view field, redis_binary_operator operation, const char* unsupported) {
    if (operation == redis_binary_operator::equal) {
        output.push_back('-');
        append_presence(output, field);
    } else if (operation == redis_binary_operator::not_equal) {
        append_presence(output, field);
    } else {
        throw std::invalid_argument(unsupported);
    }
}

void append_tag_comparison(std::string& output, std::string_view field, const redis_literal& value, redis_column_kind column_kind, redis_binary_operator operation) {
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
            output.reserve(output.size() + 14U + present_shadow.size() + field.size() * 2U + raw.size() * 2U);
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

void append_numeric_comparison(std::string& output, std::string_view field, const redis_literal& value, redis_column_kind column_kind, redis_binary_operator operation) {
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
    redis_query_compiler(const redis_mapping& mapping) noexcept :
        mapping_(mapping) {}

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

    void append_expression(std::string& output, nodes_type nodes, const redis_predicate_node& node, std::size_t depth) const {
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
        if (comparison.column_.primary_key_ && comparison.column_.kind_ == redis_column_kind::text && value.kind() == redis_literal_kind::string && value.text().empty()) {
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
        if (value.kind() == redis_literal_kind::null && (node.binary_ == redis_binary_operator::equal || node.binary_ == redis_binary_operator::not_equal)) {
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
            if (comparison.column_.primary_key_ && comparison.column_.kind_ == redis_column_kind::text && value.kind() == redis_literal_kind::string && value.text().empty()) {
                throw std::invalid_argument("Redis primary key cannot be empty");
            }
            if (kind == redis_index_kind::tag) {
                append_encoded_tag_term(output, field, raw_tag_value(value, comparison.column_.kind_));
            } else {
                redis_numeric_bound_buffer number_buffer;
                const auto number = format_canonical_numeric(number_buffer, value, comparison.column_.kind_);
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
        if (index_kind(mapping_, field) != redis_index_kind::numeric || lower.kind() == redis_literal_kind::null || upper.kind() == redis_literal_kind::null) {
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
    const auto parsed_value = std::from_chars(value.data(), value.data() + value.size(), number, std::chars_format::general);
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

redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns, std::string_view entity_prefix, const redis_repository_config& config) {
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
    redis_mapping result_value{columns, std::string(entity_prefix), std::string(prefix), redis_entity_storage_prefix(prefix), std::vector<redis_index_definition>{}, std::vector<redis_index_kind>(columns.size(), redis_index_kind::none)};
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

redis_search_mapping::redis_search_mapping(const redis_search_mapping& other) :
    mapping_{other.mapping_.columns_, std::string(other.mapping_.entity_prefix_), std::string(other.mapping_.prefix_), std::string(other.mapping_.storage_prefix_), std::vector<detail::redis_index_definition>{}, std::vector<redis_index_kind>(other.mapping_.column_index_kinds_)} {
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

redis_compiled_command redis_search_mapping::compile_find(const redis_find_options& options, redis_search_mode mode) const {
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

std::optional<std::string_view> redis_primary_key(const redis_mapping& mapping, const redis_predicate& predicate, std::span<char, redis_scalar_scratch_size> scratch) {
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

void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field, std::optional<std::string_view> value, bool numeric_index, bool tag_shadow_value) {
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
