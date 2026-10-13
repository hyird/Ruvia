#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/error_text.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/web/detail/redis/redis_utils.h"
#include "ruvia/web/redis/redis.h"

#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"

namespace ruvia::detail {
namespace {

// Redis' mapping of failed client_tcp_transport results. Cancellation and
// pool shutdown abort the transport and surface through the slot's abort
// reason, which callers check first. An aborted result is therefore an
// expired deadline.
[[noreturn]] void throw_redis_connect_error(const client_transport_result& result) {
    switch (result.status_) {
        case client_transport_status::aborted:
            throw redis_error(redis_error::code_type::timeout, "redis connect timed out");
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
        throw redis_error(redis_error::code_type::timeout, "redis command timed out");
    }
    if (result.status_ == client_transport_status::eof) {
        throw redis_error(redis_error::code_type::io_error, "redis server closed the connection");
    }
    throw redis_error(redis_error::code_type::io_error, error_code_text(result.error_));
}

}  // namespace

void redis_pool::close(std::size_t slot) noexcept {
    auto& connection = *connections_[slot];
    connection.transport_.close();
    connection.connected_ = false;
    connection.reader_.reset();
    connection.reply_bytes_ = 0;
}

void redis_pool::abort(std::size_t slot, client_abort_reason reason) noexcept {
    connections_[slot]->transport_.abort(reason);
    close(slot);
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

task<void> redis_pool::write_request(std::size_t slot, const ruvia::operation_timeout& timeout) {
    auto& connection = *connections_[slot];
    const auto serialized_bytes = connection.write_buffer_.size();
    const auto written = co_await connection.transport_.write(connection.write_buffer_, timeout);
    // The transport no longer borrows the buffer. Retention is amortized on
    // the request just sent so one oversized request does not pin its
    // capacity for the lifetime of this long-lived connection.
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    const auto amortized_bytes = serialized_bytes <= size_limit / 4 ? serialized_bytes * 4 : size_limit;
    http_clear_pmr_string_retaining_small(
        connection.write_buffer_, std::max(redis_retained_write_buffer_bytes, amortized_bytes));
    slots_.throw_if_aborted(slot);
    if (!written.ok()) {
        throw_redis_io_error(written);
    }
}

task<redis_value> redis_pool::read_reply(
    std::size_t slot, const ruvia::operation_timeout& timeout, std::pmr::memory_resource* resource) {
    auto& connection = *connections_[slot];
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
        const auto received = co_await connection.transport_.read_some(
            std::span<char>(connection.read_buffer_.data(), read_capacity), timeout);
        // A cancellation or shutdown closed the connection and released its
        // reader; report it before touching the reader again.
        slots_.throw_if_aborted(slot);
        if (!received.ok()) {
            throw_redis_io_error(received);
        }
        if (redisReaderFeed(connection.reader_.get(), connection.read_buffer_.data(), received.bytes_) != REDIS_OK) {
            throw redis_error(
                redis_error::code_type::protocol_error, hiredis_reader_error(*connection.reader_));
        }
    }
}

task<void> redis_pool::connect(std::size_t slot, const ruvia::operation_timeout* operation_timeout_value) {
    auto& connection = *connections_[slot];
    if (connection.connected_) {
        // A peer that closed the idle connection or sent unsolicited bytes
        // would desynchronize the next reply; reconnect instead.
        const bool reader_idle =
            connection.reader_ == nullptr || connection.reader_->len == connection.reader_->pos;
        if (reader_idle && connection.transport_.idle_open()) {
            co_return;
        }
        close(slot);
    }
    const auto deadline_value = operation_timeout_value != nullptr
                                    ? operation_timeout_value->constrained_by(config_.connect_timeout_)
                                    : ruvia::operation_timeout(config_.connect_timeout_);
    try {
        slots_.throw_if_aborted(slot);
        // An expired deadline of the retired connection must not abort this one.
        connection.transport_.reset_abort();
        const auto connected = co_await connection.transport_.connect(deadline_value);
        slots_.throw_if_aborted(slot);
        if (!connected.ok()) {
            throw_redis_connect_error(connected);
        }
        ensure_reader(connection);
        connection.connected_ = true;
        co_await authenticate(slot, deadline_value);
    } catch (...) {
        close(slot);
        throw;
    }
}

task<void> redis_pool::authenticate(std::size_t slot, const ruvia::operation_timeout& connect_timeout) {
    auto& connection = *connections_[slot];
    auto run_control = [this, slot, &connection, &connect_timeout](
                           std::span<const std::string_view> args) -> task<redis_value> {
        connection.write_buffer_.clear();
        connection.write_buffer_.reserve(resp_command_serialized_size(args));
        append_resp_command(connection.write_buffer_, args);
        const auto deadline_value = connect_timeout.constrained_by(command_timeout_);
        co_await write_request(slot, deadline_value);

        co_return co_await read_reply(slot, deadline_value, resource_);
    };

    // An ACL user must authenticate by name even without a password (a
    // nopass user accepts any); skipping AUTH would connect as "default".
    if (!config_.username_.empty() || !config_.password_.empty()) {
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
