#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_response_head_buffer.h"

#include "server/connection_scanner.h"
#include "server/http_connection_state.h"
#include "server/http_response_writer.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

// Serialized output still bounded by one coalesced write. A pipelined
// connection holds small buffered responses back while their successors are
// already buffered, then sends them with the response that ends the run.
inline constexpr std::size_t pipelined_output_batch_bytes = std::size_t{16} * 1024;

// One write of a connection's serialized response output, optionally followed
// by a borrowed body. The output is the work set's head buffer: the current
// response's head, preceded by any pipelined responses held back for this
// write. A write starts with one non-blocking attempt on plain TCP; only a
// remainder suspends, through async_asio at the caller's co_await site so a
// response the socket accepts at once costs no coroutine frame:
//
//     if (!output.start(body)) {
//         auto completion = co_await async_asio<std::size_t>(
//             [&output](auto handler) { output.write_remaining(std::move(handler)); });
//         output.finish(completion.error_code(), completion.result());
//     }
//     const auto written = output.complete();
//
// complete() reports every held-back response whose head the write committed
// and empties the output. The output must be empty before anything else
// writes to the stream (interim heads, streams, upgrades, file bodies) and
// before the session reads or closes.
template <typename stream_type>
class http1_response_output final {
public:
    // A write either accepts every byte or ends with an error.
    struct written_type final {
        std::error_code error_;
        // Prefix of output-then-body the transport accepted.
        std::size_t bytes_{};
    };

    http1_response_output(stream_type& stream, http_response_head_buffer& output,
        http1_deferred_access_log& deferred_access, const access_log_sink& access_log,
        connection_scanner::entry_type& progress) noexcept
        : stream_(&stream),
          output_(&output),
          deferred_access_(&deferred_access),
          access_log_(&access_log),
          progress_(&progress) {}

    http1_response_output(const http1_response_output&) = delete;
    http1_response_output& operator=(const http1_response_output&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return output_->view().empty();
    }

    // Begins writing the output followed by `body`, which stays borrowed until
    // complete(). Returns true when the write already settled.
    // MSVC 19.51 can lose a required coroutine tail call when this synchronous
    // path is inlined into its caller.
    [[nodiscard]]
#if defined(_MSC_VER) && !defined(__clang__)
    __declspec(noinline)
#endif
    bool start(std::string_view body = {}) noexcept {
        body_ = body;
        written_ = {};
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
            const auto output = output_->view();
            return body.empty() ? try_plain_tcp_sync_write(*stream_, asio::buffer(output), output.size(),
                                      written_.error_, written_.bytes_)
                                : try_plain_tcp_sync_write(*stream_,
                                      std::array<asio::const_buffer, 2>{asio::buffer(output), asio::buffer(body)},
                                      output.size() + body.size(), written_.error_, written_.bytes_);
        } else {
            return false;
        }
    }

    // Initiates the asynchronous write of what start() left. Every partial
    // transfer touches the scanner entry, so write_timeout measures
    // inactivity rather than the time to drain the whole output.
    template <typename handler_type>
    void write_remaining(handler_type&& handler) {
        progress_->set_phase(connection_scanner::phase_type::writing);
        const auto output = output_->view();
        const auto written = written_.bytes_;
        const std::array<asio::const_buffer, 2> remaining =
            written < output.size()
                ? std::array<asio::const_buffer, 2>{asio::buffer(output.substr(written)), asio::buffer(body_)}
                : std::array<asio::const_buffer, 2>{
                      asio::buffer(body_.substr(written - output.size())), asio::const_buffer{}};
        asio::async_write(*stream_, remaining, http_write_progress(*progress_), std::forward<handler_type>(handler));
    }

    // Records the asynchronous remainder's outcome.
    void finish(std::error_code error, std::size_t bytes) noexcept {
        written_.error_ = error;
        written_.bytes_ += bytes;
    }

    // Reports the held-back responses whose heads were written, empties the
    // output and returns the write's outcome.
    [[nodiscard]] written_type complete() noexcept {
        deferred_access_->complete(*access_log_, written_.bytes_);
        output_->reset();
        body_ = {};
        return written_;
    }

    // Writes held-back output through a coroutine, for the rare writers
    // (interim heads, file responses) that need the output empty first. A
    // failed write leaves the connection unusable. The caller's scanner phase
    // (a handler's idle deadman, a body read, a request-head read) resumes
    // once the write settles.
    [[nodiscard]] task<std::error_code> flush() {
        if (empty()) {
            co_return std::error_code{};
        }
        if (!start()) {
            const connection_scanner::entry_type::phase_scope_type caller_phase(*progress_);
            auto completion = co_await async_asio<std::size_t>(
                [this](auto handler) { this->write_remaining(std::move(handler)); });
            finish(completion.error_code(), completion.result());
        }
        co_return complete().error_;
    }

private:
    stream_type* stream_;
    http_response_head_buffer* output_;
    http1_deferred_access_log* deferred_access_;
    const access_log_sink* access_log_;
    connection_scanner::entry_type* progress_;
    std::string_view body_;
    written_type written_;
};

}  // namespace ruvia::detail
