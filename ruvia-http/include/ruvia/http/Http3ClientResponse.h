#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3MessageBody.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class Http3ClientResponseEventKind : std::uint8_t {
    kPushPromise,
    kInformationalHead,
    kFinalHead,
    kBody,
    kTunnelData,
    kTrailerField,
    kMessageEnd,
    kReset,
};

struct Http3ClientResponseEvent final {
    Http3ClientResponseEventKind kind{Http3ClientResponseEventKind::kBody};
    std::uint64_t streamId{0};
    const Http3MessageHead* head{nullptr};
    Http3FieldSectionFieldView trailer{};
    std::span<const char> body{};
    // Final-head and message-end events carry the same value. Informational and
    // all other events leave it empty; unlike head/body views, it is not borrowed.
    std::optional<HttpResponseBodyPlan> responseBodyPlan{};
    std::optional<std::uint64_t> pushId{};
};

using Http3ClientResponseCallback = void (*)(void*, const Http3ClientResponseEvent&);

enum class Http3ClientResponseStatus : std::uint8_t {
    kNeedMoreData,
    kQpackBlocked,
    kMessageEnd,
    kReset,
    kStreamError,
    kConnectionError,
};

struct Http3ClientResponseResult final {
    Http3ClientResponseStatus status{Http3ClientResponseStatus::kNeedMoreData};
    Http3ConnectionErrorScope scope{Http3ConnectionErrorScope::kNone};
    Http3ConnectionErrorCode code{Http3ConnectionErrorCode::kNoError};
    std::size_t consumedBytes{0};
};

struct Http3ClientResponseLimits final {
    std::size_t maxFieldSectionSize{64 * 1024};
    std::size_t maxFields{256};
    std::size_t maxEncodedFieldSectionBytes{64 * 1024};
    std::optional<std::uint64_t> maxPushId{};
    bool pushStream{false};
};

// Sans-I/O receive state for one locally initiated bidirectional request stream.
// All decoded head storage and frame buffering belong to resource. Callback views
// are synchronous-only; head/trailer and body/tunnel-data storage is borrowed and
// valid only during the callback. A successful CONNECT response emits tunnel DATA
// separately and completes on stream FIN; trailers are forbidden. Callbacks must
// not reenter, move, or destroy this object. If a callback throws, the stream is
// terminal and cannot be resumed.
// A non-null decoder is borrowed for this response's entire lifetime. On
// kQpackBlocked, retain bytes after consumedBytes and repeat feed with that
// suffix (and FIN) after delivering encoder instructions to the shared decoder.
class Http3ClientResponse final {
public:
    Http3ClientResponse(std::uint64_t streamId, HttpKnownMethod requestMethod,
        std::pmr::memory_resource* resource, Http3ClientResponseLimits limits = {},
        Http3QpackDecoder* decoder = nullptr);
    ~Http3ClientResponse();
    Http3ClientResponse(Http3ClientResponse&&) noexcept;
    Http3ClientResponse& operator=(Http3ClientResponse&&) noexcept;
    Http3ClientResponse(const Http3ClientResponse&) = delete;
    Http3ClientResponse& operator=(const Http3ClientResponse&) = delete;

    [[nodiscard]] Http3ClientResponseResult feed(std::span<const char> bytes, bool fin, bool reset,
        Http3ClientResponseCallback callback, void* context);
    [[nodiscard]] std::uint64_t streamId() const noexcept;
    [[nodiscard]] bool authorizePush(std::uint64_t maximum) noexcept;

private:
    struct Impl;
    std::pmr::memory_resource* resource_;
    Impl* impl_;
};

}  // namespace ruvia
