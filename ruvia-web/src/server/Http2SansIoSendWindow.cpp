#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"

#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"

namespace ruvia::detail {

Task<Http2SendWindowWaitResult> awaitHttp2SendWindow(
    Http2Connection& connection,
    std::uint32_t streamId,
    Http2SansIoStreamSignal* signal) {
    for (;;) {
        auto* stream = connection.stream(streamId);
        if (stream == nullptr || stream->isAborted() ||
            signal == nullptr || signal->ended()) {
            co_return Http2SendWindowWaitResult::makeAborted();
        }
        if (!connection.hasQueuedData(streamId)) {
            co_return Http2SendWindowWaitResult::makeReady();
        }
        co_await signal->wait();
    }
}

}  // namespace ruvia::detail
