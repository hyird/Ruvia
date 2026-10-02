#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3ConnectionError.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3MessageBody.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3ResponseWriter.h"
#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/HttpClientResponseHead.h"
#include "ruvia/http/HttpConnectionAdvertisement.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpPriority.h"
#include "ruvia/http/HttpPush.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class Http3ConnectionStatus : std::uint8_t {
    kNeedMoreData,
    kQpackBlocked,
    kPushPromisePending,
    kMessageEnd,
    kStreamError,
    kConnectionError,
    kReset,
};

enum class Http3ConnectionEventKind : std::uint8_t {
    kPushPromise,
    // A received push stream has decoded its Push ID. This can precede its
    // promise; streamId is the physical server-initiated unidirectional stream.
    kPushStream,
    kPushCanceled,
    kPriorityUpdate,
    kOriginAdvertisement,
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
    std::optional<std::uint64_t> pushId{};
    std::optional<HttpPriorityUpdate> priorityUpdate{};
    const HttpOriginAdvertisement* originAdvertisement{nullptr};
    std::optional<HttpClientRequestContentSignal> requestContentSignal{};
};

using Http3ConnectionCallback = void (*)(void*, const Http3ConnectionEvent&);

struct Http3ConnectionConfig final {
    std::size_t maxActiveStreams{128};
    // Peer unidirectional streams include control/QPACK and are independent
    // of the request/push concurrency budget.
    std::size_t maxPeerUnidirectionalStreams{128};
    std::size_t maxFieldSectionSize{64 * 1024};
    std::size_t maxFields{256};
    std::size_t maxEncodedFieldSectionBytes{64 * 1024};
    // Advertise the same values in local SETTINGS. Zero disables dynamic QPACK.
    std::size_t qpackMaxTableCapacity{0};
    std::size_t qpackBlockedStreams{0};
    // Match local SETTINGS_ENABLE_CONNECT_PROTOCOL. Ordinary CONNECT is independent.
    bool enableConnectProtocol{false};
    bool enableDatagrams{false};
    // Enable only for a TLS-authenticated origin connection, not an explicit proxy.
    bool receiveOriginAdvertisements{false};
    // Client role: authorize pushes up to this ID; advertise MAX_PUSH_ID on control.
    std::optional<std::uint64_t> maxPushId{};
    std::size_t maxRememberedPushes{128};
};

struct Http3ConnectionResult final {
    Http3ConnectionStatus status{Http3ConnectionStatus::kNeedMoreData};
    Http3ConnectionErrorScope scope{Http3ConnectionErrorScope::kNone};
    Http3ConnectionErrorCode code{Http3ConnectionErrorCode::kNoError};
    std::size_t consumedBytes{0};
};

// Sans-I/O receive-side HTTP/3 connection. All connection and live-request
// state uses resource and is released on connection/request termination. The
// resource must outlive this object and every callback. Event views (including
// head and trailer fields) are valid only during the synchronous callback.
// Callers must guarantee synchronous consumer capacity
// before feeding bytes; this class deliberately has no unbounded event queue.
// kQpackBlocked consumes only consumedBytes. Keep the remaining bytes and FIN,
// deliver peer encoder-stream bytes, then feed the suffix on the blocked stream.
// The completed HEADERS section is retained until it can be decoded. Other
// streams remain independently usable. This also applies to an empty suffix.
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
        Http3ConnectionConfig limits = {});
    ~Http3Connection();
    Http3Connection(Http3Connection&&) noexcept;
    Http3Connection& operator=(Http3Connection&&) noexcept;
    Http3Connection(const Http3Connection&) = delete;
    Http3Connection& operator=(const Http3Connection&) = delete;

    [[nodiscard]] Http3ConnectionResult registerClientRequest(std::uint64_t streamId,
        HttpKnownMethod method);
    // Local stream cancellation: emit QPACK Stream Cancellation before releasing
    // a live request/push parser. The driver separately terminates QUIC delivery.
    [[nodiscard]] Http3ConnectionResult cancelRequest(std::uint64_t streamId);
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
    // Server: creates a complete PUSH_PROMISE frame; caller transmits it on the
    // associated request stream. The returned bytes use this connection's resource.
    // The associated response's send half must still be open; the driver owns
    // send-side HEADERS/DATA/FIN progression through the message write plans.
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> preparePushPromise(
        std::uint64_t associatedStreamId, std::uint64_t pushId, HttpPushRequestView request);
    // Complete control frames, excluding the control stream type and SETTINGS.
    // Queue the returned bytes in order. Protocol state commits at preparation.
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> prepareMaxPushId(std::uint64_t maximum);
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> prepareCancelPush(std::uint64_t pushId);
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> prepareGoaway(std::uint64_t firstUnprocessedId);
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> preparePriorityUpdate(HttpPriorityUpdate update);
    // Server: stream type + push ID. One server unidirectional stream per push.
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> preparePushStream(std::uint64_t streamId, std::uint64_t pushId);
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> prepareOriginAdvertisement(
        std::span<const std::string_view> origins);
    [[nodiscard]] std::optional<std::uint64_t> peerMaxPushId() const noexcept;
    // Validated promise metadata, borrowed until connection retirement. Available
    // to either role after preparing or receiving the complete PUSH_PROMISE.
    [[nodiscard]] const Http3MessageHead* promisedRequest(std::uint64_t pushId) const& noexcept;
    const Http3MessageHead* promisedRequest(std::uint64_t) const&& = delete;

    // Uses peer SETTINGS to encode a QPACK section for any local message or
    // promise. Before SETTINGS, only static/literal representations are used.
    // Message helpers validate HTTP semantics; this entry point owns compression.
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3QpackConnectionError> encodeFieldSection(
        std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields);
    [[nodiscard]] std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeClientRequestHead(
        std::uint64_t streamId, Http3ClientRequestHeadView view, Http3FieldSectionLimits limits = {});
    [[nodiscard]] std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeConnectResponseHead(std::uint64_t streamId,
        const HttpResponse& response, Http3FieldSectionLimits limits = {});
    [[nodiscard]] std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeResponseHead(std::uint64_t streamId,
        const HttpResponse& response, HttpBufferedResponseWritePlan plan, Http3FieldSectionLimits limits = {});
    [[nodiscard]] std::expected<Http3StreamingResponseHead, Http3ResponseHeadFailure> encodeStreamingResponseHead(std::uint64_t streamId,
        HttpResponse response, HttpKnownMethod method, ResponseStreamKind kind, ResponseTrailerIntent trailers, Http3FieldSectionLimits limits = {});
    [[nodiscard]] std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeInterimResponseHead(std::uint64_t streamId,
        const HttpInterimResponseHead& response, Http3FieldSectionLimits limits = {});
    [[nodiscard]] std::expected<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeResponseTrailers(std::uint64_t streamId,
        std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits = {});
    [[nodiscard]] std::span<const char> pendingQpackEncoderOutput() const& noexcept;
    std::span<const char> pendingQpackEncoderOutput() const&& = delete;
    [[nodiscard]] bool consumeQpackEncoderOutput(std::size_t bytes) noexcept;

    // Decoder instructions (without stream-type prefix) belong on the local
    // QPACK decoder critical stream. Consume only successfully transmitted bytes.
    [[nodiscard]] std::span<const char> pendingQpackDecoderOutput() const& noexcept;
    std::span<const char> pendingQpackDecoderOutput() const&& = delete;
    [[nodiscard]] bool consumeQpackDecoderOutput(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t activeRequestCount() const noexcept;
    // Use these exact settings to construct Http3LocalCriticalStreams. This
    // binds advertised receive capabilities to their connection-owned state.
    [[nodiscard]] Http3Settings localSettings() const noexcept;
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
