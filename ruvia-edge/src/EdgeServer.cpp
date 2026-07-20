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
      fetcher_(options.fetch) {
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
        asio::co_spawn(ioContext_, handleSession(std::move(socket)), asio::detached);
    }
}

asio::awaitable<void> EdgeServer::handleSession(asio::ip::tcp::socket socket) {
    using namespace asio::experimental::awaitable_operators;

    std::string clientAddress;
    {
        asio::error_code ec;
        const auto remote = socket.remote_endpoint(ec);
        if (!ec) {
            clientAddress = remote.address().to_string();
        }
    }

    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const auto writeStatus = [&socket, tuple](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(socket, asio::buffer(wire.data(), wire.size()), tuple);
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
                    socket, *parsed, clientAddress, keepAlive);
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
                socket.async_read_some(asio::buffer(buffer), tuple) ||
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
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
}

asio::awaitable<bool> EdgeServer::handleFramedRequest(
    asio::ip::tcp::socket& socket,
    const Http1ParsedRequest& parsed,
    std::string_view clientAddress,
    bool keepAlive) {
    const auto writeAll = [&socket](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(
            socket, asio::buffer(wire.data(), wire.size()),
            asio::as_tuple(asio::use_awaitable));
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
            auto passFetch = co_await fetcher_.fetch(
                ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
                origin->https, passRequest);
            if (passFetch.outcome != OriginFetchOutcome::kOk) {
                const std::uint16_t gatewayStatus =
                    passFetch.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
                co_await writeAll(buildStatusWire(gatewayStatus));
                co_return false;
            }
            if (passFetch.response.status < 400) {
                cache_.purge(cacheKey("GET", frontHost, target));
            }
            co_await writeAll(buildResponseWire(
                passFetch.response.status, passFetch.response.headers,
                passFetch.response.body, "BYPASS", std::nullopt, false, keepAlive));
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
            co_await writeAll(buildResponseWire(
                entry.status, entry.headers, entry.body, "HIT", age, isHead, keepAlive));
            co_return keepAlive;
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
        auto fetch = co_await fetcher_.fetch(
            ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
            origin->https, originRequest);

        // stale-if-error: a stale copy within its stale-if-error window is served
        // when the origin cannot be reached, instead of a gateway error.
        const auto serveStaleOnError = [&]() -> bool {
            return staleEntry != nullptr && staleEntry->staleIfError > 0 &&
                now <= staleEntry->expiresAt +
                           static_cast<std::time_t>(staleEntry->staleIfError);
        };
        const auto writeStale = [&]() -> asio::awaitable<void> {
            const auto age = staleEntry->storedAt <= now
                ? static_cast<std::uint64_t>(now - staleEntry->storedAt)
                : std::uint64_t{0};
            co_await writeAll(buildResponseWire(
                staleEntry->status, staleEntry->headers, staleEntry->body, "STALE", age,
                isHead, keepAlive));
        };

        if (fetch.outcome != OriginFetchOutcome::kOk) {
            if (serveStaleOnError()) {
                co_await writeStale();
                co_return keepAlive;
            }
            // A timeout is a gateway timeout; every other failure is a bad gateway.
            const std::uint16_t gatewayStatus =
                fetch.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
            co_await writeAll(buildStatusWire(gatewayStatus));
            co_return false;
        }

        // A 5xx from the origin is also an error stale-if-error can paper over.
        if (fetch.response.status >= 500 && serveStaleOnError()) {
            co_await writeStale();
            co_return keepAlive;
        }

        // 6a. A 304 confirms the stale entry is still good: refresh its freshness
        // from the (merged) headers and serve the stored body -- no full transfer.
        if (staleEntry && fetch.response.status == 304) {
            Headers merged = mergeStoredHeaders(staleEntry->headers, fetch.response.headers);
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
            co_await writeAll(buildResponseWire(
                refreshed->status, refreshed->headers, refreshed->body, "REVALIDATED",
                std::uint64_t{0}, isHead, keepAlive));
            co_return keepAlive;
        }

        // 6b. A full response: store it if a shared cache is allowed to (replacing
        // any stale entry under this key). A HEAD response has no body to cache.
        const auto decision = evaluateFreshness(
            buildFreshnessInput(fetch.response.status, fetch.response.headers, now));
        if (!isHead && decision.cacheable) {
            auto entry = std::make_shared<CachedResponse>();
            entry->status = fetch.response.status;
            entry->headers = fetch.response.headers;
            entry->body = fetch.response.body;
            entry->storedAt = now;
            entry->expiresAt = decision.expiresAt;
            entry->staleWhileRevalidate = decision.staleWhileRevalidate;
            entry->staleIfError = decision.staleIfError;
            cache_.store(key, std::move(entry));
        }

        // 7. Serve the freshly fetched response.
        co_await writeAll(buildResponseWire(
            fetch.response.status, fetch.response.headers, fetch.response.body, "MISS",
            std::nullopt, isHead, keepAlive));
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
