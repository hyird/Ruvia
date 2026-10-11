#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "client/client_transport.h"
#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"

namespace ruvia::detail {

void redis_pool::close(std::size_t slot) noexcept {
    auto& connection = connections_[slot];
    std::error_code ignored;
    connection.resolver_.cancel();
    connection.socket_.cancel(ignored);
    connection.socket_.close(ignored);
    connection.connected_ = false;
    (void)slots_.clear_deadline(slot);
    connection.reader_.reset();
    connection.reply_bytes_ = 0;
}

void redis_pool::configure_socket(connection_type& connection) noexcept {
    ruvia::apply_tcp_socket_policies(connection.socket_, config_.tcp_.no_delay_, config_.tcp_.keep_alive_);
}

void redis_pool::ensure_reader(connection_type& connection) {
    if (connection.reply_builder_ == nullptr) {
        connection.reply_builder_ =
            make_pmr_object<redis_reply_builder>(resource_, config_.max_reply_bytes_, config_.max_array_depth_);
    }
    if (connection.reader_ == nullptr) {
        connection.reader_.reset(connection.reply_builder_->create_reader());
    }
    connection.reply_bytes_ = 0;
}

task<void> redis_pool::async_socket_write(std::size_t slot, const ruvia::operation_timeout& timeout) {
    auto& connection = connections_[slot];
    if (!slots_.arm_deadline(slot, timeout.remaining(), deadline_kind_type::socket)) {
        throw redis_error(redis_error::code_type::timeout, "redis command timed out");
    }
    const auto write_completion = co_await ruvia::async_asio([&connection](auto handler) mutable {
        if (connection.tls_stream_) {
            asio::async_write(*connection.tls_stream_, asio::buffer(connection.write_buffer_), std::move(handler));
        } else {
            asio::async_write(connection.socket_, asio::buffer(connection.write_buffer_), std::move(handler));
        }
    });
    slots_.throw_if_aborted(slot);
    const auto ec = write_completion.error_code();
    if (slots_.clear_deadline(slot) || timeout.expired() || ec == asio::error::timed_out) {
        throw redis_error(redis_error::code_type::timeout, "redis command timed out");
    }
    if (ec) {
        throw redis_error(redis_error::code_type::io_error, ec.message());
    }
}

task<asio_completion<std::size_t>> redis_pool::async_socket_read_some(
    std::size_t slot, std::span<char> buffer, const ruvia::operation_timeout& timeout) {
    auto& connection = connections_[slot];
    if (!slots_.arm_deadline(slot, timeout.remaining(), deadline_kind_type::socket)) {
        co_return asio_completion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    auto result_value = co_await ruvia::async_asio<std::size_t>([&connection, buffer](auto handler) mutable {
        if (connection.tls_stream_) {
            connection.tls_stream_->async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(handler));
        } else {
            connection.socket_.async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(handler));
        }
    });
    slots_.throw_if_aborted(slot);
    if (slots_.clear_deadline(slot) || timeout.expired()) {
        co_return asio_completion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    co_return result_value;
}

task<redis_value> redis_pool::read_reply(
    std::size_t slot, const ruvia::operation_timeout& timeout, std::pmr::memory_resource* resource) {
    auto& connection = connections_[slot];
    ensure_reader(connection);
    connection.reply_builder_->begin(pmr_resource_or_default(resource));
    for (;;) {
        const auto unread_before = connection.reader_->len - connection.reader_->pos;
        void* raw_reply = nullptr;
        if (redisReaderGetReply(connection.reader_.get(), &raw_reply) != REDIS_OK) {
            connection.reply_builder_->throw_if_rejected();
            throw redis_error(redis_error::code_type::protocol_error, hiredis_reader_error(*connection.reader_));
        }
        std::optional<redis_value> reply;
        if (raw_reply != nullptr) {
            reply.emplace(redis_reply_builder::take(raw_reply));
        }
        const auto unread_after = connection.reader_->len - connection.reader_->pos;
        const auto consumed = unread_before - unread_after;
        // One socket read can contain multiple replies. Only the bytes parsed for
        // this reply count against its limit; if incomplete, any unread bytes
        // still belong to it and must also stay within the limit.
        if (config_.max_reply_bytes_.has_value()) {
            const auto limit = *config_.max_reply_bytes_;
            if (consumed > limit || connection.reply_bytes_ > limit - consumed ||
                (!reply && unread_after >= limit - (connection.reply_bytes_ + consumed))) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "redis reply exceeds configured limit");
            }
        }
        connection.reply_bytes_ += consumed;
        if (reply) {
            connection.reply_bytes_ = 0;
            co_return std::move(*reply);
        }

        auto read_capacity = connection.read_buffer_.size();
        if (config_.max_reply_bytes_.has_value()) {
            const auto unread = connection.reader_->len - connection.reader_->pos;
            read_capacity = std::min(read_capacity, *config_.max_reply_bytes_ - connection.reply_bytes_ - unread);
        }
        auto read_completion = co_await async_socket_read_some(slot,
            std::span<char>(connection.read_buffer_.data(), read_capacity), timeout);
        const auto read_ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        if (read_ec) {
            if (read_ec == asio::error::timed_out) {
                throw redis_error(redis_error::code_type::timeout, "redis command timed out");
            }
            throw redis_error(redis_error::code_type::io_error, read_ec.message());
        }
        if (redisReaderFeed(connection.reader_.get(), connection.read_buffer_.data(), bytes_read) !=
            REDIS_OK) {
            throw redis_error(
                redis_error::code_type::protocol_error, hiredis_reader_error(*connection.reader_));
        }
    }
}

task<void> redis_pool::connect(std::size_t slot, const ruvia::operation_timeout* operation_timeout_value) {
    auto& connection = connections_[slot];
    if (connection.connected_) {
        co_return;
    }

    std::array<char, 8> port_buffer;
    auto [port_end, port_ec] =
        std::to_chars(port_buffer.data(), port_buffer.data() + port_buffer.size(), config_.port_);
    if (port_ec != std::errc{}) {
        throw redis_error(redis_error::code_type::connect_failed, "invalid redis port");
    }
    const auto port =
        std::string_view(port_buffer.data(), static_cast<std::size_t>(port_end - port_buffer.data()));

    const auto deadline_value = operation_timeout_value != nullptr
                                    ? operation_timeout_value->constrained_by(config_.connect_timeout_)
                                    : ruvia::operation_timeout(config_.connect_timeout_);
    if (!slots_.arm_deadline(slot, deadline_value.remaining(), deadline_kind_type::resolve)) {
        throw redis_error(redis_error::code_type::timeout, "redis resolve timed out");
    }
    auto resolve_completion = co_await ruvia::async_asio<asio::ip::tcp::resolver::results_type>(
        [this, &connection, port](auto handler) mutable {
            connection.resolver_.async_resolve(config_.host_, port, std::move(handler));
        });
    slots_.throw_if_aborted(slot);
    const auto resolve_ec = resolve_completion.error_code();
    auto endpoints = std::move(resolve_completion).take_result();
    if (slots_.clear_deadline(slot) || deadline_value.expired()) {
        throw redis_error(redis_error::code_type::timeout, "redis resolve timed out");
    }
    if (resolve_ec) {
        throw redis_error(redis_error::code_type::connect_failed, resolve_ec.message());
    }

    if (!slots_.arm_deadline(slot, deadline_value.remaining(), deadline_kind_type::socket)) {
        throw redis_error(redis_error::code_type::timeout, "redis connect timed out");
    }
    const auto connect_completion =
        co_await ruvia::async_asio([&connection, &endpoints](auto handler) mutable {
            asio::async_connect(connection.socket_, endpoints, std::move(handler));
        });
    slots_.throw_if_aborted(slot);
    const auto connect_ec = connect_completion.error_code();
    if (slots_.clear_deadline(slot) || deadline_value.expired()) {
        throw redis_error(redis_error::code_type::timeout, "redis connect timed out");
    }
    if (connect_ec) {
        throw redis_error(redis_error::code_type::connect_failed, connect_ec.message());
    }
    configure_socket(connection);
    connection.tls_stream_.reset();
    if (tls_context_ != nullptr) {
        connection.tls_stream_ = make_pmr_object<connection_type::tls_stream_type>(resource_, connection.socket_, *tls_context_);
        const auto prepared = prepare_client_tls_session(
            *connection.tls_stream_->native_handle(), config_.tls_.tls_host(config_.host_), config_.tls_.mode_);
        if (prepared != client_tls_setup_error::none) {
            std::string message("redis TLS setup failed: ");
            message.append(client_tls_setup_error_message(prepared));
            throw redis_error(redis_error::code_type::connect_failed, message);
        }
        if (!slots_.arm_deadline(slot, deadline_value.remaining(), deadline_kind_type::socket)) {
            throw redis_error(redis_error::code_type::timeout, "redis TLS handshake timed out");
        }
        const auto handshake = co_await ruvia::async_asio([&connection](auto handler) {
            connection.tls_stream_->async_handshake(asio::ssl::stream_base::client, std::move(handler));
        });
        slots_.throw_if_aborted(slot);
        if (slots_.clear_deadline(slot) || deadline_value.expired()) {
            throw redis_error(redis_error::code_type::timeout, "redis TLS handshake timed out");
        }
        if (handshake.error_code()) {
            throw redis_error(redis_error::code_type::connect_failed, "redis TLS handshake failed: " + handshake.error_code().message());
        }
    }
    ensure_reader(connection);
    connection.connected_ = true;

    try {
        co_await authenticate(slot, deadline_value);
    } catch (...) {
        close(slot);
        throw;
    }
}

task<void> redis_pool::authenticate(std::size_t slot, const ruvia::operation_timeout& connect_timeout) {
    auto& connection = connections_[slot];
    auto run_control = [this, slot, &connection, &connect_timeout](
                           std::span<const std::string_view> args) -> task<redis_value> {
        connection.write_buffer_.clear();
        connection.write_buffer_.reserve(resp_command_serialized_size(args));
        append_resp_command(connection.write_buffer_, args);
        const auto deadline_value = connect_timeout.constrained_by(command_timeout_);
        co_await async_socket_write(slot, deadline_value);

        co_return co_await read_reply(slot, deadline_value, resource_);
    };

    if (!config_.password_.empty()) {
        redis_value reply(resource_);
        if (!config_.username_.empty()) {
            std::array<std::string_view, 3> args{
                "AUTH", std::string_view(config_.username_), std::string_view(config_.password_)};
            reply = co_await run_control(args);
        } else {
            std::array<std::string_view, 2> args{"AUTH", std::string_view(config_.password_)};
            reply = co_await run_control(args);
        }
        if (reply.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::auth_failed, reply.error());
        }
    }

    if (config_.database_ != 0) {
        std::pmr::string db(resource_);
        detail::append_redis_number(db, static_cast<std::uint64_t>(config_.database_));
        std::array<std::string_view, 2> args{"SELECT", std::string_view(db)};
        auto reply = co_await run_control(args);
        if (reply.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, reply.error());
        }
    }
}

}  // namespace ruvia::detail
