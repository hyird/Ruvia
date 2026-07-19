#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

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

// Fetches a single response from an origin over a fresh plaintext HTTP/1.1
// connection, driving ruvia-http's sans-I/O request writer and response parser
// with asio socket I/O on the caller's executor. The connection is not pooled:
// one request, read the whole response, close. Handles the four response body
// framings (no-content, exact length, chunked, close-delimited); protocol
// upgrades, CONNECT and TLS origins are reported as unsupported.
//
// Every network step is bounded by a deadline: resolve+connect share the connect
// timeout, and each read/write resets an inactivity timeout, so a slow or hung
// origin ends the fetch with kTimeout instead of stalling the connection.
class OriginFetcher final {
public:
    struct Limits final {
        // Upper bound on the decoded response body the edge will hold in memory.
        std::size_t maxResponseBytes{8u * 1024u * 1024u};
        // Deadline for resolving and connecting to the origin.
        std::chrono::milliseconds connectTimeout{5000};
        // Inactivity deadline for each subsequent read/write step.
        std::chrono::milliseconds ioTimeout{30000};
    };

    explicit OriginFetcher(Limits limits) noexcept : limits_(limits) {}

    OriginFetcher(const OriginFetcher&) = delete;
    OriginFetcher& operator=(const OriginFetcher&) = delete;

    [[nodiscard]] asio::awaitable<OriginFetchResult> fetch(
        asio::any_io_executor executor,
        std::string_view host,
        std::uint16_t port,
        const OriginRequest& request) const;

private:
    Limits limits_;
};

}  // namespace ruvia::edge
