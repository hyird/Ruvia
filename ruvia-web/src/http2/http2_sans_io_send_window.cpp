#include "http2/http2_sans_io_send_window.h"

#include <optional>
#include <stdexcept>

namespace ruvia::detail {
namespace {

[[nodiscard]] http2_stream_data_write_status classify_aborted_write(
    const http2_sans_io_stream_signal* signal, const bool* aborted) noexcept {
    if (aborted != nullptr && *aborted) {
        return http2_stream_data_write_status::local_aborted;
    }
    if (signal != nullptr && signal->terminated()) {
        return http2_stream_data_write_status::session_terminated;
    }
    return http2_stream_data_write_status::peer_closed;
}

}  // namespace

task<http2_send_window_wait_status> await_http2_send_window(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, const bool* aborted) {
    // Marks the whole wait, across wakes that find no capacity, so the
    // session's write_timeout check sees one continuous stall.
    struct send_window_wait_guard final {
        http2_sans_io_stream_signal& signal_;
        explicit send_window_wait_guard(http2_sans_io_stream_signal& signal_value) noexcept
            : signal_(signal_value) {
            signal_.set_waiting_send_window(true);
        }
        ~send_window_wait_guard() noexcept {
            signal_.set_waiting_send_window(false);
        }
        send_window_wait_guard(const send_window_wait_guard&) = delete;
        send_window_wait_guard& operator=(const send_window_wait_guard&) = delete;
    };
    std::optional<send_window_wait_guard> wait_guard;
    for (;;) {
        if ((aborted != nullptr && *aborted) ||
            connection.stream_aborted(stream_id) ||
            signal == nullptr || signal->terminated()) {
            co_return http2_send_window_wait_status::aborted;
        }
        const auto window = connection.send_window_state(stream_id);
        if (!window) {
            co_return http2_send_window_wait_status::aborted;
        }
        if (window->available_ != 0) {
            co_return http2_send_window_wait_status::ready;
        }
        if (!wait_guard.has_value()) {
            wait_guard.emplace(*signal);
        }
        co_await signal->wait();
    }
}

task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view bytes,
    http2_end_stream end_stream, const bool* aborted) {
    // The caller owns the suffix until consumed. Each reservation covers only
    // encoded payload and remains charged through socket write completion.
    for (;;) {
        if (aborted != nullptr && *aborted) {
            co_return http2_stream_data_write_status::local_aborted;
        }
        if (signal != nullptr && signal->terminated()) {
            co_return http2_stream_data_write_status::session_terminated;
        }
        if (signal == nullptr) {
            co_return http2_stream_data_write_status::peer_closed;
        }
        if (!bytes.empty()) {
            const auto window = connection.send_window_state(stream_id);
            if (!window || connection.stream_aborted(stream_id)) {
                co_return classify_aborted_write(signal, aborted);
            }
            if (window->available_ == 0) {
                if (co_await await_http2_send_window(connection, stream_id, signal, aborted) ==
                    http2_send_window_wait_status::aborted) {
                    co_return classify_aborted_write(signal, aborted);
                }
            }
            if (!output_budget.try_acquire(stream_id)) {
                if (!(co_await output_budget.acquire(stream_id, *signal, connection, aborted))) {
                    co_return classify_aborted_write(signal, aborted);
                }
                if ((aborted != nullptr && *aborted) || signal->terminated() ||
                    connection.stream_aborted(stream_id)) {
                    output_budget.release(stream_id);
                    co_return classify_aborted_write(signal, aborted);
                }
            }
        }
        http2_data_submit_result result_value{};
        try {
            result_value = connection.submit_data(stream_id, bytes, end_stream, http2_data_output_credit_bytes);
        } catch (const std::invalid_argument&) {
            write_signal.notify();
            if (!bytes.empty()) {
                output_budget.release(stream_id);
            }
            co_return http2_stream_data_write_status::content_length_exceeded;
        } catch (const std::logic_error&) {
            write_signal.notify();
            if (!bytes.empty()) {
                output_budget.release(stream_id);
            }
            co_return http2_stream_data_write_status::invalid_state;
        } catch (...) {
            write_signal.notify();
            if (!bytes.empty()) {
                output_budget.release(stream_id);
            }
            throw;
        }
        output_budget.note_data_submitted(stream_id, result_value.consumed_bytes_);
        write_signal.notify();
        if (!bytes.empty() && result_value.consumed_bytes_ == 0) {
            output_budget.release(stream_id);
        }
        bytes.remove_prefix(result_value.consumed_bytes_);
        if (result_value.status_ == http2_data_submit_status::complete) {
            co_return http2_stream_data_write_status::completed;
        }
        if (result_value.status_ == http2_data_submit_status::closed) {
            co_return http2_stream_data_write_status::peer_closed;
        }
        if (result_value.status_ == http2_data_submit_status::expectation_pending) {
            co_await signal->wait();
        }
    }
}

}  // namespace ruvia::detail
