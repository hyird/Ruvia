#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

#include "Http2Frame.h"
#include "Http2HeaderList.h"
#include "../../router/RouteResolution.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

enum class Http2StreamCloseSource : std::uint8_t {
    kNone,
    kLocal,
    kPeer
};

struct Http2StreamState final {
    explicit Http2StreamState(std::uint32_t streamId, std::pmr::memory_resource* resource)
        : id(streamId),
          authority(resource),
          path(resource),
          cookie(resource),
          body(resource),
          headerBlock(resource),
          responseHeaderBlock(resource),
          queuedBodyChunk(resource),
          activeBodyChunk(resource),
          bodyChunks(resource),
          headers(resource),
          sendWindow(kHttp2DefaultInitialWindowSize) {}

    std::uint32_t id{0};
    HttpMethod method{HttpMethod::kUnknown};
    std::pmr::string authority;
    std::pmr::string path;
    std::pmr::string cookie;
    std::pmr::string body;
    std::pmr::string headerBlock;
    std::pmr::string responseHeaderBlock;
    std::pmr::string queuedBodyChunk;
    std::pmr::string activeBodyChunk;
    std::pmr::vector<std::pmr::string> bodyChunks;
    Http2HeaderList headers;
    std::size_t contentLength{0};
    std::size_t receivedBodyBytes{0};
    std::size_t bodyChunkOffset{0};
    std::int32_t sendWindow{0};
    std::int32_t receiveWindow{static_cast<std::int32_t>(kHttp2LocalInitialWindowSize)};
    RouteResolution routeResolution;
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    bool hasMethod : 1 {false};
    bool hasProtocol : 1 {false};
    bool protocolIsWebSocket : 1 {false};
    bool hasScheme : 1 {false};
    bool hasAuthority : 1 {false};
    bool hasPath : 1 {false};
    bool hasHost : 1 {false};
    bool hasCookie : 1 {false};
    bool hasContentLength : 1 {false};
    bool regularHeaderSeen : 1 {false};
    bool headersDecoded : 1 {false};
    bool endStream : 1 {false};
    bool bodyEnded : 1 {false};
    bool hasQueuedBodyChunk : 1 {false};
    bool queued : 1 {false};
    bool dispatchStarted : 1 {false};
    bool reset : 1 {false};
    bool standardConnect : 1 {false};
    bool extendedConnectWebSocket : 1 {false};
    bool webSocketTunnel : 1 {false};
    Http2StreamCloseSource closeSource{Http2StreamCloseSource::kNone};
    std::coroutine_handle<> bodyWaiter{};
};

}  // namespace ruvia::detail
