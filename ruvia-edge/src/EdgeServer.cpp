#include "ruvia/edge/EdgeServer.h"

#include <array>
#include <charconv>
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
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/edge/EdgeFreshness.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::edge {

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

constexpr std::size_t kMaxRequestHeadBytes = 64u * 1024u;

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

// Serialize a response for the client: status line, curated headers, a fresh
// Content-Length, Connection: close (one request per connection), an X-Cache
// marker, and -- when the edge computes its own age for a cache hit -- an Age
// header (dropping any inherited one).
[[nodiscard]] std::string buildResponseWire(
    std::uint16_t status,
    const Headers& headers,
    std::string_view body,
    std::string_view xCache,
    std::optional<std::uint64_t> ageOverride) {
    std::string out;
    out.reserve(body.size() + 256);

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
            continue;
        }
        if (ageOverride && lower == "age") {
            continue;
        }
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    out.append("Content-Length: ");
    appendDecimal(out, body.size());
    out.append("\r\n");
    out.append("Connection: close\r\n");
    out.append("X-Cache: ");
    out.append(xCache);
    out.append("\r\n");
    if (ageOverride) {
        out.append("Age: ");
        appendDecimal(out, *ageOverride);
        out.append("\r\n");
    }
    out.append("\r\n");
    out.append(body);
    return out;
}

// A minimal status-only response for edge-generated errors.
[[nodiscard]] std::string buildStatusWire(std::uint16_t status) {
    return buildResponseWire(status, Headers{}, {}, "MISS", std::nullopt);
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
      fetcher_(options.fetch) {}

EdgeServer::~EdgeServer() {
    ioContext_.stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void EdgeServer::start() {
    asio::co_spawn(ioContext_, acceptLoop(), asio::detached);
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
    const auto writeAll = [&socket](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(
            socket, asio::buffer(wire.data(), wire.size()),
            asio::as_tuple(asio::use_awaitable));
    };
    const auto finish = [&socket]() {
        asio::error_code ignore;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
    };

    std::string clientAddress;
    {
        asio::error_code ec;
        const auto remote = socket.remote_endpoint(ec);
        if (!ec) {
            clientAddress = remote.address().to_string();
        }
    }

    // 1. Read and frame the client request.
    std::string inbound;
    std::array<char, 8192> buffer;
    const Http1RequestParser parser;

    for (;;) {
        auto parseResult = parser.parse(inbound);
        if (parseResult.failure() != nullptr) {
            co_await writeAll(buildStatusWire(400));
            finish();
            co_return;
        }
        const auto* parsed = parseResult.parsed();
        if (parsed == nullptr) {
            if (inbound.size() > kMaxRequestHeadBytes) {
                co_await writeAll(buildStatusWire(431));
                finish();
                co_return;
            }
            auto [ec, n] = co_await socket.async_read_some(
                asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            if (n > 0) {
                inbound.append(buffer.data(), n);
            }
            if (ec) {
                co_return;  // client vanished before sending a full request
            }
            continue;
        }

        // 2. A framed request. The MVP proxies GET only.
        const auto& request = parsed->request();
        if (request.knownMethod() != HttpKnownMethod::kGet) {
            co_await writeAll(buildStatusWire(501));
            finish();
            co_return;
        }

        const std::string_view frontHost = request.header("host").value_or("");
        const std::string_view target = request.target();

        // 3. Resolve the origin from the current published config snapshot.
        const auto snapshot = config_.snapshot();
        const OriginSettings* origin = snapshot->findOrigin(frontHost);
        if (origin == nullptr) {
            co_await writeAll(buildStatusWire(502));
            finish();
            co_return;
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
            co_await writeAll(
                buildResponseWire(entry.status, entry.headers, entry.body, "HIT", age));
            finish();
            co_return;
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
        originRequest.method = "GET";
        originRequest.target = target;
        originRequest.headers = forwardHeaders;
        auto fetch = co_await fetcher_.fetch(
            ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
            originRequest);
        if (fetch.outcome != OriginFetchOutcome::kOk) {
            // A timeout is a gateway timeout; every other failure is a bad gateway.
            const std::uint16_t gatewayStatus =
                fetch.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
            co_await writeAll(buildStatusWire(gatewayStatus));
            finish();
            co_return;
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
                std::uint64_t{0}));
            finish();
            co_return;
        }

        // 6b. A full response: store it if a shared cache is allowed to (replacing
        // any stale entry under this key).
        const auto decision = evaluateFreshness(
            buildFreshnessInput(fetch.response.status, fetch.response.headers, now));
        if (decision.cacheable) {
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
            std::nullopt));
        finish();
        co_return;
    }
}

}  // namespace ruvia::edge
