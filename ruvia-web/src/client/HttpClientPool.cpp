#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ranges>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpSetCookie.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/integration/WorkerCancellationPost.h"

namespace ruvia::detail {
namespace {

std::string_view selectedAlpn(SSL* ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int length = 0;
    SSL_get0_alpn_selected(ssl, &selected, &length);
    return {reinterpret_cast<const char*>(selected), length};
}

bool isIpAddress(std::string_view host) noexcept {
    std::error_code error;
    (void)asio::ip::make_address(host, error);
    return !error;
}

bool headerNameEquals(std::string_view left, std::string_view right) noexcept {
    return httpAsciiEqualsIgnoreCase(left, right);
}

bool cookieDomainMatches(std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) return true;
    if (httpAsciiEqualsIgnoreCase(host, domain)) return true;
    if (isIpAddress(host)) return false;
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
        httpAsciiEqualsIgnoreCase(host.substr(host.size() - domain.size()), domain);
}

bool cookiePathMatches(std::string_view requestPath, std::string_view cookiePath) noexcept {
    if (cookiePath.empty() || cookiePath == "/") return !requestPath.empty() && requestPath.front() == '/';
    if (!requestPath.starts_with(cookiePath)) return false;
    return requestPath.size() == cookiePath.size() || cookiePath.back() == '/' || requestPath[cookiePath.size()] == '/';
}

bool isValidReceivedCookieRequestValue(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return isValidCookieValue(value);
}

bool canSerializeReceivedCookie(std::string_view name, std::string_view value) noexcept {
    return (name.empty() || isValidHttpHeaderName(name)) && isValidReceivedCookieRequestValue(value);
}

std::size_t httpClientSchedulerSlots(const HttpClientConfigStorage& config) {
    if (config.protocol == HttpClientProtocol::kHttp1Only) return config.connectionsPerWorker;
    if (config.maxConcurrentHttp2StreamsPerConnection > std::numeric_limits<std::size_t>::max() / config.connectionsPerWorker) {
        throw std::invalid_argument("HTTP client connection and HTTP/2 stream capacity is too large");
    }
    return config.connectionsPerWorker * config.maxConcurrentHttp2StreamsPerConnection;
}

std::string_view requestPathOnly(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view defaultCookiePath(std::string_view target) noexcept {
    const auto path = requestPathOnly(target);
    if (path.empty() || path.front() != '/') return "/";
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/") : path.substr(0, slash);
}

std::chrono::system_clock::time_point cookieExpiration(
    std::chrono::system_clock::time_point now,
    std::int64_t maxAgeSeconds) noexcept {
    using Clock = std::chrono::system_clock;
    const std::chrono::duration<long double> requested{std::chrono::seconds(maxAgeSeconds)};
    const std::chrono::duration<long double> available{Clock::time_point::max() - now};
    if (requested >= available) return Clock::time_point::max();
    return now + std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(maxAgeSeconds));
}

}  // namespace

static_assert(workerCancellationPostIsInline<HttpClientOperationCancellationMailbox>);

HttpClientPool::Connection::Connection(asio::io_context& ioContext, asio::ssl::context& tlsContext, const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : resolver(ioContext), stream(ioContext, tlsContext), readBuffer(httpPmrResourceOrDefault(resource)),
      writeBuffer(httpPmrResourceOrDefault(resource)), http2(nullptr, PmrObjectDeleter<Http2Connection>{httpPmrResourceOrDefault(resource)}),
      http2Runtime(makePmrObject<Http2Runtime>(resource, worker, resource)),
      deadlineTimer(makePmrObject<WorkerTimerRegistration>(resource)) {
    readBuffer.reserve(16 * 1024);
}

HttpClientPool::Connection::~Connection() = default;
HttpClientPool::Connection::Connection(Connection&&) noexcept = default;

HttpClientPool::HttpClientPool(asio::io_context& ioContext, const WorkerHandle& worker, HttpClientConfigStorage config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext), worker_(worker), resource_(httpPmrResourceOrDefault(resource)), config_(std::move(config)),
      tlsContext_(asio::ssl::context::tls_client), connections_(resource_), scheduler_(httpClientSchedulerSlots(config_), resource_),
      backgroundTasks_(worker_, resource_),
      cookies_(resource_), cookiesEnabled_(config_.cookiesEnabled) {
    validateHttpClientConfig(config_);
    for (const auto& [name, value] : config_.cookies) addCookie(name, value);
    configureTls();
    connections_.reserve(config_.connectionsPerWorker);
    for (std::size_t i = 0; i < config_.connectionsPerWorker; ++i) connections_.emplace_back(ioContext_, tlsContext_, worker_, resource_);
    cancellationMailbox_ = std::make_shared<HttpClientOperationCancellationMailbox>(*this, worker_);
}

HttpClientPool::~HttpClientPool() { closeNow(); }

void HttpClientPool::configureTls() {
    if (config_.verifyCertificate) {
        tlsContext_.set_verify_mode(asio::ssl::verify_peer);
        if (config_.caFile.empty()) tlsContext_.set_default_verify_paths();
        else tlsContext_.load_verify_file(std::string(config_.caFile));
    } else {
        tlsContext_.set_verify_mode(asio::ssl::verify_none);
    }
    if (!config_.privateKeyPassword.empty()) {
        auto password = std::string(config_.privateKeyPassword);
        tlsContext_.set_password_callback([password = std::move(password)](std::size_t, asio::ssl::context_base::password_purpose) { return password; });
    }
    if (!config_.certificateChainFile.empty()) {
        tlsContext_.use_certificate_chain_file(std::string(config_.certificateChainFile));
        tlsContext_.use_private_key_file(std::string(config_.privateKeyFile), asio::ssl::context::pem);
    }
}

HttpClientPool::Lease::~Lease() {
    if (discard_) pool_.close(connection());
    pool_.release(index_);
}

void HttpClientPool::close(Connection& connection) noexcept {
    connection.deadlineTimer->cancel();
    connection.deadline.reset();
    connection.resolver.cancel();
    std::error_code ignored;
    connection.stream.lowest_layer().cancel(ignored);
    connection.stream.lowest_layer().close(ignored);
    connection.connected = false;
    connection.protocol = WireProtocol::kUnknown;
    if (connection.http2Runtime->sessionTasks == 0) {
        connection.http2.reset();
        connection.http2Runtime->running = false;
        connection.http2Runtime->draining = false;
        connection.http2Runtime->failed = false;
    }
    connection.readBuffer.clear();
    connection.writeBuffer.clear();
}

void HttpClientPool::closeNow() noexcept {
    cancellationMailbox_->detach(*this);
    if (!scheduler_.close()) return;
    backgroundTasks_.requestStop();
    for (auto& connection : connections_) {
        connection.abortReason = AbortReason::kClosing;
        auto& runtime = *connection.http2Runtime;
        (void)runtime.connectScheduler.close();
        (void)runtime.http1Scheduler.close();
        if (runtime.running) {
            failHttp2Session(connection, runtime.generation, std::make_error_code(std::errc::operation_canceled));
        } else {
            close(connection);
        }
    }
}

Task<void> HttpClientPool::join() {
    if (backgroundJoined_) co_return;
    backgroundJoined_ = true;
    co_await backgroundTasks_.join();
}

HttpClientStats HttpClientPool::stats() const noexcept {
    return {requestsBuffered_, requestsInFlight_, completedRequests_, failedRequests_, bytesSent_, bytesReceived_};
}

std::uint16_t HttpClientPool::port() const noexcept { return httpClientPort(config_); }

void HttpClientPool::addCookie(std::string_view name, std::string_view value) {
    if (!isValidHttpHeaderName(name) || !isValidCookieValue(value)) {
        throw std::invalid_argument("invalid HTTP client cookie");
    }
    const auto match = std::ranges::find_if(cookies_, [name](const StoredCookie& cookie) {
        return cookie.persistent && cookie.name == name && cookie.path == "/" && cookie.domain.empty();
    });
    const auto replacementBytes = cookieStorageBytes(name, value, "/", {});
    const auto replacedBytes = match == cookies_.end()
        ? 0
        : cookieStorageBytes(match->name, match->value, match->path, match->domain);
    if (!cookieCapacityAvailable(replacedBytes, replacementBytes, match == cookies_.end())) {
        throw std::length_error("HTTP client cookie jar capacity exceeded");
    }
    if (match == cookies_.end()) cookies_.emplace_back(name, value, resource_);
    else {
        std::pmr::string replacementValue(value, resource_);
        match->value.swap(replacementValue);
    }
    cookieBytes_ = cookieBytes_ - replacedBytes + replacementBytes;
}

std::size_t HttpClientPool::cookieStorageBytes(
    std::string_view name,
    std::string_view value,
    std::string_view path,
    std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += field.size();
    }
    return total;
}

bool HttpClientPool::cookieCapacityAvailable(
    std::size_t replacedBytes,
    std::size_t replacementBytes,
    bool adding) const noexcept {
    if (adding && cookies_.size() >= config_.maxCookiesPerWorker) return false;
    if (replacedBytes > cookieBytes_) return false;
    const auto retainedBytes = cookieBytes_ - replacedBytes;
    return replacementBytes <= config_.maxCookieBytesPerWorker -
        std::min(retainedBytes, config_.maxCookieBytesPerWorker);
}

void HttpClientPool::appendAutomaticHeaders(const HttpClientRequest& request, std::pmr::vector<HttpHeaderView>& headers, std::pmr::string& cookieHeader) {
    const auto hasHeader = [&headers](std::string_view name) {
        return std::ranges::any_of(headers, [name](const HttpHeaderView& header) { return headerNameEquals(header.name(), name); });
    };
    if (!config_.userAgent.empty() && !hasHeader("user-agent")) headers.emplace_back("user-agent", config_.userAgent);
    std::erase_if(headers, [&cookieHeader](const HttpHeaderView& header) {
        if (!headerNameEquals(header.name(), "cookie")) return false;
        if (!cookieHeader.empty()) cookieHeader.append("; ");
        cookieHeader.append(header.value());
        return true;
    });

    const auto now = std::chrono::system_clock::now();
    for (auto cookie = cookies_.begin(); cookie != cookies_.end();) {
        if (cookie->expires.has_value() && *cookie->expires <= now) {
            cookieBytes_ -= cookieStorageBytes(cookie->name, cookie->value, cookie->path, cookie->domain);
            cookie = cookies_.erase(cookie);
        } else {
            ++cookie;
        }
    }
    const auto path = requestPathOnly(request.target());
    for (const auto& cookie : cookies_) {
        if (!cookie.persistent && !cookiesEnabled_) continue;
        if (cookie.secure && config_.scheme != HttpScheme::kHttps) continue;
        if (!cookieDomainMatches(config_.host, cookie.domain) || !cookiePathMatches(path, cookie.path)) continue;
        if (!cookieHeader.empty()) cookieHeader.append("; ");
        if (!cookie.name.empty()) {
            cookieHeader.append(cookie.name);
            cookieHeader.push_back('=');
        }
        cookieHeader.append(cookie.value);
    }
    if (!cookieHeader.empty()) headers.emplace_back("cookie", cookieHeader);
}

void HttpClientPool::retainResponseCookies(const HttpClientRequest& request, const HttpClientResponse& response) {
    if (!cookiesEnabled_) return;
    const auto now = std::chrono::system_clock::now();
    for (const auto& header : response.headers()) {
        if (!headerNameEquals(header.name(), "set-cookie")) continue;
        const auto parsed = parseSetCookie(header.value());
        if (!parsed || (parsed->secure && config_.scheme != HttpScheme::kHttps) ||
            !cookieDomainMatches(config_.host, parsed->domain) ||
            !canSerializeReceivedCookie(parsed->name, parsed->value)) continue;

        const auto path = parsed->path.empty() || parsed->path.front() != '/' ? defaultCookiePath(request.target()) : parsed->path;
        const bool securePrefixed = cookieNameStartsWithIgnoreCase(parsed->name, "__Secure-");
        const bool hostPrefixed = cookieNameStartsWithIgnoreCase(parsed->name, "__Host-");
        const bool namelessPrefix = parsed->name.empty() &&
            (cookieNameStartsWithIgnoreCase(parsed->value, "__Secure-") ||
                cookieNameStartsWithIgnoreCase(parsed->value, "__Host-"));
        if (namelessPrefix || (parsed->sameSiteNone && !parsed->secure) ||
            (securePrefixed && (!parsed->secure || config_.scheme != HttpScheme::kHttps)) ||
            (hostPrefixed && (!parsed->secure || config_.scheme != HttpScheme::kHttps ||
                !parsed->hasPathAttribute || parsed->path != "/" || !parsed->domain.empty()))) continue;
        std::optional<std::chrono::system_clock::time_point> expires;
        bool remove = false;
        if (parsed->maxAgeSeconds) {
            remove = *parsed->maxAgeSeconds <= 0;
            if (!remove) expires = cookieExpiration(now, *parsed->maxAgeSeconds);
        } else if (parsed->expires) {
            remove = *parsed->expires <= std::chrono::system_clock::to_time_t(now);
            if (!remove) {
                const auto expirationLimit = std::chrono::system_clock::to_time_t(
                    cookieExpiration(now, detail::kMaxCookieAgeSeconds));
                expires = std::chrono::system_clock::from_time_t(
                    std::min(*parsed->expires, expirationLimit));
            }
        }
        const auto parsedIdentityDomain = parsed->domain.empty()
            ? std::string_view(config_.host)
            : parsed->domain;
        const bool parsedHostOnly = parsed->domain.empty();
        const auto match = std::ranges::find_if(cookies_, [&](const StoredCookie& cookie) {
            const auto cookieIdentityDomain = cookie.domain.empty()
                ? std::string_view(config_.host)
                : std::string_view(cookie.domain);
            return !cookie.persistent && cookie.name == parsed->name &&
                cookie.hostOnly == parsedHostOnly && cookie.path == path &&
                httpAsciiEqualsIgnoreCase(cookieIdentityDomain, parsedIdentityDomain);
        });
        if (remove) {
            if (match != cookies_.end()) {
                cookieBytes_ -= cookieStorageBytes(match->name, match->value, match->path, match->domain);
                cookies_.erase(match);
            }
            continue;
        }
        const auto replacementBytes = cookieStorageBytes(parsed->name, parsed->value, path, parsed->domain);
        const auto replacedBytes = match == cookies_.end()
            ? 0
            : cookieStorageBytes(match->name, match->value, match->path, match->domain);
        if (!cookieCapacityAvailable(replacedBytes, replacementBytes, match == cookies_.end())) continue;
        auto makeStoredCookie = [&]() {
            StoredCookie cookie(parsed->name, parsed->value, resource_);
            cookie.path.assign(path);
            cookie.domain.assign(parsed->domain);
            cookie.expires = expires;
            cookie.secure = parsed->secure;
            cookie.hostOnly = parsedHostOnly;
            cookie.persistent = false;
            return cookie;
        };
        if (match == cookies_.end()) {
            // RFC 6265 section 5.4 sends longer paths first and uses creation
            // order as the tie-breaker. Keep the jar in that order when it is
            // mutated so the request hot path only has to scan and append.
            const auto insertion = std::ranges::find_if(cookies_, [path](const StoredCookie& cookie) {
                return cookie.path.size() < path.size();
            });
            const auto insertionIndex = static_cast<std::size_t>(insertion - cookies_.begin());
            auto cookie = makeStoredCookie();
            cookies_.reserve(cookies_.size() + 1);
            cookies_.emplace(cookies_.begin() + static_cast<std::ptrdiff_t>(insertionIndex), std::move(cookie));
        } else {
            auto replacement = makeStoredCookie();
            match->name.swap(replacement.name);
            match->value.swap(replacement.value);
            match->path.swap(replacement.path);
            match->domain.swap(replacement.domain);
            std::swap(match->expires, replacement.expires);
            std::swap(match->secure, replacement.secure);
            std::swap(match->hostOnly, replacement.hostOnly);
            std::swap(match->persistent, replacement.persistent);
        }
        cookieBytes_ = cookieBytes_ - replacedBytes + replacementBytes;
    }
}

Task<std::size_t> HttpClientPool::acquire(const OperationTimeout& timeout, StopToken stopToken) {
    auto result = co_await scheduler_.acquire(timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken), worker_);
    if (result.timedOut()) throw HttpClientError(HttpClientError::Code::kTimeout, "http client connection pool acquire timed out");
    if (result.cancelled()) throw HttpClientError(HttpClientError::Code::kCancelled, "http client request cancelled");
    if (result.closed()) throw HttpClientError(HttpClientError::Code::kClosing, "http client pool is closing");
    if (!result.acquired()) std::terminate();
    co_return result.acquired()->index();
}

void HttpClientPool::release(std::size_t index) noexcept {
    const auto status = scheduler_.release(index);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot || status == PoolLeaseReleaseStatus::kAlreadyReleased) std::terminate();
}

std::uint64_t HttpClientPool::nextCancellationId() noexcept {
    if (++nextCancellationId_ == 0) {
        ++nextCancellationId_;
    }
    return nextCancellationId_;
}

void HttpClientPool::cancelOperationById(std::uint64_t cancellationId) noexcept {
    if (cancellationId == 0) {
        return;
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        auto& connection = connections_[index];
        if (connection.cancellationId == cancellationId) {
            connection.cancellationId = 0;
            cancelOperation(index, connection.generation, AbortReason::kCancelled);
            return;
        }
        auto& runtime = *connection.http2Runtime;
        if (runtime.stateCancellationId == cancellationId) {
            runtime.stateSignal.notify();
            return;
        }
        const auto pending = std::ranges::find_if(runtime.pending, [cancellationId](const Http2PendingStream* stream) {
            return stream->cancellationId == cancellationId;
        });
        if (pending != runtime.pending.end()) {
            (*pending)->cancellationId = 0;
            cancelHttp2Stream(connection, (*pending)->requestId, AbortReason::kCancelled);
            return;
        }
    }
}

bool HttpClientPool::armDeadline(Connection& connection, const OperationTimeout& timeout, DeadlineKind kind) {
    connection.deadlineTimer->cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) { connection.deadline.reset(); return true; }
    if (remaining->count() == 0) { connection.deadline.reset(); return false; }
    const auto deadline = workerTimerDeadlineAfter(*remaining);
    connection.deadline.arm(deadline, kind);
    WorkerHandleAccess::scheduleTimer(worker_, *connection.deadlineTimer, deadline, [&connection](WorkerTimerOutcome outcome) noexcept {
        if (outcome != WorkerTimerOutcome::kExpired) return;
        const auto expired = connection.deadline.expire(std::chrono::steady_clock::now());
        if (!expired) return;
        connection.abortReason = AbortReason::kTimeout;
        std::error_code ignored;
        if (*expired == DeadlineKind::kResolve) connection.resolver.cancel();
        else connection.stream.lowest_layer().cancel(ignored);
    });
    return true;
}

bool HttpClientPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineTimer->cancel();
    return connection.deadline.clear();
}

void HttpClientPool::cancelOperation(std::size_t index, std::uint64_t generation, AbortReason reason) noexcept {
    if (connections_.empty()) return;
    auto& connection = connections_[index % connections_.size()];
    if (connection.generation != generation) return;
    connection.abortReason = reason;
    std::error_code ignored;
    connection.resolver.cancel();
    connection.stream.lowest_layer().cancel(ignored);
    connection.stream.lowest_layer().close(ignored);
    connection.http2Runtime->writeSignal.notify();
    connection.http2Runtime->stateSignal.notify();
}

void HttpClientPool::throwAbort(const Connection& connection) const {
    switch (connection.abortReason) {
        case AbortReason::kNone: return;
        case AbortReason::kTimeout: throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
        case AbortReason::kCancelled: throw HttpClientError(HttpClientError::Code::kCancelled, "http client request cancelled");
        case AbortReason::kClosing: throw HttpClientError(HttpClientError::Code::kClosing, "http client pool is closing");
    }
}

Task<void> HttpClientPool::write(Connection& connection, std::string_view bytes, const OperationTimeout& timeout) {
    if (bytes.empty()) co_return;
    const auto writeTimeout = timeout.constrainedBy(config_.writeTimeout);
    if (!armDeadline(connection, writeTimeout, DeadlineKind::kSocket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    }
    AsioCompletion<std::size_t> completion = config_.scheme == HttpScheme::kHttps
        ? co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable { asio::async_write(connection.stream, asio::buffer(bytes), std::move(handler)); })
        : co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable { asio::async_write(connection.stream.next_layer(), asio::buffer(bytes), std::move(handler)); });
    const bool timedOut = clearDeadline(connection) || writeTimeout.expired();
    throwAbort(connection);
    if (timedOut) throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    if (completion.errorCode()) {
        const auto code = config_.scheme == HttpScheme::kHttps &&
                (completion.errorCode().category() == asio::error::get_ssl_category() ||
                    completion.errorCode() == asio::ssl::error::stream_truncated)
            ? HttpClientError::Code::kTlsFailed
            : HttpClientError::Code::kIoError;
        throw HttpClientError(code, completion.errorCode().message());
    }
    bytesSent_ += completion.result();
}

Task<std::size_t> HttpClientPool::readSome(Connection& connection, std::span<char> bytes, const OperationTimeout& timeout, bool allowEof) {
    if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    AsioCompletion<std::size_t> completion = config_.scheme == HttpScheme::kHttps
        ? co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable { connection.stream.async_read_some(asio::buffer(bytes.data(), bytes.size()), std::move(handler)); })
        : co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable { connection.stream.next_layer().async_read_some(asio::buffer(bytes.data(), bytes.size()), std::move(handler)); });
    const bool timedOut = clearDeadline(connection) || timeout.expired();
    throwAbort(connection);
    if (timedOut) throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    if (completion.errorCode()) {
        if (allowEof && completion.errorCode() == asio::error::eof) co_return 0;
        const auto code = config_.scheme == HttpScheme::kHttps &&
                (completion.errorCode().category() == asio::error::get_ssl_category() ||
                    completion.errorCode() == asio::ssl::error::stream_truncated)
            ? HttpClientError::Code::kTlsFailed
            : HttpClientError::Code::kIoError;
        throw HttpClientError(code, completion.errorCode().message());
    }
    bytesReceived_ += completion.result();
    co_return completion.result();
}

Task<void> HttpClientPool::ensureConnected(
    Connection& connection,
    const OperationTimeout& operationTimeout,
    const OperationTimeout& acquireTimeout,
    StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    auto acquired = co_await runtime.connectScheduler.acquire(acquireTimeout.remaining(), stopToken, worker_);
    if (acquired.timedOut()) throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP client connect wait timed out");
    if (acquired.cancelled()) throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client connect wait cancelled");
    if (!acquired.acquired()) throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is closing");
    const auto connectSlot = acquired.acquired()->index();
    struct ConnectLease final {
        PoolLeaseScheduler& scheduler;
        std::size_t slot;
        ~ConnectLease() { (void)scheduler.release(slot); }
    } connectLease{runtime.connectScheduler, connectSlot};
    if (connection.connected) co_return;
    connection.abortReason = AbortReason::kNone;
    auto generation = ++connection.generation;
    if (generation == 0) generation = ++connection.generation;
    struct ConnectCancellationGeneration final {
        Connection& connection;
        ~ConnectCancellationGeneration() { ++connection.generation; }
    } cancellationGeneration{connection};
    connection.cancellationId = 0;
    std::uint64_t cancellationId = 0;
    StopRegistration stopRegistration;
    if (stopToken.stoppable()) {
        cancellationId = nextCancellationId();
        connection.cancellationId = cancellationId;
        stopToken.registerCallback(
            stopRegistration,
            WorkerCancellationPost<HttpClientOperationCancellationMailbox>(cancellationMailbox_, cancellationId));
    }
    struct CancellationRegistrationGuard final {
        Connection& connection;
        std::uint64_t cancellationId;
        StopRegistration& registration;

        ~CancellationRegistrationGuard() {
            if (connection.cancellationId == cancellationId) {
                connection.cancellationId = 0;
            }
            registration.reset();
        }
    } cancellationRegistrationGuard{connection, cancellationId, stopRegistration};
    if (stopToken.stopRequested()) cancelOperationById(cancellationId);
    while (runtime.sessionTasks != 0 || !runtime.pending.empty()) {
        throwAbort(connection);
        co_await runtime.stateSignal.wait();
    }
    throwAbort(connection);
    runtime.connecting = true;
    struct ConnectGuard final {
        Http2Runtime& runtime;
        ~ConnectGuard() {
            runtime.connecting = false;
            runtime.stateSignal.notify();
        }
    } connectGuard{runtime};
    connection.http2.reset();
    runtime.running = false;
    runtime.draining = false;
    runtime.failed = false;
    const auto timeout = operationTimeout.constrainedBy(config_.connectTimeout);
    std::array<char, 8> portBytes{};
    const auto [portEnd, ec] = std::to_chars(portBytes.data(), portBytes.data() + portBytes.size(), httpClientPort(config_));
    if (ec != std::errc{}) throw HttpClientError(HttpClientError::Code::kConnectFailed, "invalid http client port");
    const std::string_view port(portBytes.data(), static_cast<std::size_t>(portEnd - portBytes.data()));
    if (!armDeadline(connection, timeout, DeadlineKind::kResolve)) throw HttpClientError(HttpClientError::Code::kTimeout, "http client resolve timed out");
    auto resolve = co_await asyncAsio<asio::ip::tcp::resolver::results_type>([&connection, this, port](auto handler) mutable { connection.resolver.async_resolve(config_.host, port, std::move(handler)); });
    const bool resolveTimedOut = clearDeadline(connection) || timeout.expired();
    throwAbort(connection);
    if (resolveTimedOut) throw HttpClientError(HttpClientError::Code::kTimeout, "http client resolve timed out");
    if (resolve.errorCode()) throw HttpClientError(HttpClientError::Code::kResolveFailed, resolve.errorCode().message());
    auto endpoints = std::move(resolve).takeResult();

    if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) throw HttpClientError(HttpClientError::Code::kTimeout, "http client connect timed out");
    auto connected = co_await asyncAsio([&connection, &endpoints](auto handler) mutable { asio::async_connect(connection.stream.lowest_layer(), endpoints, std::move(handler)); });
    const bool connectTimedOut = clearDeadline(connection) || timeout.expired();
    throwAbort(connection);
    if (connectTimedOut) throw HttpClientError(HttpClientError::Code::kTimeout, "http client connect timed out");
    if (connected.errorCode()) throw HttpClientError(HttpClientError::Code::kConnectFailed, connected.errorCode().message());
    std::error_code ignored;
    if (config_.tcpNoDelay) connection.stream.lowest_layer().set_option(asio::ip::tcp::no_delay(true), ignored);
    if (config_.keepAlive) connection.stream.lowest_layer().set_option(asio::socket_base::keep_alive(true), ignored);

    if (config_.scheme == HttpScheme::kHttps) {
        SSL_clear(connection.stream.native_handle());
        // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6
        // literal. Certificate verification still receives the configured IP
        // so OpenSSL can apply its IP subjectAltName rules.
        if (!isIpAddress(config_.host) &&
            SSL_set_tlsext_host_name(connection.stream.native_handle(), config_.host.c_str()) != 1) {
            throw HttpClientError(HttpClientError::Code::kTlsFailed, "failed to set TLS SNI host");
        }
        if (config_.verifyCertificate) connection.stream.set_verify_callback(asio::ssl::host_name_verification(std::string(config_.host)));
        static constexpr unsigned char both[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        static constexpr unsigned char h1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        static constexpr unsigned char h2[] = {2, 'h', '2'};
        const auto* protocols = config_.protocol == HttpClientProtocol::kHttp1Only ? h1 : (config_.protocol == HttpClientProtocol::kHttp2Only ? h2 : both);
        const auto protocolBytes = config_.protocol == HttpClientProtocol::kHttp1Only ? sizeof(h1) : (config_.protocol == HttpClientProtocol::kHttp2Only ? sizeof(h2) : sizeof(both));
        if (SSL_set_alpn_protos(connection.stream.native_handle(), protocols, static_cast<unsigned int>(protocolBytes)) != 0) {
            throw HttpClientError(HttpClientError::Code::kTlsFailed, "failed to configure TLS ALPN");
        }
        if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) throw HttpClientError(HttpClientError::Code::kTimeout, "http client TLS handshake timed out");
        auto handshake = co_await asyncAsio([&connection](auto handler) mutable { connection.stream.async_handshake(asio::ssl::stream_base::client, std::move(handler)); });
        const bool handshakeTimedOut = clearDeadline(connection) || timeout.expired();
        throwAbort(connection);
        if (handshakeTimedOut) throw HttpClientError(HttpClientError::Code::kTimeout, "http client TLS handshake timed out");
        if (handshake.errorCode()) throw HttpClientError(HttpClientError::Code::kTlsFailed, handshake.errorCode().message());
        const auto alpn = selectedAlpn(connection.stream.native_handle());
        if (config_.protocol == HttpClientProtocol::kHttp2Only && alpn != "h2") throw HttpClientError(HttpClientError::Code::kProtocolUnavailable, "upstream did not negotiate HTTP/2");
        connection.protocol = alpn == "h2" ? WireProtocol::kHttp2 : WireProtocol::kHttp1;
    } else {
        connection.protocol = config_.protocol == HttpClientProtocol::kHttp2Only ? WireProtocol::kHttp2 : WireProtocol::kHttp1;
    }
    connection.connected = true;
    if (connection.protocol == WireProtocol::kHttp2) co_await initializeHttp2(connection, timeout);
}

Task<HttpClientResponse> HttpClientPool::execute(HttpClientRequest request, OperationOptions options, std::pmr::memory_resource* responseResource) {
    responseResource = httpPmrResourceOrDefault(responseResource);
    const OperationTimeout timeout(options.timeout.has_value() ? options.timeout : config_.requestTimeout);
    const auto acquireTimeout = timeout.constrainedBy(config_.acquireTimeout);
    if (requestsBuffered_ >= config_.maxBufferedRequestsPerWorker) {
        ++failedRequests_;
        throw HttpClientError(HttpClientError::Code::kQueueFull, "http client request buffer is full");
    }
    ++requestsBuffered_;
    std::size_t index = 0;
    try {
        index = co_await acquire(timeout, options.stopToken);
    } catch (...) {
        --requestsBuffered_;
        ++failedRequests_;
        throw;
    }
    --requestsBuffered_;
    ++requestsInFlight_;
    Lease lease(*this, index);
    auto& connection = lease.connection();
    bool discardConnection = true;
    try {
        co_await ensureConnected(connection, timeout, acquireTimeout, options.stopToken);
        HttpClientResponse response(responseResource);
        if (connection.protocol == WireProtocol::kHttp2) {
            discardConnection = false;
            response = co_await executeHttp2(connection, request, timeout, options.stopToken, responseResource);
        } else {
            // A negotiated HTTP/1 connection can have several operations that
            // already hold outer HTTP/2-capacity slots. Waiting for this
            // connection's single exchange slot does not own its socket: a
            // timeout/cancellation here must not close the exchange currently
            // using it.
            discardConnection = false;
            auto& runtime = *connection.http2Runtime;
            const bool mustBuffer = runtime.http1Operations != 0;
            if (mustBuffer && requestsBuffered_ >= config_.maxBufferedRequestsPerWorker) {
                throw HttpClientError(HttpClientError::Code::kQueueFull, "HTTP client request buffer is full");
            }
            ++runtime.http1Operations;
            if (mustBuffer) {
                --requestsInFlight_;
                ++requestsBuffered_;
            }
            struct H1Operation final {
                HttpClientPool& pool;
                Http2Runtime& runtime;
                bool buffered;
                ~H1Operation() {
                    if (buffered) {
                        --pool.requestsBuffered_;
                        ++pool.requestsInFlight_;
                    }
                    if (runtime.http1Operations == 0) std::terminate();
                    --runtime.http1Operations;
                }
            } h1Operation{*this, runtime, mustBuffer};
            auto h1Acquired = co_await connection.http2Runtime->http1Scheduler.acquire(
                acquireTimeout.remaining(), options.stopToken, worker_);
            if (h1Operation.buffered) {
                --requestsBuffered_;
                ++requestsInFlight_;
                h1Operation.buffered = false;
            }
            if (h1Acquired.timedOut()) throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP/1 connection acquire timed out");
            if (h1Acquired.cancelled()) throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP/1 request cancelled");
            if (!h1Acquired.acquired()) throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is closing");
            const auto h1Slot = h1Acquired.acquired()->index();
            struct H1Release final {
                PoolLeaseScheduler& scheduler;
                std::size_t slot;
                ~H1Release() { (void)scheduler.release(slot); }
            } h1Release{connection.http2Runtime->http1Scheduler, h1Slot};
            discardConnection = true;
            connection.abortReason = AbortReason::kNone;
            auto generation = ++connection.generation;
            if (generation == 0) generation = ++connection.generation;
            struct H1CancellationGeneration final {
                Connection& connection;
                ~H1CancellationGeneration() { ++connection.generation; }
            } cancellationGeneration{connection};
            connection.cancellationId = 0;
            std::uint64_t cancellationId = 0;
            StopRegistration stopRegistration;
            if (options.stopToken.stoppable()) {
                cancellationId = nextCancellationId();
                connection.cancellationId = cancellationId;
                options.stopToken.registerCallback(
                    stopRegistration,
                    WorkerCancellationPost<HttpClientOperationCancellationMailbox>(cancellationMailbox_, cancellationId));
            }
            struct H1CancellationRegistrationGuard final {
                Connection& connection;
                std::uint64_t cancellationId;
                StopRegistration& registration;

                ~H1CancellationRegistrationGuard() {
                    if (connection.cancellationId == cancellationId) {
                        connection.cancellationId = 0;
                    }
                    registration.reset();
                }
            } cancellationRegistrationGuard{connection, cancellationId, stopRegistration};
            if (options.stopToken.stopRequested()) cancelOperationById(cancellationId);
            response = co_await executeHttp1(connection, request, timeout, responseResource);
        }
        retainResponseCookies(request, response);
        --requestsInFlight_;
        ++completedRequests_;
        co_return response;
    } catch (...) {
        --requestsInFlight_;
        ++failedRequests_;
        if (discardConnection) lease.discard();
        throw;
    }
}

}  // namespace ruvia::detail
