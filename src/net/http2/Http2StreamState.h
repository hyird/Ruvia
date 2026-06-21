#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "Http2Frame.h"
#include "../../http/parser/HttpParserSyntax.h"
#include "../../router/RouteResolution.h"

namespace ruvia::detail {

enum class Http2StreamCloseSource : std::uint8_t {
    kNone,
    kLocal,
    kPeer
};

struct Http2HeaderField final {
    std::pmr::string name;
    std::pmr::string value;
    RequestHeaderKind kind{RequestHeaderKind::kOther};

    explicit Http2HeaderField(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : name(resource), value(resource) {}

    Http2HeaderField(
        std::string_view headerName,
        std::string_view headerValue,
        RequestHeaderKind headerKind,
        std::pmr::memory_resource* resource)
        : name(headerName, resource), value(headerValue, resource), kind(headerKind) {}
};

struct Http2StreamState final {
    explicit Http2StreamState(std::uint32_t streamId, std::pmr::memory_resource* resource)
        : id(streamId),
          method(resource),
          protocol(resource),
          scheme(resource),
          authority(resource),
          path(resource),
          cookie(resource),
          body(resource),
          headerBlock(resource),
          responseHeaderBlock(resource),
          lowerNameScratch(resource),
          fileChunk(resource),
          activeBodyChunk(resource),
          bodyChunks(resource),
          headers(resource),
          sendWindow(kHttp2DefaultInitialWindowSize) {}

    std::uint32_t id{0};
    std::pmr::string method;
    std::pmr::string protocol;
    std::pmr::string scheme;
    std::pmr::string authority;
    std::pmr::string path;
    std::pmr::string cookie;
    std::pmr::string body;
    std::pmr::string headerBlock;
    std::pmr::string responseHeaderBlock;
    std::pmr::string lowerNameScratch;
    std::pmr::string fileChunk;
    std::pmr::string activeBodyChunk;
    std::pmr::vector<std::pmr::string> bodyChunks;
    std::pmr::vector<Http2HeaderField> headers;
    std::size_t contentLength{0};
    std::size_t receivedBodyBytes{0};
    std::size_t bodyChunkOffset{0};
    std::int32_t sendWindow{0};
    std::int32_t receiveWindow{static_cast<std::int32_t>(kHttp2LocalInitialWindowSize)};
    RouteResolution routeResolution;
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    bool hasMethod{false};
    bool hasProtocol{false};
    bool hasScheme{false};
    bool hasAuthority{false};
    bool hasPath{false};
    bool hasHost{false};
    bool hasCookie{false};
    bool hasContentLength{false};
    bool regularHeaderSeen{false};
    bool headersDecoded{false};
    bool endStream{false};
    bool bodyEnded{false};
    bool queued{false};
    bool dispatchStarted{false};
    bool reset{false};
    bool standardConnect{false};
    bool extendedConnectWebSocket{false};
    bool webSocketTunnel{false};
    Http2StreamCloseSource closeSource{Http2StreamCloseSource::kNone};
    std::coroutine_handle<> bodyWaiter{};
};

}  // namespace ruvia::detail
