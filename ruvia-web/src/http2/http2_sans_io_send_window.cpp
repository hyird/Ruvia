#include "http2/http2_sans_io_send_window.h"

#include "http2/http2_sans_io_stream_runtime.h"

namespace ruvia::detail {

task<http2_send_window_wait_result> await_http2_send_window(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal) {
    for (;;) {
        if (connection.stream_receive_status(stream_id) == ruvia::http2_stream_receive_status::closed ||
            signal == nullptr || signal->terminated()) {
            co_return http2_send_window_wait_result::make_aborted();
        }
        if (!connection.has_queued_data(stream_id)) {
            co_return http2_send_window_wait_result::make_ready();
        }
        co_await signal->wait();
    }
}

}  // namespace ruvia::detail
