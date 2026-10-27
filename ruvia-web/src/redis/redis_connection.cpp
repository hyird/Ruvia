#include "redis/redis_connection.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_pmr_string.h"

#include "redis/redis_protocol.h"
#include "redis/redis_types_access.h"
#include "redis/redis_utils.h"

namespace ruvia::detail {
namespace {

// Redis has no ALPN identifier, so the TLS handshake offers none.
[[nodiscard]] client_tcp_endpoint redis_endpoint(const redis_config_storage& config) noexcept {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_ = config.tls_.mode_ == client_tls_mode::disabled ? nullptr : &config.tls_,
        .tcp_ = config.tcp_,
        .alpn_ = client_alpn::none};
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
    const worker_handle& worker_value, const redis_config_storage& config, client_wire_counters& counters,
    std::pmr::memory_resource* resource)
    : config_(config),
      resource_(pmr_resource_or_default(resource)),
      transport_(io_context, tls_context, worker_value, redis_endpoint(config), counters),
      write_buffer_(resource_) {}

redis_connection::~redis_connection() = default;

void redis_connection::close() noexcept {
    transport_.close();
    connected_ = false;
    reader_.reset();
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
            make_pmr_object<redis_reply_builder>(resource_, config_.max_reply_bytes_, config_.max_array_depth_);
    }
    if (reader_ == nullptr) {
        reader_.reset(reply_builder_->create_reader());
    }
    reply_bytes_ = 0;
}

task<void> redis_connection::write_request(const operation_timeout& timeout) {
    const auto serialized_bytes = write_buffer_.size();
    const auto written = co_await transport_.write(write_buffer_, timeout);
    // The transport no longer borrows the buffer. Retention is amortized on
    // the request just sent so one oversized request does not pin its
    // capacity for the lifetime of this long-lived connection.
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    const auto amortized_bytes = serialized_bytes <= size_limit / 4 ? serialized_bytes * 4 : size_limit;
    http_clear_pmr_string_retaining_small(
        write_buffer_, std::max(redis_retained_write_buffer_bytes, amortized_bytes));
    throw_if_aborted();
    if (!written.ok()) {
        throw_redis_io_error(written);
    }
}

task<redis_value> redis_connection::read_reply(const operation_timeout& timeout, std::pmr::memory_resource* resource) {
    ensure_reader();
    reply_builder_->begin(pmr_resource_or_default(resource));
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
        // reader; report it before touching the reader again.
        throw_if_aborted();
        if (!received.ok()) {
            throw_redis_io_error(received);
        }
        if (redisReaderFeed(reader_.get(), read_buffer_.data(), received.bytes_) != REDIS_OK) {
            throw redis_error(
                redis_error::code_type::protocol_error, hiredis_reader_error(*reader_));
        }
    }
}

task<void> redis_connection::connect(const operation_timeout* operation_timeout_value,
    std::optional<std::chrono::milliseconds> command_timeout) {
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
                                    : operation_timeout(config_.connect_timeout_);
    try {
        // An expired deadline of the retired connection must not abort this one.
        transport_.reset_abort();
        const auto connected = co_await transport_.connect(deadline_value);
        throw_if_aborted();
        if (!connected.ok()) {
            throw_redis_connect_error(connected);
        }
        ensure_reader();
        connected_ = true;
        co_await authenticate(deadline_value, command_timeout);
    } catch (...) {
        close();
        throw;
    }
}

task<void> redis_connection::authenticate(const operation_timeout& connect_timeout,
    std::optional<std::chrono::milliseconds> command_timeout) {
    auto run_control = [this, &connect_timeout, command_timeout](
                           std::span<const std::string_view> args) -> task<redis_value> {
        write_buffer_.clear();
        write_buffer_.reserve(resp_command_serialized_size(args));
        append_resp_command(write_buffer_, args);
        const auto deadline_value = connect_timeout.constrained_by(command_timeout);
        co_await write_request(deadline_value);

        co_return co_await read_reply(deadline_value, resource_);
    };

    // An ACL user must authenticate by name even without a password (a
    // nopass user accepts any); skipping AUTH would connect as "default".
    if (!config_.username_.empty() || !config_.password_.empty()) {
        auto reply = redis_types_access::null_value(resource_);
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
