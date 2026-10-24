#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>
#include <asio/ssl/error.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"

#include "server/inbound_transport_error.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http1_peer_transport_ended(const std::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::error::connection_reset ||
           error == asio::error::connection_aborted || error == asio::error::broken_pipe ||
           error == asio::error::operation_aborted || error == asio::error::bad_descriptor ||
           error == asio::error::timed_out || error == asio::error::not_connected ||
           error == asio::error::shut_down || error == asio::ssl::error::stream_truncated;
}

// Constructed only on explicit opt-in, after the body reader is complete. The
// route owns cancellation and join; this object never borrows a request context.
// Both it and the transport remain alive until its sole child has joined.
template <typename stream_type>
class http1_peer_eof_monitor final {
public:
    http1_peer_eof_monitor(stream_type& stream, const worker_handle& worker,
        std::pmr::memory_resource* resource, std::size_t existing_pipeline_bytes)
        : stream_(stream),
          tasks_(worker, {.resource_ = resource}),
          input_(std::pmr::polymorphic_allocator<char>(resource)),
          allowance_(max_http_header_bytes - std::min(max_http_header_bytes, existing_pipeline_bytes)) {}

    void start() {
        tasks_.spawn(read_peer());
    }

    [[nodiscard]] stop_token token() const noexcept {
        return source_.token();
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_aborted_;
    }

    [[nodiscard]] std::string_view pipeline() const noexcept {
        return input_.view();
    }

    void ensure_writable() const {
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
        if (peer_aborted_) {
            throw inbound_transport_error(asio::error::operation_aborted,
                "HTTP response stream peer has disconnected");
        }
    }

    void write_failed(const std::error_code& error) noexcept {
        peer_aborted_ = peer_aborted_ || http1_peer_transport_ended(error);
        source_.request_stop();
        cancel();
    }

    void cancel() noexcept {
        // Set before emitting: an already-queued successful completion must
        // retain its bytes but must not schedule another read after teardown.
        closing_ = true;
        cancellation_.emit(asio::cancellation_type::terminal);
    }

    task<void> join() {
        co_await tasks_.join();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    task<void> read_peer() {
        try {
            // Stop once this bounded look-ahead is full. No input is discarded;
            // detecting a FIN behind more pipelined bytes then requires the
            // handler to finish so the session can consume those requests.
            while (!closing_ && input_.size() < allowance_) {
                // A quiet SSE needs no look-ahead allocation. Allocate the
                // bounded pipeline buffer only after application data arrives.
                char first_byte;
                const bool first_read = input_.capacity() == 0;
                const auto space = first_read ? std::span<char>(&first_byte, 1) : input_.prepare_spare();
                // Cancel this operation, not the socket's concurrent writer.
                // Asio's SSL io_op feeds received encrypted input into its
                // engine before honoring read cancellation; it remains there
                // for the session's next read after this task has joined.
                const auto completion = co_await async_asio<std::size_t>([this, space](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(space.data(), space.size()),
                        asio::bind_cancellation_slot(cancellation_.slot(), std::move(handler)));
                });
                // A terminal cancellation can race a successful read. Preserve
                // every returned byte before inspecting the shutdown state.
                if (first_read) {
                    if (completion.result() != 0) {
                        input_.reserve(allowance_);
                        input_.append(std::string_view(&first_byte, completion.result()));
                    }
                } else {
                    input_.commit(completion.result());
                }
                const auto error = completion.error_code();
                if (error) {
                    if (closing_ && error == asio::error::operation_aborted) {
                        co_return;
                    }
                    if (http1_peer_transport_ended(error)) {
                        peer_aborted_ = true;
                        source_.request_stop();
                        co_return;
                    }
                    throw inbound_transport_error(error, "failed to monitor HTTP response stream peer");
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
            source_.request_stop();
        }
    }

    stream_type& stream_;
    stop_source source_;
    task_scope tasks_;
    http_io_buffer input_;
    asio::cancellation_signal cancellation_;
    std::size_t allowance_;
    std::exception_ptr failure_;
    bool closing_{};
    bool peer_aborted_{};
};

}  // namespace ruvia::detail
