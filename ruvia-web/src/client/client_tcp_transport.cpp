#include "client/client_tcp_transport.h"

#include <chrono>
#include <utility>

#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/Async.h"

#include "client/HttpClientConfigStorage.h"
#include "client/HttpClientResponseState.h"

namespace ruvia::detail {

client_tcp_transport::client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
    const WorkerHandle& worker, const HttpClientConfigStorage& config,
    client_wire_counters& counters, std::pmr::memory_resource* resource)
    : worker_(worker),
      config_(config),
      counters_(counters),
      resolver_(io),
      stream_(io, tls),
      timer_(makePmrObject<WorkerTimerRegistration>(resource)) {}
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
        response_->spaceSignal.notify();
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

bool client_tcp_transport::arm_deadline(const ruvia::OperationTimeout& timeout, client_deadline_kind kind) {
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
    const auto deadline = *timeout.deadline();
    deadline_.arm(deadline, kind);
    worker_.schedule_timer(*timer_, deadline, [this](WorkerTimerOutcome outcome) noexcept {
        if (outcome != WorkerTimerOutcome::kExpired) {
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
            throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
        case client_abort_reason::cancelled:
            throw HttpClientError(
                HttpClientError::Code::kCancelled, "http client request cancelled");
        case client_abort_reason::closing:
            throw HttpClientError(HttpClientError::Code::kClosing, "http client pool is closing");
    }
}

HttpClientError::Code client_tcp_transport::error_code(
    const std::error_code& error) const noexcept {
    return config_.scheme == HttpScheme::kHttps &&
                   (error.category() == asio::error::get_ssl_category() ||
                       error == asio::ssl::error::stream_truncated)
               ? HttpClientError::Code::kTlsFailed
               : HttpClientError::Code::kIoError;
}

Task<void> client_tcp_transport::write(std::string_view bytes, const ruvia::OperationTimeout& timeout) {
    if (bytes.empty()) {
        co_return;
    }
    const auto write_timeout = timeout.constrainedBy(config_.write_timeout);
    if (!arm_deadline(write_timeout, client_deadline_kind::socket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    }
    auto completion =
        config_.scheme == HttpScheme::kHttps
            ? co_await ruvia::asyncAsio<std::size_t>([this, bytes](auto handler) mutable {
                  asio::async_write(stream_, asio::buffer(bytes), std::move(handler));
              })
            : co_await ruvia::asyncAsio<std::size_t>([this, bytes](auto handler) mutable {
                  asio::async_write(
                      stream_.next_layer(), asio::buffer(bytes), std::move(handler));
              });
    const bool timed_out = clear_deadline() || write_timeout.expired();
    throw_if_aborted();
    if (timed_out) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    }
    if (completion.errorCode()) {
        throw HttpClientError(
            error_code(completion.errorCode()), completion.errorCode().message());
    }
    counters_.sent += completion.result();
}

Task<std::size_t> client_tcp_transport::read_some(std::span<char> bytes, const ruvia::OperationTimeout& timeout, bool allow_eof) {
    if (!arm_deadline(timeout, client_deadline_kind::socket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    }
    auto completion =
        config_.scheme == HttpScheme::kHttps
            ? co_await ruvia::asyncAsio<std::size_t>([this, bytes](auto handler) mutable {
                  stream_.async_read_some(
                      asio::buffer(bytes.data(), bytes.size()), std::move(handler));
              })
            : co_await ruvia::asyncAsio<std::size_t>([this, bytes](auto handler) mutable {
                  stream_.next_layer().async_read_some(
                      asio::buffer(bytes.data(), bytes.size()), std::move(handler));
              });
    const bool timed_out = clear_deadline() || timeout.expired();
    throw_if_aborted();
    if (timed_out) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    }
    if (completion.errorCode()) {
        if (allow_eof && completion.errorCode() == asio::error::eof) {
            co_return 0;
        }
        throw HttpClientError(
            error_code(completion.errorCode()), completion.errorCode().message());
    }
    counters_.received += completion.result();
    co_return completion.result();
}

}  // namespace ruvia::detail
