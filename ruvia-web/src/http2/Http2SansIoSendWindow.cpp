#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"

#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"

namespace ruvia::detail {

Task<Http2SendWindowWaitResult> awaitHttp2SendWindow(
    ruvia::Http2Connection& connection, std::uint32_t streamId,
    Http2SansIoStreamSignal* signal) {
    for (;;) {
        if (connection.streamReceiveStatus(streamId) == ruvia::Http2StreamReceiveStatus::kClosed ||
            signal == nullptr || signal->terminated()) {
            co_return Http2SendWindowWaitResult::makeAborted();
        }
        if (!connection.hasQueuedData(streamId)) {
            co_return Http2SendWindowWaitResult::makeReady();
        }
        co_await signal->wait();
    }
}

}  // namespace ruvia::detail
