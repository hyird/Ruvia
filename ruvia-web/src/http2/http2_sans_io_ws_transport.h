#pragma once
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/websocket_protocol_types.h"

#include "http2/http2_sans_io_request_body_reader.h"
#include "http2/http2_sans_io_tunnel_transport.h"
namespace ruvia::detail {
// RFC 8441 maps websocket transport termination to this CONNECT send half.
template <typename executor_type>
class http2_sans_io_ws_transport final {
public:
    template <typename... args_type>
    explicit http2_sans_io_ws_transport(args_type&&... args)
        : tunnel_(std::forward<args_type>(args)...) {}
    [[nodiscard]] executor_type executor() const noexcept {
        return tunnel_.executor();
    }
    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        return tunnel_.read_more(buffer);
    }
    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes_value, websocket_transport_disposition disposition) {
        return tunnel_.write_bytes(bytes_value, disposition == websocket_transport_disposition::end_transport ? http_stream_end::end : http_stream_end::keep_open);
    }
    void abort() noexcept {
        tunnel_.abort();
    }

private:
    http2_sans_io_tunnel_transport<executor_type> tunnel_;
};
}  // namespace ruvia::detail
