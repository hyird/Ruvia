#include "client/client_tcp_transport.h"

#include <chrono>
#include <utility>

#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/async.h"

#include "client/http_client_config_storage.h"
#include "client/http_client_response_state.h"

namespace ruvia::detail {

client_tcp_transport::client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
    const worker_handle& worker_value, const http_client_config_storage& config,
    client_wire_counters& counters, std::pmr::memory_resource* resource)
    : worker_(worker_value),
      config_(config),
      counters_(counters),
      resolver_(io),
      stream_(io, tls),
      timer_(make_pmr_object<worker_timer_registration>(resource)) {}
client_tcp_transport::~client_tcp_transport() = default;
client_tcp_transport::client_tcp_transport(client_tcp_transport&&) noexcept = default;
void client_tcp_transport::stop_output() noexcept {
    if (response_ != nullptr) {
        if (auto* output = response_->output()) {
            output->stop();
        }
    }
}
void client_tcp_transport::abort_output(client_abort_reason reason) noexcept {
    abort_reason_ = reason;
    stop_output();
    if (response_ != nullptr) {
        response_->space_signal_.notify();
    }
}
void client_tcp_transport::close() noexcept {
    timer_->cancel();
    deadline_.reset();
    resolver_.cancel();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}
void client_tcp_transport::cancel(client_abort_reason reason) noexcept {
    abort_output(reason);
    resolver_.cancel();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}

void client_tcp_transport::prepare_reconnect(asio::ssl::context& tls) {
    if (!tls_started_) {
        return;
    }
    // Closing a socket leaves Asio's encrypted input cursor and BIO pair alive.
    // Swap through moved-from streams so the retired SSL engine is destroyed
    // without overwriting its native owner.
    asio::ssl::stream<asio::ip::tcp::socket> fresh(stream_.get_executor(), tls);
    std::swap(stream_, fresh);
    tls_started_ = false;
}

bool client_tcp_transport::arm_deadline(const ruvia::operation_timeout& timeout, client_deadline_kind kind) {
    timer_->cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) {
        deadline_.reset();
        return true;
    }
    if (remaining->count() == 0) {
        deadline_.reset();
        return false;
    }
    const auto deadline_value = *timeout.deadline();
    deadline_.arm(deadline_value, kind);
    worker_.schedule_timer(*timer_, deadline_value, [this](worker_timer_outcome outcome) noexcept {
        if (outcome != worker_timer_outcome::expired) {
            return;
        }
        const auto expired = deadline_.expire(std::chrono::steady_clock::now());
        if (!expired) {
            return;
        }
        abort_output(client_abort_reason::timeout);
        std::error_code ignored;
        if (*expired == client_deadline_kind::resolve) {
            resolver_.cancel();
        } else if (*expired == client_deadline_kind::socket) {
            (void)stream_.lowest_layer().cancel(ignored);
        }
    });
    return true;
}

bool client_tcp_transport::clear_deadline() noexcept {
    timer_->cancel();
    return deadline_.clear();
}

void client_tcp_transport::throw_if_aborted() const {
    switch (abort_reason_) {
        case client_abort_reason::none:
            return;
        case client_abort_reason::timeout:
            throw http_client_error(http_client_error::code_type::timeout, "http client request timed out");
        case client_abort_reason::cancelled:
            throw http_client_error(
                http_client_error::code_type::cancelled, "http client request cancelled");
        case client_abort_reason::closing:
            throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
    }
}

http_client_error::code_type client_tcp_transport::error_code(
    const std::error_code& error) const noexcept {
    return config_.scheme_ == http_scheme::https &&
                   (error.category() == asio::error::get_ssl_category() ||
                       error == asio::ssl::error::stream_truncated)
               ? http_client_error::code_type::tls_failed
               : http_client_error::code_type::io_error;
}

task<void> client_tcp_transport::write(std::string_view bytes_value, const ruvia::operation_timeout& timeout) {
    if (bytes_value.empty()) {
        co_return;
    }
    const auto write_timeout = timeout.constrained_by(config_.write_timeout_);
    if (!arm_deadline(write_timeout, client_deadline_kind::socket)) {
        throw http_client_error(http_client_error::code_type::timeout, "http client write timed out");
    }
    auto completion =
        config_.scheme_ == http_scheme::https
            ? co_await ruvia::async_asio<std::size_t>([this, bytes_value](auto handler) mutable {
                  asio::async_write(stream_, asio::buffer(bytes_value), std::move(handler));
              })
            : co_await ruvia::async_asio<std::size_t>([this, bytes_value](auto handler) mutable {
                  asio::async_write(
                      stream_.next_layer(), asio::buffer(bytes_value), std::move(handler));
              });
    const bool timed_out = clear_deadline() || write_timeout.expired();
    throw_if_aborted();
    if (timed_out) {
        throw http_client_error(http_client_error::code_type::timeout, "http client write timed out");
    }
    if (completion.error_code()) {
        throw http_client_error(
            error_code(completion.error_code()), completion.error_code().message());
    }
    counters_.sent_ += completion.result();
}

task<std::size_t> client_tcp_transport::read_some(std::span<char> bytes_value, const ruvia::operation_timeout& timeout, bool allow_eof) {
    if (!arm_deadline(timeout, client_deadline_kind::socket)) {
        throw http_client_error(http_client_error::code_type::timeout, "http client request timed out");
    }
    auto completion =
        config_.scheme_ == http_scheme::https
            ? co_await ruvia::async_asio<std::size_t>([this, bytes_value](auto handler) mutable {
                  stream_.async_read_some(
                      asio::buffer(bytes_value.data(), bytes_value.size()), std::move(handler));
              })
            : co_await ruvia::async_asio<std::size_t>([this, bytes_value](auto handler) mutable {
                  stream_.next_layer().async_read_some(
                      asio::buffer(bytes_value.data(), bytes_value.size()), std::move(handler));
              });
    const bool timed_out = clear_deadline() || timeout.expired();
    throw_if_aborted();
    if (timed_out) {
        throw http_client_error(http_client_error::code_type::timeout, "http client request timed out");
    }
    if (completion.error_code()) {
        if (allow_eof && completion.error_code() == asio::error::eof) {
            co_return 0;
        }
        throw http_client_error(
            error_code(completion.error_code()), completion.error_code().message());
    }
    counters_.received_ += completion.result();
    co_return completion.result();
}

}  // namespace ruvia::detail
