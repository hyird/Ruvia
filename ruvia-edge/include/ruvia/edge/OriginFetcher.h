#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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
    kConnectFailed,   // resolve or connect failed
    kWriteFailed,     // sending the request failed
    kReadFailed,      // reading the response failed (including premature EOF)
    kProtocolError,   // the origin's response was malformed
    kTooLarge,        // the response exceeded the configured byte ceiling
    kTimeout,         // the connect or an I/O step exceeded its deadline
    kUnsupported,     // a framing the MVP does not handle (upgrade / CONNECT / TLS origin)
};

// A fully materialized origin response: status, the origin's response headers
// verbatim, and the decoded body (de-chunked, exact length). The serve path
// curates the headers (drops hop-by-hop and framing fields, adds Age) when
// replaying it to the client.
struct OriginResponse final {
    std::uint16_t status{0};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct OriginFetchResult final {
    OriginFetchOutcome outcome{OriginFetchOutcome::kConnectFailed};
    OriginResponse response;  // meaningful only when outcome == kOk
};

// Fetches a response from an origin over plaintext HTTP/1.1, driving ruvia-http's
// sans-I/O request writer and response parser with asio socket I/O on the
// caller's executor. Keep-alive origin connections are pooled and reused: a fetch
// takes an idle connection to the same host:port when one is available and
// returns it afterward if the response allows reuse, so back-to-back requests to
// an origin avoid a fresh TCP handshake. A reused connection the origin has since
// closed is detected and the request is retried once on a fresh connection.
// Handles the four response body framings (no-content, exact length, chunked,
// close-delimited); protocol upgrades, CONNECT and TLS origins are unsupported.
//
// Every network step is bounded by a deadline: resolve+connect share the connect
// timeout, and each read/write resets an inactivity timeout, so a slow or hung
// origin ends the fetch with kTimeout instead of stalling the connection.
//
// This type is stateful (it owns the connection pool) and single-threaded: it
// must be used only from the one io_context thread whose executor it is given.
class OriginFetcher final {
public:
    struct Limits final {
        // Upper bound on the decoded response body the edge will hold in memory.
        std::size_t maxResponseBytes{8u * 1024u * 1024u};
        // Deadline for resolving and connecting to the origin.
        std::chrono::milliseconds connectTimeout{5000};
        // Inactivity deadline for each subsequent read/write step.
        std::chrono::milliseconds ioTimeout{30000};
        // How long an idle pooled connection may be reused before it is dropped.
        std::chrono::milliseconds idleTimeout{15000};
        // Maximum idle connections kept per origin host:port.
        std::size_t maxIdlePerHost{8};
    };

    explicit OriginFetcher(Limits limits)
        : limits_(limits), originTlsContext_(asio::ssl::context::tls_client) {
        // MVP: origin certificates are not verified, so a self-signed origin
        // works out of the box. A production edge should verify the upstream.
        originTlsContext_.set_verify_mode(asio::ssl::verify_none);
        originTlsContext_.set_default_verify_paths();
    }

    OriginFetcher(const OriginFetcher&) = delete;
    OriginFetcher& operator=(const OriginFetcher&) = delete;

    // Fetch from host:port. When https, the connection is TLS (with SNI set to
    // host); TLS origin connections are not pooled.
    [[nodiscard]] asio::awaitable<OriginFetchResult> fetch(
        asio::any_io_executor executor,
        std::string_view host,
        std::uint16_t port,
        bool https,
        const OriginRequest& request);

    // Number of idle pooled connections (for observability and tests).
    [[nodiscard]] std::size_t idleConnectionCount() const noexcept;

private:
    struct PooledConnection final {
        asio::ip::tcp::socket socket;
        std::chrono::steady_clock::time_point idleSince;
    };

    Limits limits_;
    asio::ssl::context originTlsContext_;
    std::unordered_map<std::string, std::vector<PooledConnection>> idlePool_;
};

}  // namespace ruvia::edge
