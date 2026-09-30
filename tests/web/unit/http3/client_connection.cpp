#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http3/Http3ClientConnection.h"
#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {
using Connection = ruvia::detail::Http3ClientConnection;
using namespace std::chrono_literals;

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t liveBytes{};
    std::size_t rejectedAllocations{};
    struct Allocation final {
        std::size_t bytes{};
        std::size_t alignment{};
        friend bool operator==(const Allocation&, const Allocation&) = default;
    };

    void beginTrace() noexcept {
        firstAllocation_.reset();
    }
    [[nodiscard]] std::optional<Allocation> firstAllocation() const noexcept {
        return firstAllocation_;
    }
    void failFirstAllocation(Allocation allocation) noexcept {
        failedAllocation_ = allocation;
        failMatchingAllocation_ = true;
    }
    [[nodiscard]] Allocation lastRejectedAllocation() const noexcept {
        return lastRejectedAllocation_;
    }
    [[nodiscard]] std::size_t matchingAllocationAttempts() const noexcept {
        return matchingAllocationAttempts_;
    }
    void rejectAllocations(bool reject = true) noexcept {
        rejecting_ = reject;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        const Allocation allocation{bytes, alignment};
        if (!firstAllocation_) {
            firstAllocation_ = allocation;
        }
        if (failedAllocation_ && allocation == *failedAllocation_) {
            ++matchingAllocationAttempts_;
            if (failMatchingAllocation_) {
                failMatchingAllocation_ = false;
                ++rejectedAllocations;
                lastRejectedAllocation_ = allocation;
                throw std::bad_alloc();
            }
        }
        if (rejecting_) {
            ++rejectedAllocations;
            throw std::bad_alloc();
        }
        auto* address = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        liveBytes += bytes;
        return address;
    }
    void do_deallocate(void* address, std::size_t bytes, std::size_t alignment) override {
        ++returns;
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(address, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return &other == this;
    }

    std::optional<Allocation> firstAllocation_;
    std::optional<Allocation> failedAllocation_;
    Allocation lastRejectedAllocation_{};
    std::size_t matchingAllocationAttempts_{};
    bool failMatchingAllocation_{};
    bool rejecting_{};
};

struct Observation final {
    bool coldCancelled{};
    bool runningCancelled{};
    bool joined{};
    bool storageReleased{};
    bool invalidIdleTimeoutRejected{};
};

class TestIdentityFiles final {
public:
    TestIdentityFiles() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-h3-client-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create HTTP/3 test identity directory");
        }
        try {
            EVP_PKEY_CTX* rawContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
            if (rawContext == nullptr) {
                throw std::runtime_error("failed to create test key generator");
            }
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
                rawContext, EVP_PKEY_CTX_free);
            EVP_PKEY* rawKey = nullptr;
            if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
                EVP_PKEY_keygen(context.get(), &rawKey) <= 0) {
                throw std::runtime_error("failed to generate HTTP/3 test key");
            }
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
            std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
            if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
                ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
                X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) == nullptr ||
                X509_gmtime_adj(X509_get_notAfter(certificate.get()), 86400) == nullptr ||
                X509_set_pubkey(certificate.get(), key.get()) != 1 ||
                X509_set_issuer_name(certificate.get(), X509_get_subject_name(certificate.get())) != 1 ||
                X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
                throw std::runtime_error("failed to create HTTP/3 test certificate");
            }
            certificateFile_ = directory_ / "cert.pem";
            privateKeyFile_ = directory_ / "key.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> certBio(
                BIO_new_file(certificateFile_.string().c_str(), "w"), BIO_free);
            std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
                BIO_new_file(privateKeyFile_.string().c_str(), "w"), BIO_free);
            if (!certBio || !keyBio || PEM_write_bio_X509(certBio.get(), certificate.get()) != 1 ||
                PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
                throw std::runtime_error("failed to write HTTP/3 test identity");
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
            throw;
        }
    }

    ~TestIdentityFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& certificate() const noexcept {
        return certificateFile_;
    }
    [[nodiscard]] const std::filesystem::path& privateKey() const noexcept {
        return privateKeyFile_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificateFile_;
    std::filesystem::path privateKeyFile_;
};

[[nodiscard]] ruvia::detail::Http3QuicDatagramAddress testDatagramAddress(
    const asio::ip::udp::endpoint& endpoint) {
    if (!endpoint.address().is_v4()) {
        throw std::runtime_error("HTTP/3 test peer requires an IPv4 endpoint");
    }
    ruvia::detail::Http3QuicDatagramAddress result;
    const auto bytes = endpoint.address().to_v4().to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.address.begin());
    result.port = endpoint.port();
    return result;
}

[[nodiscard]] std::vector<char> testHttp3Frame(
    std::uint64_t type, std::span<const char> payload) {
    std::array<char, 16> header{};
    const auto typeSize = ruvia::encodeHttp3VarInt(header, type);
    if (!typeSize) {
        throw std::runtime_error("failed to encode HTTP/3 test frame type");
    }
    const auto payloadSize = ruvia::encodeHttp3VarInt(
        std::span<char>(header).subspan(*typeSize), payload.size());
    if (!payloadSize) {
        throw std::runtime_error("failed to encode HTTP/3 test frame length");
    }
    std::vector<char> output(header.begin(), header.begin() + *typeSize + *payloadSize);
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

class LocalQuicResponsePeer final {
public:
    using Server = ruvia::detail::Http3QuicServerTransport;
    using StreamId = Server::StreamId;

    explicit LocalQuicResponsePeer(const TestIdentityFiles& identity,
        bool malformedTail = false, bool consumeRequest = true)
        : socket_(io_, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0)),
          endpoint_(socket_.local_endpoint()),
          consumeRequest_(consumeRequest) {
        socket_.non_blocking(true);
        ruvia::detail::HttpServerListenerDefinition::Tls tls;
        tls.identity.certificateChainFile = identity.certificate().string();
        tls.identity.privateKeyFile = identity.privateKey().string();
        const std::string_view contentLength = malformedTail ? "67" : "6";
        ruvia::Http3FieldSectionFieldView fields[]{{":status", "200"}, {"content-length", contentLength}};
        std::pmr::monotonic_buffer_resource temporary;
        const auto encodedHead = ruvia::encodeHttp3FieldSection(fields, &temporary);
        if (!encodedHead) {
            throw std::runtime_error("failed to encode HTTP/3 test response head");
        }
        firstPart_ = testHttp3Frame(1, *encodedHead);
        const auto firstData = testHttp3Frame(0, std::span<const char>("abc", 3));
        firstPart_.insert(firstPart_.end(), firstData.begin(), firstData.end());
        if (malformedTail) {
            const std::string body(64, 'x');
            finalPart_ = testHttp3Frame(0, std::span<const char>(body.data(), body.size()));
            const auto unexpected = testHttp3Frame(4, {});
            finalPart_.insert(finalPart_.end(), unexpected.begin(), unexpected.end());
        } else {
            finalPart_ = testHttp3Frame(0, std::span<const char>("def", 3));
        }
        thread_ = std::thread([this, tls = std::move(tls)]() mutable { run(std::move(tls)); });
        std::unique_lock lock(mutex_);
        if (!startedCondition_.wait_for(lock, 5s, [this] { return started_ || failure_ != nullptr; })) {
            lock.unlock();
            stop_.store(true, std::memory_order_release);
            thread_.join();
            throw std::runtime_error("HTTP/3 local peer startup watchdog expired");
        }
        const auto failure = failure_;
        lock.unlock();
        if (failure != nullptr) {
            thread_.join();
            std::rethrow_exception(failure);
        }
    }

    ~LocalQuicResponsePeer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LocalQuicResponsePeer(const LocalQuicResponsePeer&) = delete;
    LocalQuicResponsePeer& operator=(const LocalQuicResponsePeer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }
    [[nodiscard]] bool firstPartReady() const noexcept {
        return firstPartReady_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool finalPartSent() const noexcept {
        return finalPartSent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool handshakeObserved() const noexcept {
        return handshakeObserved_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t requestPayloadBytes() const noexcept {
        return requestPayloadBytes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool synchronize() {
        std::unique_lock lock(mutex_);
        const auto generation = ++synchronizationRequested_;
        if (!startedCondition_.wait_for(lock, 5s, [this, generation] {
                return synchronizationCompleted_ >= generation || failure_ != nullptr;
            })) {
            return false;
        }
        const auto failure = failure_;
        lock.unlock();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return true;
    }
    void allowFinalPart() noexcept {
        allowFinalPart_.store(true, std::memory_order_release);
    }
    void rethrowIfFailed() const {
        std::exception_ptr failure;
        {
            std::lock_guard lock(mutex_);
            failure = failure_;
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

private:
    void run(ruvia::detail::HttpServerListenerDefinition::Tls tls) noexcept {
        try {
            std::pmr::unsynchronized_pool_resource resource;
            ruvia::detail::Http3QuicTlsContext tlsContext(tls, &resource);
            ruvia::detail::Http3QuicDatagramBridge bridge(testDatagramAddress(endpoint_));
            Server server(tlsContext, bridge);
            auto prefixes = ruvia::Http3LocalCriticalStreams::create();
            if (!prefixes) {
                throw std::runtime_error("failed to create local HTTP/3 critical stream prefixes");
            }
            {
                std::lock_guard lock(mutex_);
                started_ = true;
            }
            startedCondition_.notify_all();

            std::optional<Server::ConnectionId> connectionId;
            std::array<std::optional<StreamId>, 3> localCriticalIds;
            std::array<std::size_t, 3> localCriticalOffsets{};
            std::array<std::span<const char>, 3> localCriticalBytes{
                prefixes->controlPrefix(), prefixes->qpackEncoderPrefix(), prefixes->qpackDecoderPrefix()};
            std::optional<StreamId> requestStream;
            std::optional<asio::ip::udp::endpoint> remoteEndpoint;
            bool requestFinished = false;
            bool firstPrepared = false;
            std::size_t firstOffset = 0;
            std::size_t finalOffset = 0;
            bool finalFinSent = false;
            bool outboundPending = false;
            ruvia::detail::Http3QuicOutboundDatagram outbound;
            std::array<std::byte, 65536> input{};
            std::array<char, 4096> requestBytes{};

            while (!stop_.load(std::memory_order_acquire)) {
                for (unsigned packet = 0; packet != 32; ++packet) {
                    asio::ip::udp::endpoint source;
                    asio::error_code error;
                    const auto size = socket_.receive_from(asio::buffer(input), source, 0, error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error) {
                        throw std::system_error(error, "HTTP/3 test peer receive");
                    }
                    if (size == 0) {
                        continue;
                    }
                    remoteEndpoint = source;
                    const auto injected = bridge.inject(
                        std::span<const std::byte>(input.data(), size), testDatagramAddress(source));
                    if (injected != ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted) {
                        throw std::runtime_error("HTTP/3 test peer datagram queue rejected input");
                    }
                }
                if (server.handleEvents() == Server::EventResult::kFatal) {
                    throw std::runtime_error("HTTP/3 local peer event processing failed");
                }
                if (!connectionId) {
                    const auto accepted = server.acceptConnections(1);
                    if (!accepted.empty()) {
                        connectionId = accepted.ids[0];
                    }
                }
                if (connectionId) {
                    const auto info = server.connectionInfo(*connectionId);
                    if (info && info->handshakeComplete && info->h3Negotiated) {
                        handshakeObserved_.store(true, std::memory_order_release);
                        for (std::size_t i = 0; i < localCriticalIds.size(); ++i) {
                            if (!localCriticalIds[i]) {
                                const auto opened = server.openLocalUnidirectionalStream(*connectionId);
                                if (opened.error != Server::Error::kNone) {
                                    throw std::runtime_error("HTTP/3 local peer critical stream open failed");
                                }
                                localCriticalIds[i] = opened.id;
                            }
                            auto& offset = localCriticalOffsets[i];
                            if (offset < localCriticalBytes[i].size()) {
                                const auto written = server.writeStream(*connectionId, *localCriticalIds[i],
                                    localCriticalBytes[i].subspan(offset));
                                if (written.status == Server::StreamWrite::Status::kAccepted) {
                                    offset += written.bytes;
                                } else if (written.status != Server::StreamWrite::Status::kWouldBlock) {
                                    throw std::runtime_error("HTTP/3 local peer critical stream write failed");
                                }
                            }
                        }
                        auto accepted = server.acceptStreams(*connectionId);
                        if (accepted.error != Server::Error::kNone &&
                            accepted.error != Server::Error::kHandshakePending &&
                            accepted.error != Server::Error::kStreamLimitRetry) {
                            throw std::runtime_error("HTTP/3 local peer stream acceptance failed");
                        }
                        for (std::size_t i = 0; i < accepted.size; ++i) {
                            const auto& stream = accepted.streams[i];
                            if (stream.readable && stream.writeable) {
                                requestStream = stream.id;
                            }
                        }
                        if (requestStream && !requestFinished && consumeRequest_) {
                            for (;;) {
                                const auto read = server.readStream(
                                    *connectionId, *requestStream, requestBytes);
                                if (read.status == Server::StreamRead::Status::kData) {
                                    requestPayloadBytes_.fetch_add(read.size, std::memory_order_release);
                                    continue;
                                }
                                if (read.status == Server::StreamRead::Status::kFin) {
                                    requestFinished = true;
                                } else if (read.status != Server::StreamRead::Status::kWouldBlock) {
                                    throw std::runtime_error("HTTP/3 local peer request stream read failed");
                                }
                                break;
                            }
                        }
                        if (requestFinished && !firstPrepared) {
                            firstPrepared = true;
                        }
                        if (firstPrepared && firstOffset < firstPart_.size()) {
                            const auto written = server.writeStream(*connectionId, *requestStream,
                                std::span<const char>(firstPart_).subspan(firstOffset));
                            if (written.status == Server::StreamWrite::Status::kAccepted) {
                                firstOffset += written.bytes;
                                if (firstOffset == firstPart_.size()) {
                                    firstPartReady_.store(true, std::memory_order_release);
                                }
                            } else if (written.status != Server::StreamWrite::Status::kWouldBlock) {
                                throw std::runtime_error("HTTP/3 local peer initial response write failed");
                            }
                        }
                        if (firstPartReady() && allowFinalPart_.load(std::memory_order_acquire) &&
                            finalOffset < finalPart_.size()) {
                            const auto written = server.writeStream(*connectionId, *requestStream,
                                std::span<const char>(finalPart_).subspan(finalOffset));
                            if (written.status == Server::StreamWrite::Status::kAccepted) {
                                finalOffset += written.bytes;
                            } else if (written.status != Server::StreamWrite::Status::kWouldBlock) {
                                throw std::runtime_error("HTTP/3 local peer final response write failed");
                            }
                        }
                        if (finalOffset == finalPart_.size() && !finalFinSent) {
                            const auto finished = server.finishStream(*connectionId, *requestStream);
                            if (finished == Server::Error::kNone) {
                                finalFinSent = true;
                                finalPartSent_.store(true, std::memory_order_release);
                            } else if (finished != Server::Error::kWouldBlock) {
                                throw std::runtime_error("HTTP/3 local peer response FIN failed");
                            }
                        }
                    }
                }
                for (unsigned packet = 0; packet != 32; ++packet) {
                    if (!outboundPending) {
                        const auto result = bridge.takeOutbound(outbound);
                        if (result == ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty) {
                            break;
                        }
                        if (result == ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kBusy) {
                            throw std::runtime_error("HTTP/3 local peer found a concurrent outbound datagram");
                        }
                        if (result != ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kReady) {
                            throw std::runtime_error("HTTP/3 local peer outbound datagram failed");
                        }
                        outboundPending = true;
                    }
                    asio::error_code error;
                    if (!remoteEndpoint) {
                        throw std::runtime_error("HTTP/3 test peer has no datagram destination");
                    }
                    const auto sent = socket_.send_to(asio::buffer(outbound.bytes.data(), outbound.bytes.size()),
                        *remoteEndpoint, 0, error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error || sent != outbound.bytes.size()) {
                        throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                            "HTTP/3 test peer send");
                    }
                    bridge.completeOutbound();
                    outboundPending = false;
                }
                {
                    std::lock_guard lock(mutex_);
                    synchronizationCompleted_ = synchronizationRequested_;
                }
                startedCondition_.notify_all();
                std::this_thread::sleep_for(1ms);
            }
            if (connectionId) {
                (void)server.retireConnectionLocally(*connectionId);
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
                started_ = true;
            }
            startedCondition_.notify_all();
        }
    }

    asio::io_context io_;
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint endpoint_;
    std::vector<char> firstPart_;
    std::vector<char> finalPart_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable startedCondition_;
    std::exception_ptr failure_;
    bool started_{};
    std::uint64_t synchronizationRequested_{};
    std::uint64_t synchronizationCompleted_{};
    std::atomic<bool> stop_{};
    std::atomic<bool> handshakeObserved_{};
    std::atomic<std::size_t> requestPayloadBytes_{};
    std::atomic<bool> firstPartReady_{};
    std::atomic<bool> allowFinalPart_{};
    std::atomic<bool> finalPartSent_{};
    bool consumeRequest_{};
};

template <typename Predicate>
ruvia::Task<bool> waitForPeer(const ruvia::WorkerHandle& worker, LocalQuicResponsePeer& peer,
    Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        peer.rethrowIfFailed();
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        if (co_await ruvia::sleepFor(worker, 1ms) != ruvia::TimerSleepResult::kElapsed) {
            co_return false;
        }
    }
    peer.rethrowIfFailed();
    co_return true;
}

class ConnectionWatchdog final {
    struct State final {
        Connection* connection{};
        bool expired{};
    };

public:
    ConnectionWatchdog(asio::io_context& io, Connection& connection)
        : timer_(io),
          state_(std::make_shared<State>(State{&connection, false})) {
        timer_.expires_after(20s);
        timer_.async_wait([state = state_](const asio::error_code& error) noexcept {
            if (!error && state->connection != nullptr) {
                state->expired = true;
                state->connection->requestStop();
            }
        });
    }

    ~ConnectionWatchdog() {
        disarm();
    }

    ConnectionWatchdog(const ConnectionWatchdog&) = delete;
    ConnectionWatchdog& operator=(const ConnectionWatchdog&) = delete;

    void disarm() noexcept {
        state_->connection = nullptr;
        asio::error_code ignored;
        timer_.cancel(ignored);
    }

    [[nodiscard]] bool expired() const noexcept {
        return state_->expired;
    }

private:
    asio::steady_timer timer_;
    std::shared_ptr<State> state_;
};

ruvia::Task<void> exerciseRealResponse(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, LocalQuicResponsePeer& peer,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState response(worker, &memory);
            response.bufferedLimit = 6;
            ruvia::detail::HttpClientRequestStorage request("GET", "/incremental", &memory);
            const auto deadline = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(response.references == 2);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool gotHead = co_await waitForPeer(worker, peer, [&] { return peer.firstPartReady() && response.headReady; }, 8s);
                RUVIA_CHECK(gotHead);
                if (gotHead) {
                    RUVIA_CHECK(response.responseBodyPlan.has_value());
                    RUVIA_CHECK_EQ(response.status.value(), 200);
                    RUVIA_CHECK_EQ(response.headers.size(), std::size_t{1});
                    if (!response.headers.empty()) {
                        RUVIA_CHECK_EQ(response.headers.front().name(), std::string_view("content-length"));
                        RUVIA_CHECK_EQ(response.headers.front().value(), std::string_view("6"));
                    }

                    auto first = co_await response.read<std::string_view>();
                    RUVIA_CHECK(first.has_value());
                    if (first) {
                        RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                        peer.allowFinalPart();
                        const bool gotFinal = co_await waitForPeer(worker, peer, [&] { return peer.finalPartSent() && response.pending.size() == 3; }, 3s);
                        RUVIA_CHECK(gotFinal);
                        RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                        if (gotFinal) {
                            auto second = co_await response.read<std::string_view>();
                            RUVIA_CHECK(second.has_value());
                            if (second) {
                                RUVIA_CHECK_EQ(*second, std::string_view("def"));
                            }
                            co_await connection.wait(submitted.id);
                            RUVIA_CHECK(response.complete);
                            const auto completed = connection.result(submitted.id);
                            RUVIA_CHECK(completed != nullptr);
                            if (completed != nullptr) {
                                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
                                RUVIA_CHECK(completed->responseBodyPlan.has_value());
                                RUVIA_CHECK_EQ(completed->status, std::uint16_t{200});
                            }
                        }
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }

            connection.requestStop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            connection.consumerReleased(submitted.id);
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            RUVIA_CHECK(response.references == 1);
            RUVIA_CHECK(response.http3Connection == nullptr && response.http3RequestId == 0);
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseResponseReleaseAcrossGenerations(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    LocalQuicResponsePeer& firstPeer, LocalQuicResponsePeer& secondPeer,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource firstMemory;
        CountingResource secondMemory;
        {
            ruvia::detail::Http3ClientBodyBudget bodyBudget(7);
            {
                ruvia::detail::HttpClientResponseState firstState(worker, &firstMemory);
                ruvia::detail::HttpClientResponseState secondState(worker, &secondMemory);
                std::string_view bodyView;
                std::string_view headerView;
                std::string_view trailerView;
                {
                    ruvia::detail::ClientTransportConfigView config{
                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
                    ruvia::detail::Http3QuicClientTlsContext tls(config);
                    ruvia::TaskScope tasks(worker, {.resource = &firstMemory});
                    {
                        Connection connection(io, worker, tasks, tls,
                            ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = firstPeer.port()}),
                            10s, &firstMemory, 4, 16 * 1024 * 1024, 30s, &bodyBudget);
                        ConnectionWatchdog watchdog(io, connection);
                        firstState.bufferedLimit = 7;
                        const auto submitted = connection.submit(
                            ruvia::detail::HttpClientRequestStorage("GET", "/retain-response", &firstMemory),
                            firstState);
                        RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
                        connection.start();

                        std::exception_ptr failure;
                        try {
                            const bool gotHead = co_await waitForPeer(worker, firstPeer, [&] { return firstPeer.firstPartReady() && firstState.headReady; }, 8s);
                            RUVIA_CHECK(gotHead);
                            firstPeer.allowFinalPart();
                            const bool completed = co_await waitForPeer(worker, firstPeer, [&] { return firstPeer.finalPartSent() && firstState.complete; }, 5s);
                            RUVIA_CHECK(completed);
                        } catch (...) {
                            failure = std::current_exception();
                        }

                        connection.requestStop();
                        try {
                            co_await connection.wait(submitted.id);
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        try {
                            co_await tasks.join();
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        watchdog.disarm();
                        RUVIA_CHECK(!watchdog.expired());
                        RUVIA_CHECK(connection.result(submitted.id) != nullptr);
                        RUVIA_CHECK(firstState.complete);
                        RUVIA_CHECK_EQ(firstState.status.value(), 200);
                        RUVIA_CHECK_EQ(firstState.protocolVersion, ruvia::HttpProtocolVersion::kHttp3);
                        RUVIA_CHECK_EQ(firstState.headers.size(), std::size_t{1});
                        RUVIA_CHECK_EQ(firstState.pending, std::string_view("abcdef"));
                        firstState.trailers.push_back(
                            ruvia::HttpHeader::copyOf("x-retained", "trailer", &firstMemory));
                        headerView = firstState.headers.front().value();
                        trailerView = firstState.trailers.front().value();
                        RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{6});
                        RUVIA_CHECK(connection.releaseResponseRequest(submitted.id));
                        RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{6});
                        RUVIA_CHECK(!firstState.errorCode && firstState.failure == nullptr);
                        RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
                        RUVIA_CHECK_EQ(firstState.references, std::size_t{1});
                        RUVIA_CHECK(firstState.http3Connection == nullptr);
                        if (failure != nullptr) {
                            std::rethrow_exception(failure);
                        }
                    }
                }

                RUVIA_CHECK(firstState.complete);
                RUVIA_CHECK_EQ(firstState.status.value(), 200);
                RUVIA_CHECK_EQ(firstState.protocolVersion, ruvia::HttpProtocolVersion::kHttp3);
                RUVIA_CHECK_EQ(firstState.headers.front().value(), headerView);
                RUVIA_CHECK_EQ(firstState.trailers.front().value(), trailerView);
                {
                    auto firstBodyRead = co_await firstState.read<std::string_view>();
                    RUVIA_CHECK(firstBodyRead.has_value());
                    if (firstBodyRead) {
                        bodyView = *firstBodyRead;
                        RUVIA_CHECK_EQ(bodyView, std::string_view("abcdef"));
                    }
                }
                RUVIA_CHECK_EQ(firstState.offset, std::size_t{6});
                RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{6});
                RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{6});

                {
                    ruvia::detail::ClientTransportConfigView config{
                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
                    ruvia::detail::Http3QuicClientTlsContext tls(config);
                    ruvia::TaskScope tasks(worker, {.resource = &secondMemory});
                    {
                        Connection connection(io, worker, tasks, tls,
                            ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = secondPeer.port()}),
                            10s, &secondMemory, 4, 16 * 1024 * 1024, 30s, &bodyBudget);
                        ConnectionWatchdog watchdog(io, connection);
                        const auto submitted = connection.submit(
                            ruvia::detail::HttpClientRequestStorage("GET", "/next-generation", &secondMemory),
                            secondState);
                        RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
                        connection.start();

                        std::exception_ptr failure;
                        try {
                            const bool blockedAtBudget = co_await waitForPeer(worker, secondPeer, [&] { return secondPeer.firstPartReady() && secondState.headReady &&
                                                                                                               secondState.producerBodyBytes() == 1; }, 8s);
                            RUVIA_CHECK(blockedAtBudget);
                            RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{7});
                            RUVIA_CHECK_EQ(bodyView, std::string_view("abcdef"));
                            RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{6});
                            bodyView = {};
                            firstState.releaseConsumedBodyPrefix();
                            RUVIA_CHECK(firstState.buffered.empty());
                            RUVIA_CHECK_EQ(firstState.offset, std::size_t{0});
                            RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{0});
                            RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{1});
                            secondPeer.allowFinalPart();
                            const bool completed = co_await waitForPeer(worker, secondPeer, [&] { return secondPeer.finalPartSent() && secondState.complete; }, 5s);
                            RUVIA_CHECK(completed);
                        } catch (...) {
                            failure = std::current_exception();
                        }

                        connection.requestStop();
                        try {
                            co_await connection.wait(submitted.id);
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        try {
                            co_await tasks.join();
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        watchdog.disarm();
                        RUVIA_CHECK(!watchdog.expired());
                        RUVIA_CHECK(connection.result(submitted.id) != nullptr);
                        RUVIA_CHECK(connection.releaseResponseRequest(submitted.id));
                        RUVIA_CHECK_EQ(secondState.http3BodyBudget.retainedBytes(), std::size_t{6});
                        RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
                        RUVIA_CHECK_EQ(secondState.references, std::size_t{1});
                        if (failure != nullptr) {
                            std::rethrow_exception(failure);
                        }
                    }
                }
                RUVIA_CHECK(secondState.complete);
                RUVIA_CHECK_EQ(secondState.pending, std::string_view("abcdef"));
                RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{6});
                secondState.discardResponseBody();
                RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{0});
            }
        }
        RUVIA_CHECK_EQ(firstMemory.allocations, firstMemory.returns);
        RUVIA_CHECK_EQ(firstMemory.liveBytes, std::size_t{0});
        RUVIA_CHECK_EQ(secondMemory.allocations, secondMemory.returns);
        RUVIA_CHECK_EQ(secondMemory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseConnectionErrorPriority(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    LocalQuicResponsePeer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState response(worker, &memory);
            ruvia::detail::HttpClientRequestStorage request("GET", "/protocol-priority", &memory);
            const auto deadline = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool gotHead = co_await waitForPeer(worker, peer, [&] { return peer.firstPartReady() && response.headReady; }, 8s);
                RUVIA_CHECK(gotHead);
                if (gotHead) {
                    RUVIA_CHECK_EQ(response.pending, std::string_view("abc"));
                    memory.rejectAllocations();
                    peer.allowFinalPart();
                    const bool terminal = co_await waitForPeer(
                        worker, peer, [&] { return response.complete; }, 4s);
                    RUVIA_CHECK(terminal);
                    if (terminal) {
                        RUVIA_CHECK(memory.rejectedAllocations != 0);
                        RUVIA_CHECK(response.errorCode.has_value());
                        RUVIA_CHECK_EQ(*response.errorCode,
                            static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kProtocolError));
                        RUVIA_CHECK(response.failure == nullptr);
                        const auto result = connection.result(submitted.id);
                        RUVIA_CHECK(result != nullptr);
                        if (result != nullptr) {
                            RUVIA_CHECK(result->outcome == Connection::Outcome::kProtocolError);
                        }
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }

            memory.rejectAllocations(false);
            connection.requestStop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            response.discardResponseBody();
            RUVIA_CHECK(response.errorCode.has_value());
            RUVIA_CHECK_EQ(*response.errorCode,
                static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kProtocolError));
            RUVIA_CHECK(connection.releaseResponseRequest(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            RUVIA_CHECK(response.references == 1);
            RUVIA_CHECK(response.failure == nullptr);
            RUVIA_CHECK_EQ(response.protocolVersion, ruvia::HttpProtocolVersion::kHttp3);
            RUVIA_CHECK_EQ(response.status.value(), 200);
            RUVIA_CHECK_EQ(response.headers.size(), std::size_t{1});
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseParserRegistrationAllocationFailure(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    LocalQuicResponsePeer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource registrationMemory;
        CountingResource::Allocation registrationAllocation{};
        {
            ruvia::detail::Http3ClientSansIoSessionEngine parser(&registrationMemory);
            registrationMemory.beginTrace();
            const auto registered = parser.registerRequest(0, ruvia::HttpKnownMethod::kGet);
            RUVIA_CHECK(registered.scope == ruvia::Http3ConnectionErrorScope::kNone);
            const auto allocation = registrationMemory.firstAllocation();
            RUVIA_CHECK(allocation.has_value());
            if (!allocation) {
                throw std::runtime_error("HTTP/3 parser registration made no PMR allocation");
            }
            registrationAllocation = *allocation;
            (void)parser.stop();
        }
        RUVIA_CHECK_EQ(registrationMemory.allocations, registrationMemory.returns);
        RUVIA_CHECK_EQ(registrationMemory.liveBytes, std::size_t{0});

        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            const auto first = connection.submit(
                ruvia::detail::HttpClientRequestStorage("GET", "/parser-allocation", &memory));
            const auto sibling = connection.submit(
                ruvia::detail::HttpClientRequestStorage("GET", "/sibling-must-not-write", &memory));
            RUVIA_CHECK(first.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(sibling.outcome == Connection::Outcome::kPending);
            // registerRequest()'s first allocation is inside responses_.try_emplace().
            // The request driver opens its QUIC stream before invoking this callback,
            // and emits HEADERS only after it returns.
            memory.failFirstAllocation(registrationAllocation);
            connection.start();

            std::exception_ptr failure;
            try {
                co_await connection.wait(first.id);
                co_await connection.wait(sibling.id);
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure != nullptr) {
                connection.requestStop();
            }
            try {
                co_await tasks.join();
            } catch (...) {
                if (failure == nullptr) {
                    failure = std::current_exception();
                }
            }
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            RUVIA_CHECK(memory.rejectedAllocations == 1);
            RUVIA_CHECK(memory.lastRejectedAllocation() == registrationAllocation);
            RUVIA_CHECK(memory.matchingAllocationAttempts() == 1);
            RUVIA_CHECK(!connection.running());
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK(peer.handshakeObserved());
            RUVIA_CHECK_EQ(peer.requestPayloadBytes(), std::size_t{0});

            const auto firstResult = connection.result(first.id);
            const auto siblingResult = connection.result(sibling.id);
            RUVIA_CHECK(firstResult != nullptr &&
                        firstResult->outcome == Connection::Outcome::kTransportError);
            RUVIA_CHECK(siblingResult != nullptr &&
                        siblingResult->outcome == Connection::Outcome::kTransportError);
            RUVIA_CHECK(firstResult != nullptr && !firstResult->responseBodyPlan);
            RUVIA_CHECK(siblingResult != nullptr && !siblingResult->responseBodyPlan);
            RUVIA_CHECK(connection.submit(ruvia::detail::HttpClientRequestStorage(
                                              "GET", "/must-not-reuse", &memory))
                            .outcome == Connection::Outcome::kConnectionDraining);
            RUVIA_CHECK(connection.release(first.id));
            RUVIA_CHECK(connection.release(sibling.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exercise(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, Observation& observed,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            try {
                Connection invalid(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}), 2s,
                    &memory, 4, 16 * 1024 * 1024, 0ms);
            } catch (const std::invalid_argument&) {
                observed.invalidIdleTimeoutRejected = true;
            }
            for (const auto invalid : {0ms, -1ms, std::chrono::milliseconds::max()}) {
                const auto baseline = memory.liveBytes;
                RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                    Connection rejected(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}),
                        invalid, &memory);
                }));
                RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                    Connection rejected(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}),
                        2s, &memory, 4, 16 * 1024 * 1024, invalid);
                }));
                RUVIA_CHECK_EQ(memory.liveBytes, baseline);
            }
            ruvia::detail::Http3ClientBodyBudget receiveBodyBudget(1024);
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}), 2s,
                &memory, 4, 16 * 1024 * 1024, 30s, &receiveBodyBudget);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState responseState(worker, &memory);
            ruvia::detail::HttpClientRequestStorage boundRequest("GET", "/response-state", &memory);
            const auto bound = connection.submit(std::move(boundRequest), responseState);
            RUVIA_CHECK(bound.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(responseState.references == 2);
            RUVIA_CHECK(responseState.hasHttp3BodyBudget());
            RUVIA_CHECK_EQ(receiveBodyBudget.used(), std::size_t{0});
            RUVIA_CHECK(responseState.transport == ruvia::detail::HttpClientResponseTransport::kHttp3);
            RUVIA_CHECK(responseState.http3Connection == &connection);
            bool boundWaiterDone = false;
            auto boundWaiter = [&]() -> ruvia::Task<void> {
                co_await connection.wait(bound.id);
                boundWaiterDone = true;
            };
            tasks.spawn(boundWaiter());
            RUVIA_CHECK(!connection.releaseResponseRequest(bound.id));
            RUVIA_CHECK(co_await ruvia::sleepFor(worker, 1ms) == ruvia::TimerSleepResult::kElapsed);
            RUVIA_CHECK(!boundWaiterDone);
            const auto retainedFailure =
                std::make_exception_ptr(std::runtime_error("preserved response failure"));
            connection.cancel(bound.id);
            responseState.failure = retainedFailure;
            RUVIA_CHECK(connection.result(bound.id) && responseState.complete);
            RUVIA_CHECK(!connection.releaseResponseRequest(bound.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{1});
            RUVIA_CHECK(responseState.references == 2);
            RUVIA_CHECK(co_await ruvia::sleepFor(worker, 1ms) == ruvia::TimerSleepResult::kElapsed);
            RUVIA_CHECK(boundWaiterDone);
            RUVIA_CHECK(connection.releaseResponseRequest(bound.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            RUVIA_CHECK(responseState.references == 1);
            RUVIA_CHECK(responseState.transport == ruvia::detail::HttpClientResponseTransport::kUnassigned);
            RUVIA_CHECK(responseState.http3Connection == nullptr && responseState.http3RequestId == 0);
            RUVIA_CHECK(!responseState.hasHttp3BodyBudget());
            RUVIA_CHECK(responseState.errorCode.has_value());
            RUVIA_CHECK_EQ(*responseState.errorCode,
                static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kCancelled));
            RUVIA_CHECK(responseState.complete);
            RUVIA_CHECK(responseState.failure == retainedFailure);
            RUVIA_CHECK_EQ(receiveBodyBudget.used(), std::size_t{0});

            ruvia::detail::HttpClientResponseState localState(worker, &memory);
            Connection localBudgetConnection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}), 2s,
                &memory, 4, 16 * 1024 * 1024);
            const auto local = localBudgetConnection.submit(
                ruvia::detail::HttpClientRequestStorage("GET", "/local-budget", &memory),
                localState);
            RUVIA_CHECK(local.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(!localBudgetConnection.releaseResponseRequest(local.id));
            localState.pending.assign("retained");
            RUVIA_CHECK(localState.replaceProducerBodyBytes(localState.pending.size()));
            localBudgetConnection.cancel(local.id);
            RUVIA_CHECK(localBudgetConnection.result(local.id) && localState.complete);
            RUVIA_CHECK(!localBudgetConnection.releaseResponseRequest(local.id));
            RUVIA_CHECK_EQ(localState.pending, std::string_view("retained"));
            localState.discardResponseBody();
            RUVIA_CHECK(localBudgetConnection.releaseResponseRequest(local.id));
            RUVIA_CHECK_EQ(localState.errorCode.value(),
                static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kCancelled));
            RUVIA_CHECK_EQ(localState.references, std::size_t{1});
            struct OriginCase {
                std::string_view host;
                std::uint16_t port;
                std::string_view authority;
            };
            constexpr std::array origins{
                OriginCase{"localhost", 443, "localhost"},
                OriginCase{"localhost", 49529, "localhost:49529"},
                OriginCase{"[::1]", 443, "[::1]"},
                OriginCase{"[::1]", 8443, "[::1]:8443"}};
            for (const auto& item : origins) {
                std::string host(item.host);
                Connection selected(io, worker, tasks, tls,
                    ruvia::HttpOriginView::https({.host = host, .port = item.port}), 2s,
                    &memory, 4, 16 * 1024 * 1024, 30s, &receiveBodyBudget);
                host.assign("modified-after-construction.invalid");
                ruvia::detail::HttpClientRequestStorage matching("GET", "/origin", &memory);
                matching.appendHeader("Host", item.authority);
                const auto admitted = selected.submit(std::move(matching));
                RUVIA_CHECK(admitted.outcome == Connection::Outcome::kPending);
                selected.cancel(admitted.id);
                RUVIA_CHECK(selected.release(admitted.id));
                ruvia::detail::HttpClientRequestStorage conflicting("GET", "/origin", &memory);
                conflicting.appendHeader("Host", "different.invalid");
                RUVIA_CHECK(selected.submit(std::move(conflicting)).outcome == Connection::Outcome::kInvalidRequest);
            }
            RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                Connection plain(io, worker, tasks, tls,
                    ruvia::HttpOriginView::http({.host = "localhost"}), 2s, &memory);
            }));
            const auto beforeExpired = memory.liveBytes;
            const auto expired = connection.submit(Connection::RejectedRequest{
                .request = ruvia::detail::HttpClientRequestStorage("POST", "/expired-handoff", &memory),
                .deadline = Connection::TimePoint::min()});
            RUVIA_CHECK(expired.outcome == Connection::Outcome::kDeadline);
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
            RUVIA_CHECK_EQ(memory.liveBytes, beforeExpired);
            ruvia::detail::HttpClientRequestStorage cold("GET", "/cold", &memory);
            const auto first = connection.submit(std::move(cold));
            RUVIA_CHECK(first.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(!connection.takeRejectedRequest(first.id));
            connection.cancel(first.id);
            RUVIA_CHECK(!connection.takeRejectedRequest(first.id));
            RUVIA_CHECK(!connection.releaseResponseRequest(first.id));
            RUVIA_CHECK(connection.result(first.id) &&
                        !connection.result(first.id)->responseBodyPlan);
            observed.coldCancelled = connection.result(first.id) &&
                                     connection.result(first.id)->outcome ==
                                         Connection::Outcome::kCancelled;
            RUVIA_CHECK(connection.release(first.id));
            RUVIA_CHECK(!connection.takeRejectedRequest(first.id));

            ruvia::detail::HttpClientRequestStorage active("GET", "/active", &memory);
            ruvia::detail::HttpClientRequestStorage queued("GET", "/queued", &memory);
            const auto second = connection.submit(std::move(active));
            const auto third = connection.submit(std::move(queued));
            RUVIA_CHECK(second.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(third.outcome == Connection::Outcome::kPending);
            connection.start();  // The sole Task is now waiting on DNS or UDP.
            connection.requestStop();
            co_await connection.wait(second.id);
            co_await connection.wait(third.id);
            observed.runningCancelled = connection.result(second.id) &&
                                        connection.result(third.id) &&
                                        connection.result(second.id)->outcome == Connection::Outcome::kCancelled &&
                                        connection.result(third.id)->outcome == Connection::Outcome::kCancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.joined = !connection.running();
            RUVIA_CHECK(connection.release(second.id));
            RUVIA_CHECK(connection.release(third.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseSocketStop(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, std::uint16_t port, Observation& observed,
    ruvia::testing::TestContext& ruvia_ctx, std::chrono::milliseconds timeout) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = port}), timeout,
                &memory, 32, 16 * 1024 * 1024, timeout);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage request("GET", "/blackhole", &memory);
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            RUVIA_CHECK(co_await ruvia::sleepFor(worker, 40ms) ==
                        ruvia::TimerSleepResult::kElapsed);
            observed.coldCancelled = connection.running() && !connection.result(submitted.id);
            connection.requestStop();
            co_await connection.wait(submitted.id);
            observed.runningCancelled = connection.result(submitted.id) &&
                                        connection.result(submitted.id)->outcome == Connection::Outcome::kCancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.joined = !connection.running();
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseTerminalTimeout(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, std::uint16_t port, Observation& observed,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = port}), 80ms, &memory);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage request("GET", "/timeout", &memory);
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            co_await connection.wait(submitted.id);
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.runningCancelled = connection.result(submitted.id) &&
                                        connection.result(submitted.id)->outcome == Connection::Outcome::kDeadline;
            ruvia::detail::HttpClientRequestStorage later("GET", "/later", &memory);
            observed.joined = !connection.running() &&
                              connection.submit(std::move(later)).outcome == Connection::Outcome::kConnectionDraining;
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
            RUVIA_CHECK_EQ(connection.retainedResultBodyBytes(), 0U);
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exercisePerRequestDeadline(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    std::uint16_t port, Observation& observed, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = port}), 2s, &memory);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage first("GET", "/first", &memory);
            ruvia::detail::HttpClientRequestStorage second("GET", "/short", &memory);
            const auto ongoing = connection.submit(std::move(first));
            const auto started = std::chrono::steady_clock::now();
            const auto shortWait = connection.submit(Connection::RejectedRequest{
                .request = std::move(second), .deadline = started + 35ms});
            RUVIA_CHECK(ongoing.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(shortWait.outcome == Connection::Outcome::kPending);
            connection.start();
            co_await connection.wait(shortWait.id);
            observed.coldCancelled = connection.result(shortWait.id) &&
                                     connection.result(shortWait.id)->outcome == Connection::Outcome::kDeadline &&
                                     !connection.result(ongoing.id) &&
                                     std::chrono::steady_clock::now() - started < 500ms;
            connection.requestStop();
            co_await connection.wait(ongoing.id);
            observed.runningCancelled = connection.result(ongoing.id) &&
                                        connection.result(ongoing.id)->outcome == Connection::Outcome::kCancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.joined = !connection.running();
            RUVIA_CHECK(connection.release(ongoing.id));
            RUVIA_CHECK(connection.release(shortWait.id));
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseWriteInactivityTimeout(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    LocalQuicResponsePeer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::Http3QuicClientTlsContext tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https(
                    {.host = "127.0.0.1", .port = peer.port()}),
                5s, &memory, 4, 16 * 1024 * 1024, 30s, nullptr, 120ms);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage request(
                "POST", "/write-timeout", &memory);
            const std::string body(4 * 1024 * 1024, 'x');
            request.setBody(body);
            const auto started = std::chrono::steady_clock::now();
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            co_await connection.wait(submitted.id);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const auto result = connection.result(submitted.id);
            RUVIA_CHECK(result != nullptr);
            if (result != nullptr) {
                RUVIA_CHECK(result->outcome == Connection::Outcome::kDeadline);
            }
            RUVIA_CHECK(elapsed >= 100ms && elapsed < 3s);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK(peer.handshakeObserved());
            connection.requestStop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exercisePublicHttp3ClientPool(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    LocalQuicResponsePeer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    CountingResource memory;
    std::exception_ptr failure;
    {
        ruvia::HttpClientConfig config{
            .scheme = ruvia::HttpScheme::kHttps,
            .host = "127.0.0.1",
            .port = peer.port(),
            .connectionCount = 1,
            .connectTimeout = 10s,
            .requestTimeout = 12s,
            .acquireTimeout = 2s,
            .maxResponseBytes = 64,
            .protocol = ruvia::HttpClientProtocol::kHttp3Only,
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
            .userAgent = "Ruvia-H3-Test",
        };
        ruvia::detail::HttpClientPool pool(io, worker,
            ruvia::detail::HttpClientConfigStorage(config, &memory),
            ruvia::HttpClientResultBudgetConfig{}, &memory);
        try {
            {
                ruvia::detail::HttpClientRequestStorage request("GET", "/public-pool", &memory);
                auto response = co_await pool.execute(std::move(request), {});
                RUVIA_CHECK(response.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
                RUVIA_CHECK_EQ(response.status().value(), 200);
                RUVIA_CHECK(peer.firstPartReady());
                RUVIA_CHECK(!peer.finalPartSent());

                auto first = co_await response.body().text();
                RUVIA_CHECK(first.has_value());
                if (first) {
                    RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                }
                peer.allowFinalPart();
                auto second = co_await response.body().text();
                RUVIA_CHECK(second.has_value());
                if (second) {
                    RUVIA_CHECK_EQ(*second, std::string_view("def"));
                }
                auto end = co_await response.body().text();
                RUVIA_CHECK(!end.has_value());
                RUVIA_CHECK(response.body().complete());
            }
            const bool completed = co_await waitForPeer(worker, peer, [&] { return pool.stats().completedRequests == 1; }, 2s);
            RUVIA_CHECK(completed);
            const auto stats = pool.stats();
            RUVIA_CHECK_EQ(stats.completedRequests, std::size_t{1});
            RUVIA_CHECK_EQ(stats.failedRequests, std::size_t{0});
            RUVIA_CHECK_EQ(stats.inFlightRequests, std::size_t{0});
        } catch (...) {
            failure = std::current_exception();
        }
        pool.closeNow();
        co_await pool.join();
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    attachment.stop();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(http3ClientConnectionResponsePlanMovesAcrossResourceHandoffAsAnOwnedValue) {
    CountingResource source;
    CountingResource target;
    {
        Connection::Response original(&source);
        original.outcome = Connection::Outcome::kComplete;
        original.status = 200;
        original.responseBodyPlan = ruvia::planHttpResponseBody(
            ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk);
        original.headers.push_back(ruvia::HttpHeader::copyOf("x-result", "owned", &source));
        original.trailers.push_back(ruvia::HttpHeader::copyOf("x-trailer", "owned", &source));
        original.body.assign("representation");

        Connection::Response moved(std::move(original));
        Connection::Response handedOff(&target);
        handedOff = std::move(moved);
        RUVIA_CHECK(handedOff.outcome == Connection::Outcome::kComplete);
        RUVIA_CHECK(handedOff.responseBodyPlan &&
                    handedOff.responseBodyPlan->requestMethod() == ruvia::HttpKnownMethod::kHead &&
                    handedOff.responseBodyPlan->responseStatus() == ruvia::http_status::kOk &&
                    handedOff.responseBodyPlan->bodySuppressed());
        RUVIA_CHECK(handedOff.headers.size() == 1 && handedOff.headers.front().value() == "owned");
        RUVIA_CHECK(handedOff.trailers.size() == 1 && handedOff.trailers.front().value() == "owned");
        RUVIA_CHECK(handedOff.body == "representation");
    }
    RUVIA_CHECK(source.allocations == source.returns && source.liveBytes == 0);
    RUVIA_CHECK(target.allocations == target.returns && target.liveBytes == 0);
}

RUVIA_TEST(http3ClientConnectionPublishesRealQuicResponseIncrementallyAndReclaimsRequestState) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    LocalQuicResponsePeer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseRealResponse(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientPoolPublishesIncrementalResponseThroughPublicResponseApi) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    LocalQuicResponsePeer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionResponseReleasePreservesDataAndWakesTheNextBudgetGeneration) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    LocalQuicResponsePeer firstPeer(identity);
    LocalQuicResponsePeer secondPeer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseResponseReleaseAcrossGenerations(
        io, worker, attachment, firstPeer, secondPeer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionParserRegistrationAllocationFailureClosesAndJoinsTheConnection) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    LocalQuicResponsePeer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exerciseParserRegistrationAllocationFailure(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionProtocolErrorWinsOverCallbackAllocationFailureInOneFeed) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    LocalQuicResponsePeer peer(identity, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseConnectionErrorPriority(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionWriteInactivityTimeoutStopsAFlowControlledRequest) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    LocalQuicResponsePeer peer(identity, false, false);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exerciseWriteInactivityTimeout(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionColdCancelAndStartedDriverStopJoinAreWorkerOwned) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    auto root = attachment.loop().start(exercise(io, worker, attachment, observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.coldCancelled && observed.runningCancelled && observed.joined);
    RUVIA_CHECK(observed.storageReleased);
    RUVIA_CHECK(observed.invalidIdleTimeoutRejected);
#endif
}

RUVIA_TEST(http3ClientConnectionRequestDeadlineDoesNotFailOtherPendingRequests) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    auto root = attachment.loop().start(exercisePerRequestDeadline(io, worker, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.coldCancelled && observed.runningCancelled && observed.joined);
    RUVIA_CHECK(observed.storageReleased);
#endif
}

RUVIA_TEST(http3ClientConnectionTerminalHandshakeTimeoutRejectsNewRequests) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    auto root = attachment.loop().start(exerciseTerminalTimeout(io, worker, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.runningCancelled && observed.joined && observed.storageReleased);
#endif
}

RUVIA_TEST(http3ClientConnectionStopWakesPendingQuicSocketWaitAndJoinsDriver) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    const auto largestTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max());
    auto root = attachment.loop().start(exerciseSocketStop(io, worker, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx, largestTimeout));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.coldCancelled && observed.runningCancelled && observed.joined);
    RUVIA_CHECK(observed.storageReleased);
#endif
}
