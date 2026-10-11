#include "client/client_tcp_transport.h"

#include <chrono>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/tcp_socket_options.h"

#include "tls/tls_stream_end.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_tls_error(const std::error_code& error) noexcept {
    return error.category() == asio::error::get_ssl_category() || error == asio::ssl::error::stream_truncated;
}

}  // namespace

client_tcp_transport::client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
    const worker_handle& worker_value, client_tcp_endpoint endpoint, client_wire_counters& counters)
    : worker_(worker_value),
      tls_(tls),
      endpoint_(endpoint),
      counters_(counters),
      resolver_(io),
      stream_(io, tls) {}

client_tcp_transport::~client_tcp_transport() = default;

void client_tcp_transport::abort(client_abort_reason reason) noexcept {
    // The latest owner decision wins: a pool shutdown reports closing even if
    // a request cancellation reached the transport first.
    if (reason != client_abort_reason::none) {
        abort_reason_ = reason;
    }
    resolver_.cancel();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
}

void client_tcp_transport::close() noexcept {
    read_.timer_.cancel();
    read_.deadline_.reset();
    write_.timer_.cancel();
    write_.deadline_.reset();
    resolver_.cancel();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}

bool client_tcp_transport::arm(lane_deadline& lane, lane_type kind, const operation_timeout& timeout) {
    lane.timer_.cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) {
        lane.deadline_.reset();
        return true;
    }
    if (remaining->count() == 0) {
        lane.deadline_.reset();
        return false;
    }
    const auto deadline_value = *timeout.deadline();
    lane.deadline_.arm(deadline_value, kind);
    (void)worker_.schedule_timer(lane.timer_, deadline_value, [this, &lane](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired && lane.deadline_.expire(std::chrono::steady_clock::now())) {
            abort(client_abort_reason::timeout);
        }
    });
    return true;
}

bool client_tcp_transport::clear(lane_deadline& lane) noexcept {
    lane.timer_.cancel();
    return lane.deadline_.clear();
}

client_transport_result client_tcp_transport::aborted() const noexcept {
    return {.status_ = client_transport_status::aborted,
        .abort_ = abort_reason_ == client_abort_reason::none ? client_abort_reason::timeout : abort_reason_};
}

client_transport_result client_tcp_transport::finish(lane_deadline& lane, const operation_timeout& timeout,
    const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept {
    if ((clear(lane) || timeout.expired()) && abort_reason_ == client_abort_reason::none) {
        abort_reason_ = client_abort_reason::timeout;
    }
    if (abort_reason_ != client_abort_reason::none) {
        return aborted();
    }
    if (error) {
        if (failure == client_transport_status::io_failed && is_stream_read_end(error)) {
            return {.status_ = client_transport_status::eof, .error_ = error};
        }
        const auto status = secure() && tls_started_ && is_tls_error(error) ? client_transport_status::tls_failed : failure;
        return {.status_ = status, .bytes_ = bytes, .error_ = error};
    }
    return {.bytes_ = bytes};
}

task<client_transport_result> client_tcp_transport::connect(client_alpn_mode alpn, const operation_timeout& timeout) {
    if (tls_started_) {
        // Closing a socket leaves Asio's encrypted input cursor and BIO pair
        // alive. Swap through a fresh stream so the retired SSL engine is
        // destroyed without overwriting its native owner.
        asio::ssl::stream<asio::ip::tcp::socket> fresh(stream_.get_executor(), tls_);
        std::swap(stream_, fresh);
        tls_started_ = false;
    }
    if (!arm(read_, lane_type::read, timeout)) {
        co_return aborted();
    }
    client_port_text_buffer_type port_buffer{};
    const auto port = format_client_port(endpoint_.port_, port_buffer);
    auto resolved = co_await async_asio<asio::ip::tcp::resolver::results_type>([this, port](auto handler) mutable {
        resolver_.async_resolve(endpoint_.host_, port, std::move(handler));
    });
    if (abort_reason_ != client_abort_reason::none || read_.deadline_.expired() || timeout.expired()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::resolve_failed);
    }
    if (resolved.error_code()) {
        co_return finish(read_, timeout, resolved.error_code(), 0, client_transport_status::resolve_failed);
    }
    const auto endpoints = std::move(resolved).take_result();
    const auto connected = co_await async_asio([this, &endpoints](auto handler) mutable {
        asio::async_connect(stream_.lowest_layer(), endpoints, std::move(handler));
    });
    if (connected.error_code() || abort_reason_ != client_abort_reason::none || read_.deadline_.expired() ||
        timeout.expired()) {
        co_return finish(read_, timeout, connected.error_code(), 0, client_transport_status::connect_failed);
    }
    apply_tcp_socket_policies(stream_.next_layer(), endpoint_.tcp_.no_delay_, endpoint_.tcp_.keep_alive_);
    if (!secure()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::connect_failed);
    }
    tls_started_ = true;
    if (const auto prepared = prepare_client_tls_stream(
            stream_, endpoint_.tls_->tls_host(endpoint_.host_), endpoint_.tls_->mode_, alpn);
        prepared != client_tls_setup_error::none) {
        (void)clear(read_);
        co_return client_transport_result{.status_ = client_transport_status::tls_failed,
            .error_ = make_client_tls_setup_error_code(prepared)};
    }
    const auto handshake = co_await async_asio([this](auto handler) mutable {
        stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
    });
    co_return finish(read_, timeout, handshake.error_code(), 0, client_transport_status::tls_failed);
}

std::string_view client_tcp_transport::alpn() noexcept {
    return tls_started_ ? selected_client_alpn(stream_.native_handle()) : std::string_view{};
}

task<client_transport_result> client_tcp_transport::read_some(std::span<char> bytes_value, const operation_timeout& timeout) {
    if (abort_reason_ != client_abort_reason::none || !arm(read_, lane_type::read, timeout)) {
        co_return aborted();
    }
    const auto buffer = asio::buffer(bytes_value.data(), bytes_value.size());
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.async_read_some(buffer, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.next_layer().async_read_some(buffer, std::move(handler));
                                  });
    auto result_value = finish(read_, timeout, completion.error_code(),
        completion.error_code() ? 0 : completion.result(), client_transport_status::io_failed);
    if (result_value.ok() && result_value.bytes_ == 0) {
        result_value.status_ = client_transport_status::eof;
    }
    counters_.received_ += result_value.bytes_;
    co_return result_value;
}

task<client_transport_result> client_tcp_transport::write(std::string_view bytes_value, const operation_timeout& timeout) {
    if (abort_reason_ != client_abort_reason::none) {
        co_return aborted();
    }
    if (bytes_value.empty()) {
        co_return client_transport_result{};
    }
    if (!arm(write_, lane_type::write, timeout)) {
        co_return aborted();
    }
    const auto buffer = asio::buffer(bytes_value);
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      asio::async_write(stream_, buffer, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      asio::async_write(stream_.next_layer(), buffer, std::move(handler));
                                  });
    auto result_value = finish(write_, timeout, completion.error_code(),
        completion.error_code() ? 0 : completion.result(), client_transport_status::io_failed);
    if (result_value.status_ == client_transport_status::eof) {
        result_value.status_ = client_transport_status::io_failed;
    }
    counters_.sent_ += result_value.bytes_;
    co_return result_value;
}

}  // namespace ruvia::detail
