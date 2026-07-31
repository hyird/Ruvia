#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/edge/EdgeTypes.h"

namespace ruvia::edge {

// One origin request the edge issues. All views are borrowed and must outlive
// the fetch. The header list is forwarded verbatim to the origin, so the caller
// must have already stripped hop-by-hop fields and any Host header (the writer
// generates Host and Content-Length itself). `body` is the already-decoded
// request payload for methods that carry one (POST/PUT/...); leave it empty for
// bodyless requests (GET/HEAD/DELETE) so no content framing is sent.
struct OriginRequest final {
    std::string_view method{"GET"};
    std::string_view target{"/"};
    std::span<const HttpHeaderView> headers{};
    std::optional<std::string_view> body{};
};

// How a fetch ended. Only kOk carries a usable response; every other value means
// the edge could not obtain one and should surface a gateway error to the client.
enum class OriginFetchOutcome : std::uint8_t {
    kOk,
    kConnectFailed,  // resolve or connect failed
    kWriteFailed,    // sending the request failed
    kReadFailed,     // reading the response failed (including premature EOF)
    kProtocolError,  // the origin's response was malformed
    kTooLarge,       // the response exceeded the configured byte ceiling
    kTimeout,        // the connect or an I/O step exceeded its deadline
    kUnsupported,    // a framing the MVP does not handle (upgrade / CONNECT)
    kCircuitOpen,    // the origin's breaker is open: not dialed at all
};

// The head of an origin response, delivered to a sink before the body streams.
// contentLength is set for an exact-length body and for valid representation
// metadata on a bodyless response such as HEAD/304. A chunked or close-delimited
// body reports std::nullopt (unknown total). hasBody is false for responses that
// carry none (HEAD, 204, 304).
struct OriginResponseHead final {
    std::uint16_t status{0};
    std::vector<std::pair<std::string, std::string>> headers;
    bool hasBody{false};
    std::optional<std::size_t> contentLength{};
};

// A non-owning destination for a streamed origin response. The concrete
// callbacks live in the calling coroutine frame, so binding performs no heap
// allocation and destruction needs no erased cleanup. Both callbacks are
// asynchronous, preserving origin-to-client backpressure.
class ResponseSink final {
public:
    template <typename HeadCallback, typename BodyCallback>
    ResponseSink(HeadCallback& head, BodyCallback& body) noexcept
        : headTarget_(&head),
          bodyTarget_(&body),
          headInvoke_([](void* target, const OriginResponseHead& value) -> asio::awaitable<bool> {
              return (*static_cast<HeadCallback*>(target))(value);
          }),
          bodyInvoke_([](void* target, std::string_view value) -> asio::awaitable<bool> {
              return (*static_cast<BodyCallback*>(target))(value);
          }) {}

    [[nodiscard]] asio::awaitable<bool> writeHead(const OriginResponseHead& head) const {
        return headInvoke_(headTarget_, head);
    }

    [[nodiscard]] asio::awaitable<bool> writeBody(std::string_view body) const {
        return bodyInvoke_(bodyTarget_, body);
    }

private:
    using HeadInvoke = asio::awaitable<bool> (*)(void*, const OriginResponseHead&);
    using BodyInvoke = asio::awaitable<bool> (*)(void*, std::string_view);

    void* headTarget_;
    void* bodyTarget_;
    HeadInvoke headInvoke_;
    BodyInvoke bodyInvoke_;
};

// The outcome of a streaming fetch. A sink that aborts (returns false) is not an
// error: the origin exchange itself succeeded, so the outcome is kOk and the
// caller (which owns the sink) decides what the abort meant.
struct StreamOutcome final {
    OriginFetchOutcome outcome{OriginFetchOutcome::kConnectFailed};
};

// Fetches a response from an origin over HTTP/1.1, driving ruvia-http's sans-I/O
// request writer and response parser with asio socket/TLS I/O on the caller's
// executor. Keep-alive plaintext connections are pooled and reused: a fetch
// takes an idle connection to the same host:port when one is available and
// returns it afterward if the response allows reuse, so back-to-back requests to
// an origin avoid a fresh TCP handshake. A reused connection the origin has since
// closed is detected and the request is retried once on a fresh connection.
// Handles the four response body framings (no-content, exact length, chunked,
// close-delimited); protocol upgrades and CONNECT are unsupported. TLS origins
// are supported but are not retained in the idle connection pool.
//
// Every network step is bounded by a deadline: resolve+connect share the connect
// timeout, and each read/write resets an inactivity timeout, so a slow or hung
// origin ends the fetch with kTimeout instead of stalling the connection.
//
// This type is stateful (it owns the connection pool) and single-threaded: it
// must be used only from the one io_context thread whose executor it is given.
class OriginFetcher final {
public:
    using Limits = OriginFetchLimits;

    explicit OriginFetcher(Limits limits, std::pmr::memory_resource* resource = nullptr)
        : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
          limits_(limits),
          idlePool_(resource_),
          circuits_(resource_),
          originTlsContext_(asio::ssl::context::tls_client) {
        if (limits_.verifyOriginCertificate) {
            originTlsContext_.set_verify_mode(asio::ssl::verify_peer);
            originTlsContext_.set_default_verify_paths();
        } else {
            originTlsContext_.set_verify_mode(asio::ssl::verify_none);
        }
    }

    OriginFetcher(const OriginFetcher&) = delete;
    OriginFetcher& operator=(const OriginFetcher&) = delete;

    // Fetch from host:port, streaming the response to `sink`. When https, the
    // connection is TLS (with SNI set to host); TLS origin connections are not
    // pooled. The body is never buffered whole here: each decoded chunk is handed
    // to the sink as it arrives. A pooled connection is only reused when the whole
    // response was consumed and the sink did not abort.
    [[nodiscard]] asio::awaitable<StreamOutcome> fetch(asio::any_io_executor executor, std::string_view host, std::uint16_t port, bool https, const OriginRequest& request, ResponseSink& sink);

    // Number of idle pooled connections (for observability and tests).
    [[nodiscard]] std::size_t idleConnectionCount() const noexcept;

    // Requests answered without dialing because a breaker was open. Updated on
    // the Edge worker; read from any thread through EdgeServer::stats().
    [[nodiscard]] std::size_t circuitRejectionCount() const noexcept {
        return circuitRejections_.load(std::memory_order_relaxed);
    }

private:
    struct PooledConnection final {
        asio::ip::tcp::socket socket;
        std::chrono::steady_clock::time_point idleSince;
    };

    // One upstream's breaker. Closed while `failures` is under the threshold;
    // open once it reaches it, until `retryAt` lets a single probe through.
    struct Circuit final {
        std::size_t failures{0};
        std::chrono::steady_clock::time_point retryAt{};
        bool open{false};
        bool probing{false};  // a probe is in flight; hold everyone else back
    };

    // Whether this request may dial the origin, and if so whether it is the
    // single probe that reopens a tripped breaker.
    [[nodiscard]] bool admitToOrigin(std::string_view key) noexcept;
    // Feeds one completed attempt back into the breaker.
    void recordOriginOutcome(std::string_view key, OriginFetchOutcome outcome) noexcept;

    // The dial-and-exchange path, with the breaker already consulted.
    [[nodiscard]] asio::awaitable<StreamOutcome> fetchFromOrigin(asio::any_io_executor executor, std::string_view host, std::uint16_t port, bool https, const OriginRequest& request, ResponseSink& sink, std::string_view key);

    struct TransparentHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };
    struct TransparentEqual final {
        using is_transparent = void;
        template <typename Left, typename Right>
        [[nodiscard]] bool operator()(const Left& left, const Right& right) const noexcept {
            return std::string_view(left) == std::string_view(right);
        }
    };
    using IdleBucket = std::pmr::vector<PooledConnection>;
    using IdlePool = std::pmr::unordered_map<std::pmr::string, IdleBucket, TransparentHash, TransparentEqual>;
    using Circuits = std::pmr::unordered_map<std::pmr::string, Circuit, TransparentHash, TransparentEqual>;

    std::pmr::memory_resource* resource_;
    Limits limits_;
    IdlePool idlePool_;
    Circuits circuits_;
    asio::ssl::context originTlsContext_;
    std::atomic<std::size_t> circuitRejections_{0};
};

}  // namespace ruvia::edge
