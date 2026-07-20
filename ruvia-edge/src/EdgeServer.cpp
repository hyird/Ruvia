#include "ruvia/edge/EdgeServer.h"

#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/edge/EdgeFreshness.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"

namespace ruvia::edge {

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

// Upper bound on a whole buffered client request (head plus any forwarded body).
constexpr std::size_t kMaxRequestBytes = 1u * 1024u * 1024u;

// How long a persistent client connection may sit idle awaiting its next request.
constexpr std::chrono::seconds kKeepAliveIdleTimeout{60};

[[nodiscard]] char toLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(a[i]) != toLowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

// Fields a proxy must not forward (RFC 9110 section 7.6.1), plus the framing
// fields the edge regenerates itself. Age is handled separately: it is dropped
// only when the edge emits its own computed value.
[[nodiscard]] bool isConnectionOrFramingField(std::string_view lowerName) noexcept {
    return lowerName == "connection" || lowerName == "keep-alive" ||
        lowerName == "proxy-authenticate" || lowerName == "proxy-authorization" ||
        lowerName == "te" || lowerName == "trailer" ||
        lowerName == "transfer-encoding" || lowerName == "upgrade" ||
        lowerName == "content-length";
}

void appendDecimal(std::string& out, std::uint64_t value) {
    std::array<char, 20> digits;
    const auto [end, ec] =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    (void)ec;
    out.append(digits.data(), static_cast<std::size_t>(end - digits.data()));
}

// Whether to keep the connection open after this request (RFC 9112 section 9.3):
// HTTP/1.1 persists unless Connection: close; HTTP/1.0 closes unless
// Connection: keep-alive.
[[nodiscard]] bool clientWantsKeepAlive(const HttpRequest& request) {
    const bool http11 = request.protocolVersion() == HttpProtocolVersion::kHttp11;
    const auto connection = request.header("connection");
    if (!connection) {
        return http11;
    }
    std::string lower(*connection);
    for (auto& c : lower) {
        c = toLowerAscii(c);
    }
    if (lower.find("close") != std::string::npos) {
        return false;
    }
    if (lower.find("keep-alive") != std::string::npos) {
        return true;
    }
    return http11;
}

// Serialize a response for the client: status line, curated headers, a fresh
// Content-Length, a Connection header reflecting keep-alive, an X-Cache marker,
// and -- when the edge computes its own age for a cache hit -- an Age header
// (dropping any inherited one).
//
// omitBody serves a HEAD response: no message body is appended, and the resource
// length is reported by keeping the origin's Content-Length when present, else
// computing it from `body` (which for HEAD is the full representation used only
// for its size). Transfer-Encoding is still dropped in both modes.
[[nodiscard]] std::string buildResponseWire(
    std::uint16_t status,
    const Headers& headers,
    std::string_view body,
    std::string_view xCache,
    std::optional<std::uint64_t> ageOverride,
    bool omitBody = false,
    bool keepAlive = false) {
    std::string out;
    out.reserve((omitBody ? 0 : body.size()) + 256);

    out.append("HTTP/1.1 ");
    appendDecimal(out, status);
    out.push_back(' ');
    if (const auto code = HttpStatusCode::tryFromValue(status)) {
        out.append(httpReasonPhrase(*code));
    }
    out.append("\r\n");

    bool keptContentLength = false;
    for (const auto& [name, value] : headers) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        if (isConnectionOrFramingField(lower)) {
            // For a HEAD response keep the origin's Content-Length; otherwise the
            // edge emits its own from the body length.
            if (omitBody && lower == "content-length") {
                keptContentLength = true;
            } else {
                continue;
            }
        }
        if (ageOverride && lower == "age") {
            continue;
        }
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    if (!omitBody || !keptContentLength) {
        out.append("Content-Length: ");
        appendDecimal(out, body.size());
        out.append("\r\n");
    }
    out.append(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    out.append("X-Cache: ");
    out.append(xCache);
    out.append("\r\n");
    if (ageOverride) {
        out.append("Age: ");
        appendDecimal(out, *ageOverride);
        out.append("\r\n");
    }
    out.append("\r\n");
    if (!omitBody) {
        out.append(body);
    }
    return out;
}

// A minimal status-only response for edge-generated errors.
[[nodiscard]] std::string buildStatusWire(std::uint16_t status) {
    return buildResponseWire(status, Headers{}, {}, "MISS", std::nullopt);
}

// How the edge frames a streamed response body to the client.
enum class ClientFraming : std::uint8_t {
    kNoBody,   // no message body (HEAD keeps the origin Content-Length; 204/304 none)
    kLength,   // exact Content-Length, streamed straight through
    kChunked,  // unknown length re-encoded as Transfer-Encoding: chunked
};

// Wrap one body chunk in HTTP/1 chunked framing.
[[nodiscard]] std::string chunkFrame(std::string_view chunk) {
    std::string out;
    std::array<char, 16> hex;
    const auto [end, ec] =
        std::to_chars(hex.data(), hex.data() + hex.size(), chunk.size(), 16);
    (void)ec;
    out.append(hex.data(), static_cast<std::size_t>(end - hex.data()));
    out.append("\r\n");
    out.append(chunk);
    out.append("\r\n");
    return out;
}

// Serialize just the response head for a streamed response: status line, curated
// headers, the chosen framing header, Connection and X-Cache. No body follows;
// the caller streams it (raw for kLength, chunk-framed for kChunked).
[[nodiscard]] std::string buildStreamingHead(
    std::uint16_t status,
    const Headers& headers,
    std::string_view xCache,
    ClientFraming framing,
    std::size_t contentLength,
    bool keepAlive) {
    std::string out;
    out.append("HTTP/1.1 ");
    appendDecimal(out, status);
    out.push_back(' ');
    if (const auto code = HttpStatusCode::tryFromValue(status)) {
        out.append(httpReasonPhrase(*code));
    }
    out.append("\r\n");

    for (const auto& [name, value] : headers) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        if (isConnectionOrFramingField(lower)) {
            // For a bodyless response keep the origin's Content-Length (HEAD);
            // otherwise the edge emits its own framing below.
            if (framing == ClientFraming::kNoBody && lower == "content-length") {
                // keep it
            } else {
                continue;
            }
        }
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    if (framing == ClientFraming::kLength) {
        out.append("Content-Length: ");
        appendDecimal(out, contentLength);
        out.append("\r\n");
    } else if (framing == ClientFraming::kChunked) {
        out.append("Transfer-Encoding: chunked\r\n");
    }
    out.append(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    out.append("X-Cache: ");
    out.append(xCache);
    out.append("\r\n");
    out.append("\r\n");
    return out;
}

// A plain-text response for the management API.
[[nodiscard]] std::string buildAdminResponse(std::uint16_t status, std::string_view body) {
    std::string out;
    out.append("HTTP/1.1 ");
    appendDecimal(out, status);
    out.push_back(' ');
    if (const auto code = HttpStatusCode::tryFromValue(status)) {
        out.append(httpReasonPhrase(*code));
    }
    out.append("\r\nContent-Type: text/plain\r\nContent-Length: ");
    appendDecimal(out, body.size());
    out.append("\r\nConnection: close\r\n\r\n");
    out.append(body);
    return out;
}

[[nodiscard]] std::optional<std::string_view> findHeaderValue(
    const Headers& headers,
    std::string_view name) {
    for (const auto& [n, v] : headers) {
        if (iequals(n, name)) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

// Update a stored response's headers with those from a 304 (RFC 9111 section
// 4.3.4): keep the stored fields, but replace any also present in the 304, and
// ignore the 304's connection/framing fields.
[[nodiscard]] Headers mergeStoredHeaders(const Headers& stored, const Headers& updates) {
    Headers merged = stored;
    for (const auto& [name, value] : updates) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        if (isConnectionOrFramingField(lower)) {
            continue;
        }
        std::erase_if(merged, [&](const auto& field) { return iequals(field.first, name); });
        merged.emplace_back(name, value);
    }
    return merged;
}

// Assemble the RFC 9111 freshness inputs from a response's status and headers.
[[nodiscard]] FreshnessInput buildFreshnessInput(
    std::uint16_t status,
    const Headers& headers,
    std::time_t now) {
    FreshnessInput input;
    input.status = status;
    input.now = now;

    CacheControlFieldParser cacheControl;
    for (const auto& [name, value] : headers) {
        if (iequals(name, "cache-control")) {
            cacheControl.update(value);
        } else if (iequals(name, "date")) {
            input.dateHeader = parseHttpDate(value);
        } else if (iequals(name, "expires")) {
            input.expiresHeader = parseHttpDate(value);
        } else if (iequals(name, "age")) {
            std::uint64_t age = 0;
            const char* begin = value.data();
            const char* end = begin + value.size();
            if (std::from_chars(begin, end, age).ec == std::errc{}) {
                input.ageHeader = age;
            }
        }
    }
    input.cacheControl = cacheControl.finish();
    return input;
}

}  // namespace

EdgeServer::EdgeServer(const asio::ip::tcp::endpoint& endpoint, EdgeServerOptions options)
    : acceptor_(ioContext_, endpoint),
      cache_(options.cache),
      fetcher_(options.fetch),
      maxCacheableBytes_(options.maxCacheableBytes) {
    if (options.tls) {
        tlsContext_.emplace(asio::ssl::context::tls_server);
        tlsContext_->use_certificate_chain(
            asio::buffer(options.tls->certificateChainPem));
        tlsContext_->use_private_key(
            asio::buffer(options.tls->privateKeyPem), asio::ssl::context::pem);
    }
    if (options.adminEndpoint) {
        adminAcceptor_.emplace(ioContext_, *options.adminEndpoint);
    }
}

EdgeServer::~EdgeServer() {
    ioContext_.stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void EdgeServer::start() {
    asio::co_spawn(ioContext_, acceptLoop(), asio::detached);
    if (adminAcceptor_) {
        asio::co_spawn(ioContext_, adminAcceptLoop(), asio::detached);
    }
    worker_ = std::jthread([this] { ioContext_.run(); });
}

void EdgeServer::stop() {
    ioContext_.stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void EdgeServer::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

asio::ip::tcp::endpoint EdgeServer::localEndpoint() const {
    return acceptor_.local_endpoint();
}

std::optional<asio::ip::tcp::endpoint> EdgeServer::localAdminEndpoint() const {
    if (adminAcceptor_) {
        return adminAcceptor_->local_endpoint();
    }
    return std::nullopt;
}

bool EdgeServer::addOrigin(std::string frontHost, OriginSettings settings) {
    return config_.addOrigin(std::move(frontHost), std::move(settings));
}

bool EdgeServer::removeOrigin(std::string_view frontHost) {
    return config_.removeOrigin(frontHost);
}

bool EdgeServer::purge(std::string_view frontHost, std::string_view target) {
    return cache_.purge(cacheKey("GET", frontHost, target));
}

void EdgeServer::clearCache() {
    cache_.clear();
}

std::string EdgeServer::cacheKey(
    std::string_view method,
    std::string_view frontHost,
    std::string_view target) {
    std::string key;
    key.reserve(method.size() + frontHost.size() + target.size() + 2);
    key.append(method);
    key.push_back('\n');
    key.append(frontHost);
    key.push_back('\n');
    key.append(target);
    return key;
}

asio::awaitable<void> EdgeServer::acceptLoop() {
    for (;;) {
        auto [ec, socket] =
            co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                break;
            }
            continue;
        }
        if (tlsContext_) {
            asio::co_spawn(
                ioContext_, handleTlsSession(std::move(socket)), asio::detached);
        } else {
            asio::co_spawn(
                ioContext_, handleSession(std::move(socket)), asio::detached);
        }
    }
}

asio::awaitable<void> EdgeServer::handleTlsSession(asio::ip::tcp::socket socket) {
    asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), *tlsContext_);
    auto [ec] = co_await stream.async_handshake(
        asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
    if (ec) {
        asio::error_code ignore;
        stream.lowest_layer().close(ignore);
        co_return;
    }
    co_await handleSession(std::move(stream));
}

template <typename Stream>
asio::awaitable<void> EdgeServer::handleSession(Stream stream) {
    using namespace asio::experimental::awaitable_operators;

    std::string clientAddress;
    {
        asio::error_code ec;
        const auto remote = stream.lowest_layer().remote_endpoint(ec);
        if (!ec) {
            clientAddress = remote.address().to_string();
        }
    }

    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const auto writeStatus = [&stream, tuple](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(stream, asio::buffer(wire.data(), wire.size()), tuple);
    };

    std::string inbound;
    std::array<char, 8192> buffer;
    const Http1RequestParser parser;
    asio::steady_timer idleTimer(ioContext_);

    // Serve requests on this connection until one closes it, the client goes
    // away, or the connection sits idle past the keep-alive timeout.
    bool keepGoing = true;
    while (keepGoing) {
        std::size_t consumed = 0;
        bool framed = false;
        for (;;) {
            auto parseResult = parser.parse(inbound);
            if (parseResult.failure() != nullptr) {
                co_await writeStatus(buildStatusWire(400));
                keepGoing = false;
                break;
            }
            if (const auto* parsed = parseResult.parsed(); parsed != nullptr) {
                consumed = parsed->consumedBytes();
                const bool keepAlive = clientWantsKeepAlive(parsed->request());
                keepGoing = co_await handleFramedRequest(
                    stream, *parsed, clientAddress, keepAlive);
                framed = true;
                break;
            }
            if (inbound.size() > kMaxRequestBytes) {
                co_await writeStatus(buildStatusWire(413));
                keepGoing = false;
                break;
            }
            idleTimer.expires_after(kKeepAliveIdleTimeout);
            auto raced = co_await (
                stream.async_read_some(asio::buffer(buffer), tuple) ||
                idleTimer.async_wait(tuple));
            if (raced.index() == 1) {
                keepGoing = false;  // idle too long
                break;
            }
            auto& [ec, n] = std::get<0>(raced);
            if (n > 0) {
                inbound.append(buffer.data(), n);
            }
            if (ec) {
                keepGoing = false;  // client closed or read error
                break;
            }
        }
        if (framed) {
            inbound.erase(0, consumed);  // keep any pipelined bytes for the next request
        }
    }

    asio::error_code ignore;
    stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
}

template <typename Stream>
asio::awaitable<bool> EdgeServer::handleFramedRequest(
    Stream& stream,
    const Http1ParsedRequest& parsed,
    std::string_view clientAddress,
    bool keepAlive) {
    const auto writeAll = [&stream](std::string wire) -> asio::awaitable<bool> {
        auto [ec, n] = co_await asio::async_write(
            stream, asio::buffer(wire.data(), wire.size()),
            asio::as_tuple(asio::use_awaitable));
        (void)n;
        co_return !ec;
    };

    // GET and HEAD take the cache path; other methods are proxied (pass-through).
    const auto& request = parsed.request();
    const bool isGet = request.knownMethod() == HttpKnownMethod::kGet;
    const bool isHead = request.knownMethod() == HttpKnownMethod::kHead;

    const std::string_view frontHost = request.header("host").value_or("");
        const std::string_view target = request.target();

        // 3. Resolve the origin from the current published config snapshot.
        const auto snapshot = config_.snapshot();
        const OriginSettings* origin = snapshot->findOrigin(frontHost);
        if (origin == nullptr) {
            co_await writeAll(buildStatusWire(502));
            co_return false;
        }

        // Non-GET/HEAD methods bypass the cache: forward the request and its body
        // to the origin, return the response, and on a successful unsafe method
        // invalidate any cached GET for this target (RFC 9111 section 4.4).
        if (!isGet && !isHead) {
            std::optional<std::string_view> requestBody;
            std::string decodedBody;
            const auto& bodyPlan = parsed.bodyPlan();
            if (bodyPlan.knownLength() != nullptr) {
                requestBody = parsed.wireBody();
            } else if (bodyPlan.chunked() != nullptr) {
                // De-chunk the request body so it can be forwarded with a
                // Content-Length; the whole message is already buffered.
                ruvia::detail::Http1ChunkedBodyDecoder decoder(
                    ProtocolByteLimit::limited(kMaxRequestBytes));
                std::string chunkBuffer(parsed.wireBody());
                bool decodeOk = true;
                for (;;) {
                    const auto decoded = decoder.decode(chunkBuffer);
                    if (decoded.failure() != nullptr) {
                        decodeOk = false;
                        break;
                    }
                    if (const auto* chunk = decoded.bodyChunk()) {
                        decodedBody.append(chunk->bytes());
                        chunkBuffer.erase(0, decoded.consumedBytes());
                        continue;
                    }
                    if (decoded.complete() != nullptr) {
                        break;
                    }
                    decodeOk = false;  // need-more is impossible: message is complete
                    break;
                }
                if (!decodeOk) {
                    co_await writeAll(buildStatusWire(400));
                    co_return false;
                }
                requestBody = decodedBody;
            }

            std::vector<HttpHeaderView> passHeaders;
            for (const auto& field : request.headers()) {
                std::string lower;
                lower.reserve(field.name().size());
                for (const char c : field.name()) {
                    lower.push_back(toLowerAscii(c));
                }
                if (isConnectionOrFramingField(lower) || lower == "host" ||
                    lower == "via" || lower == "forwarded" ||
                    lower.starts_with("x-forwarded-")) {
                    continue;
                }
                passHeaders.push_back(field);
            }
            if (!clientAddress.empty()) {
                passHeaders.emplace_back(
                    std::string_view("X-Forwarded-For"), std::string_view(clientAddress));
            }
            if (!frontHost.empty()) {
                passHeaders.emplace_back(std::string_view("X-Forwarded-Host"), frontHost);
            }
            passHeaders.emplace_back(
                std::string_view("X-Forwarded-Proto"), std::string_view("http"));
            passHeaders.emplace_back(
                std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

            OriginRequest passRequest;
            passRequest.method = request.method();
            passRequest.target = target;
            passRequest.headers = passHeaders;
            passRequest.body = requestBody;

            // Stream the origin response straight through to the client (never
            // cached). The response body is re-framed as chunked when its length
            // is unknown.
            std::uint16_t passStatus = 0;
            bool passHeadSent = false;
            bool passChunked = false;
            bool passAborted = false;
            ResponseSink passSink;
            passSink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
                passStatus = head.status;
                ClientFraming framing = ClientFraming::kNoBody;
                if (head.hasBody) {
                    framing = head.contentLength ? ClientFraming::kLength : ClientFraming::kChunked;
                }
                passChunked = framing == ClientFraming::kChunked;
                if (!co_await writeAll(buildStreamingHead(
                        head.status, head.headers, "BYPASS", framing,
                        head.contentLength.value_or(0), keepAlive))) {
                    passAborted = true;
                    co_return false;
                }
                passHeadSent = true;
                co_return true;
            };
            passSink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
                const bool ok = passChunked ? co_await writeAll(chunkFrame(chunk))
                                            : co_await writeAll(std::string(chunk));
                if (!ok) {
                    passAborted = true;
                    co_return false;
                }
                co_return true;
            };

            auto passStream = co_await fetcher_.fetch(
                ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
                origin->https, passRequest, passSink);
            if (passAborted) {
                co_return false;
            }
            if (passStream.outcome != OriginFetchOutcome::kOk) {
                if (passHeadSent) {
                    co_return false;  // partial response already sent
                }
                const std::uint16_t gatewayStatus =
                    passStream.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
                co_await writeAll(buildStatusWire(gatewayStatus));
                co_return false;
            }
            if (passChunked && !co_await writeAll("0\r\n\r\n")) {
                co_return false;
            }
            // A successful unsafe method invalidates the cached GET for this URI.
            if (passStatus < 400) {
                cache_.purge(cacheKey("GET", frontHost, target));
            }
            co_return keepAlive;
        }

        const std::time_t now = std::time(nullptr);
        const std::string key = cacheKey("GET", frontHost, target);

        // 4. Serve a fresh cache hit without touching the origin.
        auto hit = cache_.lookup(key, now);
        if (hit.status == CacheLookupStatus::kFresh) {
            const auto& entry = *hit.entry;
            const auto age = entry.storedAt <= now
                ? static_cast<std::uint64_t>(now - entry.storedAt)
                : std::uint64_t{0};
            co_return co_await writeAll(buildResponseWire(
                entry.status, entry.headers, entry.body, "HIT", age, isHead,
                keepAlive)) && keepAlive;
        }
        // A stale entry may still be revalidated with the origin below.
        const std::shared_ptr<const CachedResponse> staleEntry =
            hit.status == CacheLookupStatus::kStale ? hit.entry : nullptr;

        // 5. Miss (or stale): fetch from the origin. Forward the client's request
        // headers minus hop-by-hop fields, Host (regenerated for the upstream),
        // and fields that would break MVP caching -- Accept-Encoding is dropped so
        // the origin sends identity (no Vary handling yet), and Range plus client
        // conditionals are dropped. Client-supplied forwarding headers are dropped
        // and replaced so a client cannot spoof them.
        std::vector<HttpHeaderView> forwardHeaders;
        for (const auto& field : request.headers()) {
            std::string lower;
            lower.reserve(field.name().size());
            for (const char c : field.name()) {
                lower.push_back(toLowerAscii(c));
            }
            if (isConnectionOrFramingField(lower) || lower == "host" ||
                lower == "accept-encoding" || lower == "range" ||
                lower == "if-none-match" || lower == "if-modified-since" ||
                lower == "if-match" || lower == "if-unmodified-since" ||
                lower == "if-range" || lower == "via" || lower == "forwarded" ||
                lower.starts_with("x-forwarded-")) {
                continue;
            }
            forwardHeaders.push_back(field);
        }
        if (!clientAddress.empty()) {
            forwardHeaders.emplace_back(
                std::string_view("X-Forwarded-For"), std::string_view(clientAddress));
        }
        if (!frontHost.empty()) {
            forwardHeaders.emplace_back(std::string_view("X-Forwarded-Host"), frontHost);
        }
        forwardHeaders.emplace_back(
            std::string_view("X-Forwarded-Proto"), std::string_view("http"));
        forwardHeaders.emplace_back(
            std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

        // Revalidate a stale entry with a conditional request when it carries a
        // validator, so an unchanged resource comes back as a bodyless 304.
        if (staleEntry) {
            if (const auto etag = findHeaderValue(staleEntry->headers, "etag")) {
                forwardHeaders.emplace_back(std::string_view("If-None-Match"), *etag);
            } else if (const auto lastModified =
                           findHeaderValue(staleEntry->headers, "last-modified")) {
                forwardHeaders.emplace_back(
                    std::string_view("If-Modified-Since"), *lastModified);
            }
        }

        OriginRequest originRequest;
        originRequest.method = request.method();  // GET or HEAD
        originRequest.target = target;
        originRequest.headers = forwardHeaders;

        // stale-if-error: a stale copy within its stale-if-error window is served
        // when the origin cannot be reached (or answers 5xx), instead of an error.
        const auto serveStaleOnError = [&]() -> bool {
            return staleEntry != nullptr && staleEntry->staleIfError > 0 &&
                now <= staleEntry->expiresAt +
                           static_cast<std::time_t>(staleEntry->staleIfError);
        };
        const auto writeStale = [&]() -> asio::awaitable<bool> {
            const auto age = staleEntry->storedAt <= now
                ? static_cast<std::uint64_t>(now - staleEntry->storedAt)
                : std::uint64_t{0};
            co_return co_await writeAll(buildResponseWire(
                staleEntry->status, staleEntry->headers, staleEntry->body, "STALE", age,
                isHead, keepAlive));
        };

        // Streaming sink: writes the client head then each body chunk as the
        // origin responds, and tees a cacheable body into cacheBuffer. A 304
        // (revalidation) or a stale-if-error-covered 5xx declines streaming so the
        // stored body is served after the fetch instead.
        std::uint16_t respStatus = 0;
        Headers respHeaders;
        bool headSent = false;
        bool clientChunked = false;
        bool clientAborted = false;
        bool caching = false;
        std::string cacheBuffer;
        FreshnessDecision cacheDecision;

        ResponseSink sink;
        sink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
            respStatus = head.status;
            respHeaders = head.headers;
            if (staleEntry != nullptr && head.status == 304) {
                co_return false;  // revalidation: serve the stored body below
            }
            if (head.status >= 500 && serveStaleOnError()) {
                co_return false;  // stale-if-error: serve the stored body below
            }
            ClientFraming framing = ClientFraming::kNoBody;
            if (head.hasBody) {
                framing = head.contentLength ? ClientFraming::kLength : ClientFraming::kChunked;
            }
            clientChunked = framing == ClientFraming::kChunked;
            if (!co_await writeAll(buildStreamingHead(
                    head.status, head.headers, "MISS", framing,
                    head.contentLength.value_or(0), keepAlive))) {
                clientAborted = true;
                co_return false;
            }
            headSent = true;
            if (!isHead) {
                cacheDecision =
                    evaluateFreshness(buildFreshnessInput(head.status, head.headers, now));
                caching = cacheDecision.cacheable;
            }
            co_return true;
        };
        sink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
            if (caching) {
                if (cacheBuffer.size() + chunk.size() > maxCacheableBytes_) {
                    caching = false;  // too big to cache; keep streaming
                    cacheBuffer.clear();
                    cacheBuffer.shrink_to_fit();
                } else {
                    cacheBuffer.append(chunk);
                }
            }
            const bool ok = clientChunked ? co_await writeAll(chunkFrame(chunk))
                                          : co_await writeAll(std::string(chunk));
            if (!ok) {
                clientAborted = true;
                co_return false;
            }
            co_return true;
        };

        auto fetchResult = co_await fetcher_.fetch(
            ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
            origin->https, originRequest, sink);

        if (clientAborted) {
            co_return false;  // the client went away mid-response
        }
        if (fetchResult.outcome != OriginFetchOutcome::kOk) {
            if (headSent) {
                co_return false;  // partial response already sent; close
            }
            if (serveStaleOnError()) {
                co_return co_await writeStale() && keepAlive;
            }
            const std::uint16_t gatewayStatus =
                fetchResult.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
            co_await writeAll(buildStatusWire(gatewayStatus));
            co_return false;
        }

        // The sink declined to stream (304 revalidation, or a stale-if-error 5xx):
        // serve the stored body instead.
        if (!headSent && staleEntry != nullptr) {
            if (respStatus == 304) {
                Headers merged = mergeStoredHeaders(staleEntry->headers, respHeaders);
                const auto decision =
                    evaluateFreshness(buildFreshnessInput(staleEntry->status, merged, now));
                auto refreshed = std::make_shared<CachedResponse>();
                refreshed->status = staleEntry->status;
                refreshed->body = staleEntry->body;
                refreshed->storedAt = now;
                refreshed->expiresAt = decision.cacheable ? decision.expiresAt : now;
                refreshed->staleWhileRevalidate = decision.staleWhileRevalidate;
                refreshed->staleIfError = decision.staleIfError;
                refreshed->headers = std::move(merged);
                if (decision.cacheable) {
                    cache_.store(key, refreshed);
                } else {
                    cache_.purge(key);  // no longer has usable freshness
                }
                co_return co_await writeAll(buildResponseWire(
                    refreshed->status, refreshed->headers, refreshed->body, "REVALIDATED",
                    std::uint64_t{0}, isHead, keepAlive)) && keepAlive;
            }
            co_return co_await writeStale() && keepAlive;  // 5xx covered by stale-if-error
        }

        // A full response streamed successfully: finish the framing and commit the
        // cache if the whole body was accumulated within the size cap.
        if (clientChunked && !co_await writeAll("0\r\n\r\n")) {
            co_return false;
        }
        if (caching) {
            auto entry = std::make_shared<CachedResponse>();
            entry->status = respStatus;
            entry->headers = std::move(respHeaders);
            entry->body = std::move(cacheBuffer);
            entry->storedAt = now;
            entry->expiresAt = cacheDecision.expiresAt;
            entry->staleWhileRevalidate = cacheDecision.staleWhileRevalidate;
            entry->staleIfError = cacheDecision.staleIfError;
            cache_.store(key, std::move(entry));
        }
        co_return keepAlive;
}

asio::awaitable<void> EdgeServer::adminAcceptLoop() {
    for (;;) {
        auto [ec, socket] =
            co_await adminAcceptor_->async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                break;
            }
            continue;
        }
        asio::co_spawn(
            ioContext_, handleAdminSession(std::move(socket)), asio::detached);
    }
}

asio::awaitable<void> EdgeServer::handleAdminSession(asio::ip::tcp::socket socket) {
    const auto writeAll = [&socket](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(
            socket, asio::buffer(wire.data(), wire.size()),
            asio::as_tuple(asio::use_awaitable));
    };
    const auto finish = [&socket]() {
        asio::error_code ignore;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
    };

    std::string inbound;
    std::array<char, 4096> buffer;
    const Http1RequestParser parser;

    for (;;) {
        auto parseResult = parser.parse(inbound);
        if (parseResult.failure() != nullptr) {
            co_await writeAll(buildAdminResponse(400, "bad request"));
            finish();
            co_return;
        }
        const auto* parsed = parseResult.parsed();
        if (parsed == nullptr) {
            if (inbound.size() > kMaxRequestBytes) {
                co_await writeAll(buildAdminResponse(413, "request too large"));
                finish();
                co_return;
            }
            auto [ec, n] = co_await socket.async_read_some(
                asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            if (n > 0) {
                inbound.append(buffer.data(), n);
            }
            if (ec) {
                co_return;
            }
            continue;
        }

        const auto& request = parsed->request();
        const auto method = request.knownMethod();
        const std::string_view path = request.path();
        const auto query = [&](std::string_view name) -> std::string_view {
            return request.query(name).value_or(std::string_view{});
        };

        std::uint16_t status = 404;
        std::string body = "not found";

        if (path == "/stats" && method == HttpKnownMethod::kGet) {
            body = "entries=";
            appendDecimal(body, cache_.entryCount());
            body += " bytes=";
            appendDecimal(body, cache_.byteSize());
            status = 200;
        } else if (path.starts_with("/origins/")) {
            const std::string_view host = path.substr(std::string_view("/origins/").size());
            if (host.empty()) {
                status = 400;
                body = "missing host";
            } else if (method == HttpKnownMethod::kPut) {
                const std::string_view upstream = query("upstream");
                const std::string_view portText = query("port");
                std::uint16_t port = 0;
                const bool portOk = !portText.empty() &&
                    std::from_chars(portText.data(), portText.data() + portText.size(), port)
                            .ec == std::errc{};
                if (upstream.empty() || !portOk) {
                    status = 400;
                    body = "need upstream and port";
                } else {
                    const bool created = config_.addOrigin(
                        std::string(host),
                        OriginSettings{std::string(upstream), port, false});
                    status = 200;
                    body = created ? "created" : "updated";
                }
            } else if (method == HttpKnownMethod::kDelete) {
                const bool removed = config_.removeOrigin(host);
                status = removed ? 200 : 404;
                body = removed ? "removed" : "not found";
            } else {
                status = 405;
                body = "method not allowed";
            }
        } else if (path == "/purge" &&
                   (method == HttpKnownMethod::kPost || method == HttpKnownMethod::kDelete)) {
            const bool purged = cache_.purge(cacheKey("GET", query("host"), query("target")));
            status = purged ? 200 : 404;
            body = purged ? "purged" : "not found";
        } else if (path == "/cache" && method == HttpKnownMethod::kDelete) {
            cache_.clear();
            status = 200;
            body = "cleared";
        }

        co_await writeAll(buildAdminResponse(status, body));
        finish();
        co_return;
    }
}

}  // namespace ruvia::edge
