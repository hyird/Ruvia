#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"

namespace ruvia::detail {

struct redis_reader_budget final {
    enum class rejection_type : std::uint8_t { none,
        elements,
        depth };

    redis_reader_budget(redisReader& reader_value, std::optional<std::size_t> max_reply_bytes,
        std::size_t max_depth) noexcept
        : functions_(*reader_value.fn),
          original_create_array_(functions_.createArray),
          maximum_elements_(max_reply_bytes.has_value()
                                ? *max_reply_bytes / 3
                                : std::numeric_limits<std::size_t>::max()),
          remaining_elements_(maximum_elements_),
          max_depth_(max_depth) {
        functions_.createArray = &redis_reader_budget::create_array;
        bind(reader_value);
    }

    void bind(redisReader& reader_value) noexcept {
        reader_value.fn = &functions_;
        reader_value.privdata = this;
    }

    void reset() noexcept {
        remaining_elements_ = maximum_elements_;
        rejection_ = rejection_type::none;
    }

    static void* create_array(const redisReadTask* task_value, std::size_t elements) noexcept {
        auto& budget = *static_cast<redis_reader_budget*>(task_value->privdata);
        if (budget.max_depth_ != 0) {
            std::size_t depth = 0;
            for (auto* parent_value = task_value; parent_value != nullptr; parent_value = parent_value->parent) {
                if (++depth > budget.max_depth_) {
                    budget.rejection_ = rejection_type::depth;
                    return nullptr;
                }
            }
        }
        if (elements > budget.remaining_elements_) {
            budget.rejection_ = rejection_type::elements;
            return nullptr;
        }
        auto* reply = budget.original_create_array_(task_value, elements);
        if (reply != nullptr) {
            budget.remaining_elements_ -= elements;
        }
        return reply;
    }

    redisReplyObjectFunctions functions_;
    void* (*original_create_array_)(const redisReadTask*, std::size_t);
    std::size_t maximum_elements_;
    std::size_t remaining_elements_;
    std::size_t max_depth_;
    rejection_type rejection_{rejection_type::none};
};

void redis_reader_budget_deleter::operator()(redis_reader_budget* budget) const noexcept {
    destroy_pmr_object(budget, resource_);
}

void redis_pool::close(connection_type& connection) noexcept {
    std::error_code ignored;
    connection.resolver_.cancel();
    connection.socket_.cancel(ignored);
    connection.socket_.close(ignored);
    connection.connected_ = false;
    (void)clear_deadline(connection);
    connection.reader_.reset();
    connection.reply_bytes_ = 0;
}

void redis_pool::configure_socket(connection_type& connection) noexcept {
    ruvia::apply_tcp_socket_policies(connection.socket_, config_.tcp_no_delay_, config_.tcp_keep_alive_);
}

void redis_pool::ensure_reader(connection_type& connection) {
    if (connection.reader_ == nullptr) {
        connection.reader_.reset(redisReaderCreate());
        if (connection.reader_ != nullptr && connection.reader_budget_ != nullptr) {
            connection.reader_budget_->bind(*connection.reader_);
        }
    }
    if (connection.reader_ == nullptr) {
        throw redis_error(redis_error::code_type::protocol_error, "failed to create redis reader");
    }
    if (connection.reader_budget_ == nullptr) {
        connection.reader_budget_.get_deleter().resource_ = resource_;
        connection.reader_budget_.reset(construct_pmr_object<redis_reader_budget>(
            resource_, *connection.reader_, config_.max_reply_bytes_, config_.max_array_depth_));
    }
    if (config_.max_reply_bytes_.has_value()) {
        // Even a short array header can otherwise reserve billions of reply
        // pointers before the byte budget can inspect the complete reply.
        // Every element requires at least three RESP bytes (e.g. "+\r\n").
        const auto elements = std::max<std::size_t>(1, *config_.max_reply_bytes_ / 3);
        connection.reader_->maxelements = static_cast<long long>(
            std::min(elements, static_cast<std::size_t>(REDIS_READER_MAX_ARRAY_ELEMENTS)));
    }
    connection.reader_budget_->reset();
    connection.reply_bytes_ = 0;
}

bool redis_pool::arm_deadline(
    connection_type& connection, const ruvia::operation_timeout& timeout, connection_type::deadline_kind_type kind) {
    connection.deadline_timer_->cancel();
    const auto remaining = timeout.remaining();
    if (!remaining.has_value()) {
        connection.deadline_.reset();
        return true;
    }
    if (remaining->count() == 0) {
        connection.deadline_.reset();
        return false;
    }
    const auto deadline_value = worker_timer_deadline_after(*remaining);
    connection.deadline_.arm(deadline_value, kind);
    (worker_).schedule_timer(*connection.deadline_timer_, deadline_value, [&connection](worker_timer_outcome outcome) noexcept {
        if (outcome != worker_timer_outcome::expired) {
            return;
        }
        const auto expired_kind = connection.deadline_.expire(std::chrono::steady_clock::now());
        if (!expired_kind.has_value()) {
            return;
        }
        std::error_code ignored;
        if (*expired_kind == connection_type::deadline_kind_type::resolve) {
            connection.resolver_.cancel();
        } else {
            connection.socket_.cancel(ignored);
        }
    });
    return true;
}

bool redis_pool::clear_deadline(connection_type& connection) noexcept {
    connection.deadline_timer_->cancel();
    return connection.deadline_.clear();
}

task<void> redis_pool::async_socket_write(
    connection_type& connection, const ruvia::operation_timeout& timeout) {
    if (!arm_deadline(connection, timeout, connection_type::deadline_kind_type::socket)) {
        throw redis_error(redis_error::code_type::timeout, "redis command timed out");
    }
    const auto write_completion = co_await ruvia::async_asio([&connection](auto handler) mutable {
        if (connection.tls_stream_) {
            asio::async_write(*connection.tls_stream_, asio::buffer(connection.write_buffer_), std::move(handler));
        } else {
            asio::async_write(connection.socket_, asio::buffer(connection.write_buffer_), std::move(handler));
        }
    });
    throw_if_aborted(connection);
    const auto ec = write_completion.error_code();
    if (clear_deadline(connection) || timeout.expired() || ec == asio::error::timed_out) {
        throw redis_error(redis_error::code_type::timeout, "redis command timed out");
    }
    if (ec) {
        throw redis_error(redis_error::code_type::io_error, ec.message());
    }
}

task<asio_completion<std::size_t>> redis_pool::async_socket_read_some(
    connection_type& connection, std::span<char> buffer, const ruvia::operation_timeout& timeout) {
    if (!arm_deadline(connection, timeout, connection_type::deadline_kind_type::socket)) {
        co_return asio_completion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    auto result_value = co_await ruvia::async_asio<std::size_t>([&connection, buffer](auto handler) mutable {
        if (connection.tls_stream_) {
            connection.tls_stream_->async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(handler));
        } else {
            connection.socket_.async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(handler));
        }
    });
    throw_if_aborted(connection);
    if (clear_deadline(connection) || timeout.expired()) {
        co_return asio_completion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    co_return result_value;
}

task<redis_value> redis_pool::read_reply(
    connection_type& connection, const ruvia::operation_timeout& timeout, std::pmr::memory_resource* resource) {
    ensure_reader(connection);
    for (;;) {
        const auto unread_before = connection.reader_->len - connection.reader_->pos;
        void* raw_reply = nullptr;
        const auto reader_status = redisReaderGetReply(connection.reader_.get(), &raw_reply);
        if (reader_status != REDIS_OK) {
            switch (connection.reader_budget_->rejection_) {
                case redis_reader_budget::rejection_type::elements:
                    throw redis_error(redis_error::code_type::protocol_error,
                        "redis reply exceeds configured element limit");
                case redis_reader_budget::rejection_type::depth:
                    throw redis_error(redis_error::code_type::protocol_error,
                        "redis array nesting is too deep");
                case redis_reader_budget::rejection_type::none:
                    throw redis_error(
                        redis_error::code_type::protocol_error, hiredis_reader_error(*connection.reader_));
            }
        }
        std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
            static_cast<redisReply*>(raw_reply), freeReplyObject);
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
            co_return hiredis_reply_to_value(
                *reply, 0, config_.max_array_depth_, detail::pmr_resource_or_default(resource));
        }

        auto read_capacity = connection.read_buffer_.size();
        if (config_.max_reply_bytes_.has_value()) {
            const auto unread = connection.reader_->len - connection.reader_->pos;
            read_capacity = std::min(read_capacity, *config_.max_reply_bytes_ - connection.reply_bytes_ - unread);
        }
        auto read_completion = co_await async_socket_read_some(connection,
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

task<void> redis_pool::connect(connection_type& connection, const ruvia::operation_timeout* operation_timeout_value) {
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
    if (!arm_deadline(connection, deadline_value, connection_type::deadline_kind_type::resolve)) {
        throw redis_error(redis_error::code_type::timeout, "redis resolve timed out");
    }
    auto resolve_completion = co_await ruvia::async_asio<asio::ip::tcp::resolver::results_type>(
        [this, &connection, port](auto handler) mutable {
            connection.resolver_.async_resolve(config_.host_, port, std::move(handler));
        });
    throw_if_aborted(connection);
    const auto resolve_ec = resolve_completion.error_code();
    auto endpoints = std::move(resolve_completion).take_result();
    if (clear_deadline(connection) || deadline_value.expired()) {
        throw redis_error(redis_error::code_type::timeout, "redis resolve timed out");
    }
    if (resolve_ec) {
        throw redis_error(redis_error::code_type::connect_failed, resolve_ec.message());
    }

    if (!arm_deadline(connection, deadline_value, connection_type::deadline_kind_type::socket)) {
        throw redis_error(redis_error::code_type::timeout, "redis connect timed out");
    }
    const auto connect_completion =
        co_await ruvia::async_asio([&connection, &endpoints](auto handler) mutable {
            asio::async_connect(connection.socket_, endpoints, std::move(handler));
        });
    throw_if_aborted(connection);
    const auto connect_ec = connect_completion.error_code();
    if (clear_deadline(connection) || deadline_value.expired()) {
        throw redis_error(redis_error::code_type::timeout, "redis connect timed out");
    }
    if (connect_ec) {
        throw redis_error(redis_error::code_type::connect_failed, connect_ec.message());
    }
    configure_socket(connection);
    connection.tls_stream_.reset();
    if (tls_context_) {
        connection.tls_stream_ = make_pmr_object<connection_type::tls_stream_type>(resource_, connection.socket_, *tls_context_);
        const auto& name = config_.tls_.server_name_.empty() ? config_.host_ : config_.tls_.server_name_;
        connection.tls_stream_->set_verify_callback(asio::ssl::host_name_verification(std::string(name)));
        std::error_code address_error;
        (void)asio::ip::make_address(name, address_error);
        if (address_error && SSL_set_tlsext_host_name(connection.tls_stream_->native_handle(), name.c_str()) != 1) {
            throw redis_error(redis_error::code_type::connect_failed, "configuring Redis TLS server name failed");
        }
        if (!arm_deadline(connection, deadline_value, connection_type::deadline_kind_type::socket)) {
            throw redis_error(redis_error::code_type::timeout, "redis TLS handshake timed out");
        }
        const auto handshake = co_await ruvia::async_asio([&connection](auto handler) {
            connection.tls_stream_->async_handshake(asio::ssl::stream_base::client, std::move(handler));
        });
        throw_if_aborted(connection);
        if (clear_deadline(connection) || deadline_value.expired()) {
            throw redis_error(redis_error::code_type::timeout, "redis TLS handshake timed out");
        }
        if (handshake.error_code()) {
            throw redis_error(redis_error::code_type::connect_failed, "redis TLS handshake failed: " + handshake.error_code().message());
        }
    }
    ensure_reader(connection);
    connection.connected_ = true;
    connection.reply_bytes_ = 0;

    try {
        co_await authenticate(connection, deadline_value);
    } catch (...) {
        close(connection);
        throw;
    }
}

task<void> redis_pool::authenticate(connection_type& connection, const ruvia::operation_timeout& connect_timeout) {
    auto run_control = [this, &connection, &connect_timeout](
                           std::span<const std::string_view> args) -> task<redis_value> {
        connection.write_buffer_.clear();
        connection.write_buffer_.reserve(resp_command_serialized_size(args));
        append_resp_command(connection.write_buffer_, args);
        const auto deadline_value = connect_timeout.constrained_by(command_timeout_);
        co_await async_socket_write(connection, deadline_value);

        co_return co_await read_reply(connection, deadline_value, resource_);
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
