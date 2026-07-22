#include "ruvia/edge/detail/server/ServerImpl.h"

#include "ruvia/edge/detail/proxy/ForwardHeaders.h"
#include "ruvia/edge/detail/proxy/RangeResponse.h"
#include "ruvia/edge/detail/proxy/RequestDirectives.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include "ruvia/edge/detail/cache/Freshness.h"
#include "ruvia/edge/detail/proxy/HeaderRules.h"
namespace ruvia::edge {

asio::awaitable<bool> EdgeServer::Impl::servePassThrough(
    const EdgeRequest& request,
    ResponseWriter& writer,
    const OriginLease& origin,
    RequestOutcome& outcome) {
    const bool keepAlive = request.keepAlive;

    OriginRequest passRequest;
    passRequest.method = request.method;
    passRequest.target = request.target;
    passRequest.headers = buildForwardHeaders(
        request.headers,
        request.clientAddress,
        request.host,
        tlsEnabled_,
        nullptr,
        ForwardMode::kPassThrough,
        memory_.resource());
    passRequest.body = request.body;

    // Stream the origin response straight through to the client (never
    // cached); the writer re-frames an unknown length as chunked.
    std::uint16_t passStatus = 0;
    bool passHeadSent = false;
    bool passAborted = false;
    ResponseSink passSink;
    passSink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
        passStatus = head.status;
        const Headers responseHeaders = endToEndResponseHeaders(head.headers);
        if (!co_await writer.respondHead(head.status, responseHeaders, "BYPASS",
                                         head.hasBody, head.contentLength, keepAlive)) {
            passAborted = true;
            co_return false;
        }
        passHeadSent = true;
        co_return true;
    };
    passSink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
        if (!co_await writer.respondChunk(chunk)) {
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
        outcome.status = gatewayStatus;
        co_await respondStatusOnly(writer, gatewayStatus, "ERROR", false);
        co_return false;
    }
    if (!co_await writer.respondEnd()) {
        co_return false;
    }
    // A successful unsafe method invalidates every cached variant of this
    // URI (RFC 9111 section 4.4).
    if (!isHttpMethodSafe(request.method) && passStatus < 400) {
        const auto prefix = cacheVariantPrefix(
            "GET", hostWithoutPort(request.host), request.target);
        cache_.purgePrefix(prefix);
        disk_.purgePrefix(prefix);
    }
    outcome.label = "BYPASS";
    outcome.status = passStatus;
    co_return keepAlive;
}

asio::awaitable<bool> EdgeServer::Impl::serveRequest(
    const EdgeRequest& request, ResponseWriter& writer) {
    // Per-request accounting: defaults to an error result; success paths set the
    // label/status below, and the byte count comes from the writer.
    RequestOutcome outcome;

    const bool isGet = request.knownMethod == HttpKnownMethod::kGet;
    const bool isHead = request.knownMethod == HttpKnownMethod::kHead;
    const auto directives = requestDirectives(request.headers);
    const auto& requestCacheControl = directives.cacheControl;
    const bool requestHasAuthorization = directives.hasAuthorization;
    const std::string_view frontHost = hostWithoutPort(request.host);
    const std::string_view target = request.target;
    const bool keepAlive = request.keepAlive;

    struct RequestRecord final {
        Impl* self;
        const EdgeRequest* request;
        const ResponseWriter* writer;
        const RequestOutcome* outcome;
        ~RequestRecord() noexcept {
            self->recordRequest(AccessLogEntry{
                request->clientAddress, request->method, request->host, request->target,
                outcome->status, outcome->label, writer->bytesWritten()});
        }
    };
    const RequestRecord record{this, &request, &writer, &outcome};
    (void)record;

    // 3. Resolve one stable owner-thread lease. It remains valid if a later
    // control operation replaces or removes the mapping while this request
    // is suspended in origin I/O.
    auto origin = config_.findOrigin(frontHost);
    if (!origin) {
        co_await respondStatusOnly(writer, 502, "ERROR", false);
        co_return false;
    }

    // Unsafe methods always write through. This MVP also conservatively
    // forwards conditional and authenticated retrievals instead of trying
    // to evaluate client validators or authenticated reuse locally; caching
    // is optional, while changing either request's semantics is forbidden.
    const bool cacheBypassMethod = !isGet && !isHead;
    const bool cannotUseStoredResponse =
        cacheBypassMethod || directives.hasCondition || requestHasAuthorization ||
        directives.forcesValidation;
    if (requestCacheControl.onlyIfCached && cannotUseStoredResponse) {
        outcome.status = 504;
        outcome.label = "MISS";
        co_return co_await respondStatusOnly(writer, 504, "MISS", keepAlive) &&
            keepAlive;
    }
    if (cannotUseStoredResponse ||
        (requestCacheControl.noStore && !requestCacheControl.onlyIfCached)) {
        co_return co_await servePassThrough(request, writer, origin, outcome);
    }

    std::time_t now = std::time(nullptr);
    const std::string variantPrefix =
        cacheVariantPrefix("GET", frontHost, target);
    const auto acceptEncoding = combinedRequestFieldValue(
        request.headers, "accept-encoding");
    const std::string key =
        cacheKeyFor(variantPrefix, request.host, acceptEncoding);

    // Serve a cached entry, honoring a single client byte-range (206, or 416
    // when unsatisfiable) served from the full cached body.
    const auto serveHit = [&](const CachedResponse& entry) -> asio::awaitable<bool> {
        outcome.label = "HIT";
        const auto age = cachedResponseAge(entry, now);
        if (!isHead) {
            if (const auto rangeHeader = findRequestHeader(request.headers, "range")) {
                if (auto ranged = cachedRangeResponse(entry, *rangeHeader)) {
                    outcome.status = ranged->status;
                    co_return co_await writer.respond(
                               ranged->status,
                               ranged->headers,
                               ranged->body,
                               "HIT",
                               ranged->withAge ? std::optional<std::uint64_t>(age)
                                               : std::nullopt,
                               false,
                               keepAlive) &&
                        keepAlive;
                }
            }
        }
        outcome.status = entry.status;
        co_return co_await writer.respond(
                   entry.status, entry.headers, entry.body, "HIT", age, isHead, keepAlive) &&
            keepAlive;
    };

    // 4. Serve a fresh cache hit without touching the origin.
    auto hit = cache_.lookup(key, now);
    if (hit.status == CacheLookupStatus::kMiss && disk_.enabled()) {
        // Memory miss: consult the persistent disk tier and, on a hit,
        // promote the entry into the hot memory cache.
        if (auto diskEntry = co_await disk_.lookup(key)) {
            cache_.store(key, std::move(*diskEntry));
            hit = cache_.lookup(key, now);
        }
    }
    if (hit.status == CacheLookupStatus::kFresh) {
        co_return co_await serveHit(*hit.entry);
    }
    // A stale entry may still be revalidated with the origin below.
    CacheEntryLease staleEntry =
        hit.status == CacheLookupStatus::kStale ? hit.entry : CacheEntryLease{};

    // stale-while-revalidate: a stale entry still inside its stale-while-
    // revalidate window is served immediately while a single background job
    // refreshes it, so the client never waits on the origin.
    if (isGet && staleEntry && staleEntry->staleWhileRevalidate > 0 &&
        now <= staleEntry->expiresAt +
                   static_cast<std::time_t>(staleEntry->staleWhileRevalidate)) {
        if (inFlight_.find(key) == inFlight_.end()) {
            inFlight_.try_emplace(key);  // one refresh per key
            spawnTracked(backgroundRefresh(RefreshJob{
                key,
                std::string(origin->upstreamHost),
                origin->upstreamPort,
                origin->https,
                std::string(target),
                acceptEncoding,
                staleEntry}));
        }
        const auto age = cachedResponseAge(*staleEntry, now);
        outcome.label = "STALE";
        outcome.status = staleEntry->status;
        co_return co_await writer.respond(
            staleEntry->status, staleEntry->headers, staleEntry->body, "STALE", age,
            isHead, keepAlive) && keepAlive;
    }

    // only-if-cached forbids contacting the origin. A fresh hit or an
    // explicitly reusable stale hit has already returned above; everything
    // left is a cache miss for this request's constraints.
    if (requestCacheControl.onlyIfCached) {
        outcome.status = 504;
        outcome.label = "MISS";
        co_return co_await respondStatusOnly(writer, 504, "MISS", keepAlive) &&
            keepAlive;
    }

    // Request coalescing (GET only): if a fetch for this key is already in
    // flight, wait for it and re-check the cache instead of sending the origin
    // a duplicate request. Whoever finds no in-flight fetch becomes the leader
    // and registers one; leaderGuard wakes the followers when it finishes.
    bool becameLeader = false;
    if (isGet) {
        for (;;) {
            if (inFlight_.find(key) == inFlight_.end()) {
                inFlight_.try_emplace(key);
                becameLeader = true;
                break;
            }
            // A detached HTTP/2 handler whose client has gone is cancelled on
            // session teardown; stop coalescing rather than wait for a leader
            // that can no longer serve this dead connection. No-op for HTTP/1,
            // whose handler carries no cancellation slot.
            if ((co_await asio::this_coro::cancellation_state).cancelled() !=
                asio::cancellation_type::none) {
                co_return false;
            }
            asio::steady_timer waitTimer(ioContext_);
            waitTimer.expires_at((std::chrono::steady_clock::time_point::max)());
            inFlight_[key].waiters.push_back(&waitTimer);
            co_await waitTimer.async_wait(asio::as_tuple(asio::use_awaitable));
            // Drop our waiter before it can dangle. The leader's wakeInFlight()
            // erases the whole entry when it finishes, so remove ours only if
            // the entry is still present -- the teardown-cancel path, where
            // wakeInFlight() has not run for this key.
            if (const auto entry = inFlight_.find(key); entry != inFlight_.end()) {
                std::erase(entry->second.waiters, &waitTimer);
            }
            if ((co_await asio::this_coro::cancellation_state).cancelled() !=
                asio::cancellation_type::none) {
                co_return false;
            }
            now = std::time(nullptr);
            auto woken = cache_.lookup(key, now);
            if (woken.status == CacheLookupStatus::kFresh) {
                co_return co_await serveHit(*woken.entry);
            }
            staleEntry =
                woken.status == CacheLookupStatus::kStale
                ? woken.entry
                : CacheEntryLease{};
        }
    }
    struct LeaderGuard final {
        Impl* self;
        const std::string* key;
        bool active;
        ~LeaderGuard() {
            if (active) {
                self->wakeInFlight(*key);
            }
        }
    } leaderGuard{this, &key, becameLeader};

    // 5. Miss (or stale): fetch from the origin, forwarding the client's
    // header section under the proxy rules in EdgeForwardHeaders.h.
    const auto forwardHeaders = buildForwardHeaders(
        request.headers,
        request.clientAddress,
        request.host,
        tlsEnabled_,
        staleEntry ? &*staleEntry : nullptr,
        ForwardMode::kCache,
        memory_.resource());

    OriginRequest originRequest;
    originRequest.method = request.method;  // GET or HEAD
    originRequest.target = target;
    originRequest.headers = forwardHeaders;

    // stale-if-error: a stale copy within its stale-if-error window is served
    // when the origin cannot be reached (or answers 5xx), instead of an error.
    const auto serveStaleOnError = [&]() -> bool {
        return staleEntry && staleEntry->staleIfError > 0 &&
            now <= staleEntry->expiresAt +
                       static_cast<std::time_t>(staleEntry->staleIfError);
    };
    const auto writeStale = [&]() -> asio::awaitable<bool> {
        const auto age = cachedResponseAge(*staleEntry, now);
        co_return co_await writer.respond(
            staleEntry->status, staleEntry->headers, staleEntry->body, "STALE", age,
            isHead, keepAlive);
    };

    // Streaming sink: writes the client head then each body chunk as the
    // origin responds, and tees a cacheable body into cacheBuffer. A 304
    // (revalidation) or a stale-if-error-covered 5xx declines streaming so the
    // stored body is served after the fetch instead.
    std::uint16_t respStatus = 0;
    Headers respHeaders;
    bool headSent = false;
    bool clientAborted = false;
    bool caching = false;
    std::string cacheBuffer;
    FreshnessDecision cacheDecision;
    const std::time_t originRequestTime = std::time(nullptr);

    ResponseSink sink;
    sink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
        now = std::time(nullptr);
        respStatus = head.status;
        respHeaders = endToEndResponseHeaders(head.headers);
        if (staleEntry && head.status == 304) {
            co_return false;  // revalidation: serve the stored body below
        }
        if (head.status >= 500 && serveStaleOnError()) {
            co_return false;  // stale-if-error: serve the stored body below
        }
        if (!co_await writer.respondHead(head.status, respHeaders, "MISS",
                                         head.hasBody, head.contentLength, keepAlive)) {
            clientAborted = true;
            co_return false;
        }
        headSent = true;
        if (!isHead) {
            cacheDecision =
                evaluateFreshness(buildFreshnessInput(
                    head.status,
                    respHeaders,
                    now,
                    originRequestTime,
                    requestHasAuthorization));
            caching = cacheDecision.cacheable && cacheableUnderVary(respHeaders);
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
        if (!co_await writer.respondChunk(chunk)) {
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
        now = std::time(nullptr);
        if (headSent) {
            co_return false;  // partial response already sent; close
        }
        if (serveStaleOnError()) {
            outcome.label = "STALE";
            outcome.status = staleEntry->status;
            co_return co_await writeStale() && keepAlive;
        }
        const std::uint16_t gatewayStatus =
            fetchResult.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
        outcome.status = gatewayStatus;
        co_await respondStatusOnly(writer, gatewayStatus, "ERROR", false);
        co_return false;
    }

    // The sink declined to stream (304 revalidation, or a stale-if-error 5xx):
    // serve the stored body instead.
    if (!headSent && staleEntry) {
        if (respStatus == 304) {
            Headers merged = mergeStoredHeaders(staleEntry->headers, respHeaders);
            const auto decision =
                evaluateFreshness(buildFreshnessInput(
                    staleEntry->status,
                    merged,
                    now,
                    originRequestTime,
                    requestHasAuthorization));
            CachedResponse refreshed;
            refreshed.status = staleEntry->status;
            refreshed.body = staleEntry->body;
            refreshed.headers = std::move(merged);
            refreshed.storedAt = now;
            refreshed.initialAge = decision.initialAge;
            refreshed.expiresAt = decision.cacheable ? decision.expiresAt : now;
            refreshed.staleWhileRevalidate = decision.staleWhileRevalidate;
            refreshed.staleIfError = decision.staleIfError;
            const bool storable = decision.cacheable && cacheableUnderVary(refreshed.headers);
            if (storable) {
                disk_.store(key, refreshed);
                cache_.store(key, CachedResponse(refreshed));
            } else {
                cache_.purge(key);  // no longer has usable freshness
                disk_.purge(key);
            }
            outcome.label = "REVALIDATED";
            outcome.status = refreshed.status;
            co_return co_await writer.respond(
                refreshed.status, refreshed.headers, refreshed.body, "REVALIDATED",
                refreshed.initialAge, isHead, keepAlive) && keepAlive;
        }
        outcome.label = "STALE";  // 5xx covered by stale-if-error
        outcome.status = staleEntry->status;
        co_return co_await writeStale() && keepAlive;
    }

    // A full response streamed successfully: finish the framing and commit the
    // cache if the whole body was accumulated within the size cap.
    if (!co_await writer.respondEnd()) {
        co_return false;
    }
    if (caching) {
        CachedResponse entry;
        entry.status = respStatus;
        entry.headers = std::move(respHeaders);
        entry.body = std::move(cacheBuffer);
        entry.storedAt = now;
        entry.initialAge = cacheDecision.initialAge;
        entry.expiresAt = cacheDecision.expiresAt;
        entry.staleWhileRevalidate = cacheDecision.staleWhileRevalidate;
        entry.staleIfError = cacheDecision.staleIfError;
        disk_.store(key, entry);
        cache_.store(key, std::move(entry));
    } else if (staleEntry && respStatus < 500) {
        // A successful/full replacement that is no longer storable (for
        // example no-store/private, unsupported Vary, or an oversized new
        // representation) supersedes the stale entry. Keeping it would let
        // a later stale-if-error path resurrect data the origin withdrew.
        cache_.purge(key);
        disk_.purge(key);
    }
    outcome.label = "MISS";
    outcome.status = respStatus;
    co_return keepAlive;
}

asio::awaitable<void> EdgeServer::Impl::backgroundRefresh(RefreshJob job) {
    // Wake any foreground waiters and drop the in-flight entry however this ends.
    struct Guard final {
        Impl* self;
        const std::string* key;
        ~Guard() { self->wakeInFlight(*key); }
    } guard{this, &job.key};

    // A conditional GET for the same variant (validator + the variant's encoding).
    std::pmr::vector<HttpHeaderView> headers(memory_.resource());
    if (const auto etag = findHeaderValue(job.stored->headers, "etag")) {
        headers.emplace_back(std::string_view("If-None-Match"), *etag);
    } else if (const auto lastModified =
                   findHeaderValue(job.stored->headers, "last-modified")) {
        headers.emplace_back(std::string_view("If-Modified-Since"), *lastModified);
    }
    if (job.acceptEncoding) {
        headers.emplace_back(
            std::string_view("Accept-Encoding"),
            std::string_view(*job.acceptEncoding));
    }
    headers.emplace_back(std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

    OriginRequest request;
    request.method = "GET";
    request.target = job.target;
    request.headers = headers;

    // Background sink: accumulate a cacheable body; it never writes to a client.
    std::uint16_t status = 0;
    Headers respHeaders;
    std::string body;
    bool caching = false;
    FreshnessDecision decision;
    std::time_t now = std::time(nullptr);
    const std::time_t originRequestTime = now;

    ResponseSink sink;
    sink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
        now = std::time(nullptr);
        status = head.status;
        respHeaders = endToEndResponseHeaders(head.headers);
        if (head.status == 304) {
            co_return false;  // not modified: refresh freshness below
        }
        decision = evaluateFreshness(buildFreshnessInput(
            head.status, respHeaders, now, originRequestTime, false));
        caching = decision.cacheable && cacheableUnderVary(respHeaders);
        co_return caching;  // only download a body we intend to cache
    };
    sink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
        if (body.size() + chunk.size() > maxCacheableBytes_) {
            caching = false;
            co_return false;  // too big to cache: abandon the refresh
        }
        body.append(chunk);
        co_return true;
    };

    auto result = co_await fetcher_.fetch(
        ioContext_.get_executor(), job.host, job.port, job.https, request, sink);
    if (result.outcome != OriginFetchOutcome::kOk) {
        co_return;  // origin unreachable: leave the stale entry in place
    }

    if (status == 304) {
        Headers merged = mergeStoredHeaders(job.stored->headers, respHeaders);
        const auto refreshed =
            evaluateFreshness(buildFreshnessInput(
                job.stored->status, merged, now, originRequestTime, false));
        if (refreshed.cacheable && cacheableUnderVary(merged)) {
            CachedResponse entry;
            entry.status = job.stored->status;
            entry.body = job.stored->body;
            entry.storedAt = now;
            entry.initialAge = refreshed.initialAge;
            entry.expiresAt = refreshed.expiresAt;
            entry.staleWhileRevalidate = refreshed.staleWhileRevalidate;
            entry.staleIfError = refreshed.staleIfError;
            entry.headers = std::move(merged);
            disk_.store(job.key, entry);
            cache_.store(job.key, std::move(entry));
        }
        co_return;
    }

    if (caching) {
        CachedResponse entry;
        entry.status = status;
        entry.headers = std::move(respHeaders);
        entry.body = std::move(body);
        entry.storedAt = now;
        entry.initialAge = decision.initialAge;
        entry.expiresAt = decision.expiresAt;
        entry.staleWhileRevalidate = decision.staleWhileRevalidate;
        entry.staleIfError = decision.staleIfError;
        disk_.store(job.key, entry);
        cache_.store(job.key, std::move(entry));
    } else if (status < 500) {
        // A background 2xx/3xx/4xx full response replaced the old
        // representation but cannot itself be stored. Drop the stale copy;
        // 5xx validation failures intentionally leave it available to policy.
        cache_.purge(job.key);
        disk_.purge(job.key);
    }
}

}  // namespace ruvia::edge
