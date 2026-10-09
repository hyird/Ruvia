#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/HttpResponse.h"

#include "http3/Http3ClientBodyBudget.h"

namespace ruvia::detail {

// Bounded, worker-affine response retention over the sans-I/O HTTP/3 core.
// This is not a production HTTP client: it owns no Tasks, QUIC/UDP transport,
// request writer, retries, or origin/pool policy. The supplied PMR resource must
// be fixed to the owning worker and outlive this engine and all its responses.
struct Http3ClientSansIoResponseLimits final {
    std::size_t maxLiveStreams{32};
    std::size_t maxBodyBytesPerStream{16 * 1024 * 1024};
    std::size_t maxTotalBodyBytes{64 * 1024 * 1024};
    Http3ConnectionConfig connection{};
};

enum class Http3ClientSansIoSessionStatus : std::uint8_t {
    kNeedMoreData,
    kQpackBlocked,
    kPushPromisePending,
    kMessageEnd,
    kReset,
    kStreamError,
    kConnectionError,
    kBodyLimitExceeded,
    kInvalidState,
    kStreamLimitExceeded,
    kTransportError,
    kLocalCancelled,
};
struct Http3ClientSansIoSessionResult final {
    Http3ClientSansIoSessionStatus status{Http3ClientSansIoSessionStatus::kNeedMoreData};
    Http3ConnectionErrorScope scope{Http3ConnectionErrorScope::kNone};
    Http3ConnectionErrorCode code{Http3ConnectionErrorCode::kNoError};
    std::size_t consumedBytes{};
};

// Optional synchronous event sink for an incremental response owner. When set,
// body bytes are not retained by this engine. The sink must own every borrowed
// view it needs before returning, and its caller must ensure capacity for the
// entire feed() input before reading from QUIC; feed() cannot pause midway.
// Only nonterminal informational/final heads, body chunks and trailer fields
// are delivered. The caller must inspect feed()'s successful return before
// committing message end, reset or failure to a consumer. In particular,
// callback work may throw; an EOF delivered from inside the callback could
// not then be withdrawn. The callback must not call registerRequest(), feed(),
// cancelRequest(), release(), or stop() on this engine until feed() returns.
struct Http3ClientResponseEventSink final {
    Http3ConnectionCallback callback{nullptr};
    void* context{nullptr};
};

struct Http3ClientSansIoResponseHeader final {
    std::pmr::string name;
    std::pmr::string value;
    explicit Http3ClientSansIoResponseHeader(std::pmr::memory_resource* resource)
        : name(resource),
          value(resource) {}
};

// Only terminal snapshots are exposed; their views borrow the engine and stay
// valid until release(streamId) or destruction. In event-sink mode body is
// delivered solely through the sink, not retained in this snapshot.
struct Http3ClientSansIoResponseView final {
    std::uint16_t status{0};
    std::optional<HttpResponseBodyPlan> responseBodyPlan{};
    std::span<const Http3ClientSansIoResponseHeader> headers;
    std::span<const Http3ClientSansIoResponseHeader> trailers;
    std::span<const char> body;
    bool complete{false};
    bool reset{false};
    Http3ClientSansIoSessionResult result{};
};

class Http3ClientSansIoSessionEngine final {
public:
    using Limits = Http3ClientSansIoResponseLimits;
    using Result = Http3ClientSansIoSessionResult;

    explicit Http3ClientSansIoSessionEngine(std::pmr::memory_resource* workerResource,
        Limits limits = {});
    // Shared budget also accounts for results held outside this engine. It
    // must outlive the engine; event-sink storage is budgeted by its own owner.
    Http3ClientSansIoSessionEngine(std::pmr::memory_resource* workerResource,
        Http3ClientBodyBudget& bodyBudget, Limits limits = {});
    ~Http3ClientSansIoSessionEngine();
    Http3ClientSansIoSessionEngine(const Http3ClientSansIoSessionEngine&) = delete;
    Http3ClientSansIoSessionEngine& operator=(const Http3ClientSansIoSessionEngine&) = delete;

    [[nodiscard]] Result registerRequest(std::uint64_t streamId, HttpKnownMethod method,
        Http3ClientResponseEventSink sink = {});
    // Feed peer control/QPACK streams as well as registered request streams.
    // A connection-scoped failure requires the QUIC owner to close transport
    // and join all socket Tasks before retiring this engine. In particular,
    // body-budget overflow fails the whole connection until per-stream local
    // receive cancellation and HTTP state retirement exist.
    [[nodiscard]] Result feed(std::uint64_t streamId, std::span<const char> bytes,
        bool fin = false, bool reset = false);
    [[nodiscard]] std::optional<Http3ClientSansIoResponseView> response(
        std::uint64_t streamId) const noexcept;
    // Transfers a successful buffered body's storage exactly once. The
    // returned string retains this engine's fixed worker allocator, whose
    // owner must outlive it. Existing body views are invalidated by transfer;
    // headers/trailers remain valid until release(). The receive reservation
    // is relinquished: a result owner sharing the budget must acquire its
    // reservation before admitting more input. Sink-mode responses have no
    // buffered body to transfer.
    [[nodiscard]] std::optional<std::pmr::string> takeBody(std::uint64_t streamId);
    // Explicitly relinquishes a terminal response and its accounted storage.
    [[nodiscard]] bool release(std::uint64_t streamId) noexcept;
    // Called only after QUIC send/receive termination prevents further stream
    // delivery. This is local abandonment, not a synthetic peer RESET. The
    // terminal response remains owned until release(streamId).
    [[nodiscard]] bool cancelRequest(std::uint64_t streamId) noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    [[nodiscard]] std::size_t maxLiveStreams() const noexcept {
        return limits_.maxLiveStreams;
    }
    [[nodiscard]] std::variant<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeRequestHead(
        std::uint64_t streamId, Http3ClientRequestHeadView view) {
        Http3FieldSectionLimits limits{};
        if (const auto& settings = connection_.peerSettings(); settings && settings->maxFieldSectionSize) {
            limits.maxDecodedBytes = static_cast<std::size_t>(std::min<std::uint64_t>(limits.maxDecodedBytes, *settings->maxFieldSectionSize));
        }
        return connection_.encodeClientRequestHead(streamId, view, limits);
    }
    [[nodiscard]] Http3Settings localSettings() const noexcept {
        return connection_.localSettings();
    }
    [[nodiscard]] bool queuePriorityUpdate(std::uint64_t streamId, HttpPriority priority);
    [[nodiscard]] bool queueMaxPushId(std::uint64_t maximum);
    [[nodiscard]] bool queueCancelPush(std::uint64_t pushId);
    [[nodiscard]] bool queuePushPriorityUpdate(std::uint64_t pushId, HttpPriority priority);
    // Borrowed synchronous push events, including stream association and
    // terminal events. The driver commits terminal results after feed returns.
    void observePushes(Http3ConnectionCallback callback, void* context) noexcept {
        pushObserver_ = {.callback = callback, .context = context};
    }
    // Called after the corresponding QUIC stream can no longer deliver input.
    [[nodiscard]] bool retirePushStream(std::uint64_t streamId);
    void observeOrigins(Http3ConnectionCallback callback, void* context) noexcept {
        originObserver_ = {.callback = callback, .context = context};
    }
    [[nodiscard]] std::span<const char> pendingControlOutput() const noexcept {
        return controlOutput_;
    }
    [[nodiscard]] bool consumeControlOutput(std::size_t bytes) noexcept;
    [[nodiscard]] std::span<const char> pendingEncoderOutput() const noexcept {
        return connection_.pendingQpackEncoderOutput();
    }
    [[nodiscard]] bool consumeEncoderOutput(std::size_t bytes) noexcept {
        return connection_.consumeQpackEncoderOutput(bytes);
    }
    [[nodiscard]] std::span<const char> pendingDecoderOutput() const noexcept {
        return connection_.pendingQpackDecoderOutput();
    }
    [[nodiscard]] bool consumeDecoderOutput(std::size_t bytes) noexcept {
        return connection_.consumeQpackDecoderOutput(bytes);
    }
    [[nodiscard]] std::size_t liveStreamCount() const noexcept;
    [[nodiscard]] std::size_t retainedBodyBytes() const noexcept;
    // The sole connection driver observes peer SETTINGS and GOAWAY before
    // admitting new requests or classifying already-open stream IDs.
    [[nodiscard]] const std::optional<Http3Settings>& peerSettings() const noexcept {
        return connection_.peerSettings();
    }
    [[nodiscard]] bool peerReportsUnprocessed(std::uint64_t streamId,
        std::optional<std::uint64_t> peerResetErrorCode = {}) const noexcept {
        return connection_.peerReportsUnprocessed(streamId, peerResetErrorCode);
    }
    [[nodiscard]] std::optional<std::uint64_t> peerGoawayId() const noexcept {
        return connection_.peerGoawayId();
    }
    // Called after transport shutdown or fatal I/O to wake every incomplete
    // response. This reports a local failure, not an HTTP/3 peer error code.
    [[nodiscard]] Result stop() noexcept;

private:
    struct StoredResponse final {
        explicit StoredResponse(std::pmr::memory_resource* resource)
            : headers(resource),
              trailers(resource),
              body(resource) {}
        std::pmr::vector<Http3ClientSansIoResponseHeader> headers;
        std::pmr::vector<Http3ClientSansIoResponseHeader> trailers;
        std::pmr::string body;
        std::uint16_t status{0};
        std::optional<HttpResponseBodyPlan> responseBodyPlan{};
        Result result{};
        bool complete{false};
        bool reset{false};
        bool finalHeadSeen{false};
        bool limitExceeded{false};
        bool terminal{false};
        bool bodyTransferred{false};
        Http3ClientResponseEventSink sink{};
    };
    static void onEvent(void* context, const Http3ConnectionEvent& event);
    [[nodiscard]] Result fromConnection(Http3ConnectionResult result) const noexcept;
    void failConnection(Result failure) noexcept;

    std::pmr::memory_resource* resource_;
    Limits limits_;
    Http3ClientBodyBudget localBodyBudget_;
    Http3ClientBodyBudget* bodyBudget_;
    Http3Connection connection_;
    std::pmr::string controlOutput_;
    Http3ClientResponseEventSink originObserver_{};
    Http3ClientResponseEventSink pushObserver_{};
    std::pmr::unordered_map<std::uint64_t, StoredResponse> responses_;
    std::size_t retainedBodyBytes_{0};
    bool feeding_{};
    Result connectionFailure_{};
};

}  // namespace ruvia::detail
