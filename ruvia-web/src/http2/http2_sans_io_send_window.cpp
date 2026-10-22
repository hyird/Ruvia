#include "http2/http2_sans_io_send_window.h"

#include <algorithm>
#include <cstddef>

#include "http2/http2_sans_io_stream_runtime.h"

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
    for (;;) {
        if ((aborted != nullptr && *aborted) ||
            connection.stream_receive_status(stream_id) == ruvia::http2_stream_receive_status::closed ||
            signal == nullptr || signal->terminated()) {
            co_return http2_send_window_wait_status::aborted;
        }
        if (!connection.has_queued_data(stream_id)) {
            co_return http2_send_window_wait_status::ready;
        }
        co_await signal->wait();
    }
}

task<http2_stream_data_write_status> write_http2_stream_data_chunk(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view chunk,
    http2_end_stream end_stream, const bool* aborted) {
    // queued transfers the unsent suffix to the core; backpressured retains
    // caller ownership and must retry this exact stable view after the older
    // queued input drains.
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
        // Empty END_STREAM needs no flow-control credit (RFC 9113 section
        // 6.9.1), but must not overtake older core-owned DATA.
        if (chunk.empty() && end_stream == http2_end_stream::end_stream &&
            connection.has_queued_data(stream_id)) {
            if (co_await await_http2_send_window(connection, stream_id, signal, aborted) ==
                http2_send_window_wait_status::aborted) {
                co_return classify_aborted_write(signal, aborted);
            }
            continue;
        }
        if (!chunk.empty()) {
            for (;;) {
                const auto window = connection.send_window_state(stream_id);
                if (!window) {
                    co_return http2_stream_data_write_status::peer_closed;
                }
                if (aborted != nullptr && *aborted) {
                    co_return http2_stream_data_write_status::local_aborted;
                }
                if (signal->terminated()) {
                    co_return http2_stream_data_write_status::session_terminated;
                }
                if (window->available_ != 0) {
                    break;
                }
                co_await output_budget.wait_for_change();
                if (aborted != nullptr && *aborted) {
                    co_return http2_stream_data_write_status::local_aborted;
                }
                if (signal->terminated()) {
                    co_return http2_stream_data_write_status::session_terminated;
                }
            }
            if (!(co_await output_budget.acquire(stream_id, *signal))) {
                co_return classify_aborted_write(signal, aborted);
            }
            if ((aborted != nullptr && *aborted) || signal->terminated()) {
                output_budget.release(stream_id);
                co_return classify_aborted_write(signal, aborted);
            }
        }
        http2_data_submit_status result_value{};
        try {
            result_value = connection.submit_data(stream_id, chunk, end_stream);
        } catch (const std::invalid_argument&) {
            write_signal.notify();
            if (!chunk.empty()) {
                output_budget.release(stream_id);
            }
            co_return http2_stream_data_write_status::content_length_exceeded;
        } catch (const std::logic_error&) {
            write_signal.notify();
            if (!chunk.empty()) {
                output_budget.release(stream_id);
            }
            co_return http2_stream_data_write_status::invalid_state;
        } catch (...) {
            write_signal.notify();
            if (!chunk.empty()) {
                output_budget.release(stream_id);
            }
            throw;
        }
        if (result_value == http2_data_submit_status::accepted ||
            result_value == http2_data_submit_status::queued) {
            output_budget.note_data_submitted(stream_id, chunk.size());
        }
        write_signal.notify();
        if (!chunk.empty() && result_value != http2_data_submit_status::accepted &&
            result_value != http2_data_submit_status::queued) {
            output_budget.release(stream_id);
        }
        if (result_value == http2_data_submit_status::accepted) {
            co_return http2_stream_data_write_status::completed;
        }
        if (result_value == http2_data_submit_status::closed) {
            co_return http2_stream_data_write_status::peer_closed;
        }
        if (co_await await_http2_send_window(connection, stream_id, signal, aborted) ==
            http2_send_window_wait_status::aborted) {
            co_return classify_aborted_write(signal, aborted);
        }
        if (result_value == http2_data_submit_status::queued) {
            co_return http2_stream_data_write_status::completed;
        }
    }
}

task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view bytes,
    http2_end_stream end_stream, const bool* aborted) {
    constexpr std::size_t submit_chunk_bytes = http2_data_output_credit_bytes;
    std::size_t offset = 0;
    bool submitted_empty_terminal = false;
    do {
        const auto count = bytes.empty() ? 0 : std::min(submit_chunk_bytes, bytes.size() - offset);
        const auto chunk = bytes.substr(offset, count);
        const bool last = offset + count == bytes.size();
        const auto status = co_await write_http2_stream_data_chunk(connection, stream_id, signal,
            write_signal, output_budget, chunk, last ? end_stream : http2_end_stream::keep_open, aborted);
        if (status != http2_stream_data_write_status::completed) {
            co_return status;
        }
        offset += count;
        submitted_empty_terminal = bytes.empty();
    } while (offset < bytes.size() || (!submitted_empty_terminal && bytes.empty()));
    co_return http2_stream_data_write_status::completed;
}
}  // namespace ruvia::detail
