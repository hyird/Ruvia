#pragma once
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/WebSocketProtocolTypes.h"
#include "ruvia/web/detail/http2/Http2SansIoRequestBodyReader.h"
#include "ruvia/web/detail/http2/Http2SansIoTunnelTransport.h"
namespace ruvia::detail {
// RFC 8441 maps WebSocket transport termination to this CONNECT send half.
template <typename Executor>
class Http2SansIoWsTransport final {
public:
    template <typename... Args>
    explicit Http2SansIoWsTransport(Args&&... args)
        : tunnel_(std::forward<Args>(args)...) {}
    [[nodiscard]] Executor executor() const noexcept {
        return tunnel_.executor();
    }
    [[nodiscard]] Task<HttpStreamReadResult> readMore(std::pmr::string& buffer) {
        return tunnel_.readMore(buffer);
    }
    [[nodiscard]] Task<std::error_code> writeBytes(std::string_view bytes, WebSocketTransportDisposition disposition) {
        return tunnel_.writeBytes(bytes, disposition == WebSocketTransportDisposition::kEndTransport ? HttpStreamEnd::kEnd : HttpStreamEnd::kKeepOpen);
    }
    void abort() noexcept {
        tunnel_.abort();
    }

private:
    Http2SansIoTunnelTransport<Executor> tunnel_;
};
}  // namespace ruvia::detail
