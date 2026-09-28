#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/http/Http3ConnectionError.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3MessageBody.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class Http3ConnectionStatus : std::uint8_t {
    kNeedMoreData,
    kMessageEnd,
    kStreamError,
    kConnectionError,
    kReset,
};

enum class Http3ConnectionEventKind : std::uint8_t {
    kRequestHead,
    kInformationalHead,
    kFinalHead,
    kTunnelData,
    kBody,
    kTrailerField,
    kMessageEnd,
    kReset,
};

struct Http3ConnectionEvent final {
    Http3ConnectionEventKind kind{Http3ConnectionEventKind::kBody};
    std::uint64_t streamId{0};
    const Http3MessageHead* head{nullptr};
    Http3FieldSectionFieldView trailer{};
    std::span<const char> body{};
    // Client response final-head and message-end events preserve the same value.
    // Server request events and all other event kinds leave it empty.
    std::optional<HttpResponseBodyPlan> responseBodyPlan{};
};

using Http3ConnectionCallback = void (*)(void*, const Http3ConnectionEvent&);

struct Http3ConnectionLimits final {
    std::size_t maxActiveStreams{128};
    std::size_t maxFieldSectionSize{64 * 1024};
    std::size_t maxFields{256};
    std::size_t maxEncodedFieldSectionBytes{64 * 1024};
};

struct Http3ConnectionResult final {
    Http3ConnectionStatus status{Http3ConnectionStatus::kNeedMoreData};
    Http3ConnectionErrorScope scope{Http3ConnectionErrorScope::kNone};
    Http3ConnectionErrorCode code{Http3ConnectionErrorCode::kNoError};
};

// Sans-I/O receive-side HTTP/3 connection. All connection and live-request
// state uses resource and is released on connection/request termination. The
// resource must outlive this object and every callback. Event views (including
// head and trailer fields) are valid only during the synchronous callback.
// feed() cannot pause: callers must guarantee synchronous consumer capacity
// before feeding bytes; this class deliberately has no unbounded event queue.
// The transport must only deliver bytes/FIN/RESET for QUIC streams that are
// still active; QUIC guarantees that terminated streams receive no further
// delivery. Callbacks must not call feed() recursively, move, or destroy this
// connection. Recursive feed throws std::logic_error without changing the
// outer feed state. Callback/allocator exceptions propagate and latch a
// connection failure: that feed cannot be resumed transactionally. Client
// response streams must be registered before feeding.
class Http3Connection final {
public:
    Http3Connection(Http3PeerRole localRole, std::pmr::memory_resource* resource,
        Http3ConnectionLimits limits = {});
    ~Http3Connection();
    Http3Connection(Http3Connection&&) noexcept;
    Http3Connection& operator=(Http3Connection&&) noexcept;
    Http3Connection(const Http3Connection&) = delete;
    Http3Connection& operator=(const Http3Connection&) = delete;

    [[nodiscard]] Http3ConnectionResult registerClientRequest(std::uint64_t streamId,
        HttpKnownMethod method);
    // Local parser retirement, not a peer RESET and not QUIC cancellation.
    // The transport must first terminate both directions and guarantee no
    // more delivery for this stream. Cannot retire from inside feed callbacks.
    [[nodiscard]] bool retireClientRequest(std::uint64_t streamId) noexcept;
    // Server-role counterpart for an incoming request, including partial HEADERS.
    // The same transport-stop and callback restrictions apply.
    [[nodiscard]] bool retireServerRequest(std::uint64_t streamId) noexcept;
    // Permanently release all protocol storage after transport delivery stops.
    // No callbacks or wire signals are generated. Invalidates peerSettings()
    // references. False when already retired or called from a feed callback.
    [[nodiscard]] bool retire() noexcept;
    [[nodiscard]] Http3ConnectionResult feed(std::uint64_t streamId, std::span<const char> bytes,
        bool fin, bool reset, Http3ConnectionCallback callback, void* context);
    [[nodiscard]] std::size_t activeRequestCount() const noexcept;
    [[nodiscard]] const std::optional<Http3Settings>& peerSettings() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> peerGoawayId() const noexcept;
    // Conservative RFC 9114 unprocessed evidence for a live client request.
    // Query before feeding a RESET or retiring the request. The optional code
    // must come from the peer's RESET_STREAM, never local STOP_SENDING. Any
    // observed response head (including informational) prevents classification
    // as unprocessed. This does not schedule retries or transfer request data.
    [[nodiscard]] bool peerReportsUnprocessed(std::uint64_t streamId,
        std::optional<std::uint64_t> peerResetErrorCode = {}) const noexcept;

private:
    struct Impl;
    std::pmr::memory_resource* resource_;
    Impl* impl_;
};

}  // namespace ruvia
