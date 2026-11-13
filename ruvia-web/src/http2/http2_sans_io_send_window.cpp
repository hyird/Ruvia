#include "http2/http2_sans_io_send_window.h"

#include <optional>
#include <stdexcept>

namespace ruvia::detail {
namespace {

[[nodiscard]] http2_stream_data_write_status classify_aborted_write(
    const multiplexed_stream_signal* signal, const bool* aborted) noexcept {
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
    multiplexed_stream_signal* signal, const bool* aborted) {
    // Marks the whole wait, across wakes that find no capacity, so the
    // session's write_timeout check sees one continuous stall.
    std::optional<multiplexed_stream_signal::output_wait_guard> wait_guard;
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

http2_stream_data_submit_result submit_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    worker_signal& write_signal, http2_data_output_budget& output_budget,
    std::string_view bytes, http2_end_stream end_stream) {
    struct submission_completion final {
        http2_data_output_budget& budget_;
        worker_signal& write_signal_;
        std::uint32_t stream_id_;
        bool release_credit_;
        ~submission_completion() noexcept {
            write_signal_.notify();
            if (release_credit_) {
                budget_.release(stream_id_);
            }
        }
    } completion{output_budget, write_signal, stream_id, !bytes.empty()};
    try {
        const auto progress = connection.submit_data(
            stream_id, bytes, end_stream, http2_data_output_credit_bytes);
        output_budget.note_data_submitted(stream_id, progress.consumed_bytes_);
        completion.release_credit_ = !bytes.empty() && progress.consumed_bytes_ == 0;
        if (progress.status_ == http2_data_submit_status::invalid_message) {
            return {.failure_ = http2_stream_data_write_status::content_length_exceeded};
        }
        return {.progress_ = progress};
    } catch (const std::logic_error&) {
        return {.failure_ = http2_stream_data_write_status::invalid_state};
    }
}

task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    multiplexed_stream_signal* signal, worker_signal& write_signal,
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
        // Match the protocol engine's nghttp2 backpressure contract, including
        // empty END_STREAM DATA.
        if (!bytes.empty() || http2_ends_stream(end_stream)) {
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
        }
        if (!bytes.empty()) {
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
        const auto submitted = submit_http2_stream_data(
            connection, stream_id, write_signal, output_budget, bytes, end_stream);
        if (submitted.failure_) {
            co_return *submitted.failure_;
        }
        const auto& result_value = submitted.progress_;
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
