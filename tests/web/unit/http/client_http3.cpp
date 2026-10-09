#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
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
#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpClientTypes.h"

#include "http3/Http3QuicDatagramBridge.h"
#include "http3/Http3QuicServerTransport.h"
#include "http3/Http3QuicSocketAddress.h"
#include "http3/Http3QuicTlsContext.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace {
using namespace std::chrono_literals;
using Udp = asio::ip::udp;

class QuicUdpBlackhole final {
public:
    explicit QuicUdpBlackhole(asio::io_context& io)
        : socket_(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0)),
          endpoint_(socket_.local_endpoint()) {
        socket_.non_blocking(true);
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }

    [[nodiscard]] std::size_t datagrams() const noexcept {
        return datagrams_;
    }

    [[nodiscard]] std::size_t quicLongHeaders() const noexcept {
        return quicLongHeaders_;
    }

    [[nodiscard]] std::span<const Udp::endpoint> sources() const noexcept {
        return sources_;
    }

    void receiveAvailable() {
        std::array<std::byte, 65536> packet{};
        for (;;) {
            Udp::endpoint source;
            asio::error_code error;
            const auto size = socket_.receive_from(asio::buffer(packet), source, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "receive HTTP/3 client UDP packet");
            }
            ++datagrams_;
            if (size >= 7 && (std::to_integer<unsigned char>(packet.front()) & 0x80U) != 0) {
                ++quicLongHeaders_;
            }
            if (std::find(sources_.begin(), sources_.end(), source) == sources_.end()) {
                sources_.push_back(source);
            }
        }
    }

private:
    Udp::socket socket_;
    Udp::endpoint endpoint_;
    std::vector<Udp::endpoint> sources_;
    std::size_t datagrams_{};
    std::size_t quicLongHeaders_{};
};

struct SendResult final {
    bool finished{};
    bool returnedResponse{};
    std::optional<ruvia::HttpClientError::Code> error{};
};

ruvia::Task<void> sendAndCapture(ruvia::HttpClientHandle handle,
    const ruvia::HttpClientRequestView& request, SendResult& result) {
    try {
        auto response = co_await handle.send(request);
        result.returnedResponse = true;
        static_cast<void>(response);
    } catch (const ruvia::HttpClientError& error) {
        result.error = error.code();
    }
    result.finished = true;
}

ruvia::Task<bool> waitForDatagrams(const ruvia::WorkerHandle& worker,
    QuicUdpBlackhole& peer, std::size_t target, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (peer.datagrams() < target) {
        peer.receiveAvailable();
        if (peer.datagrams() >= target) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        if (co_await ruvia::sleepFor(worker, 1ms) != ruvia::TimerSleepResult::kElapsed) {
            co_return false;
        }
    }
    peer.receiveAvailable();
    co_return peer.datagrams() >= target;
}

ruvia::Task<bool> waitForCompletion(const ruvia::WorkerHandle& worker,
    QuicUdpBlackhole& peer, const SendResult& result, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!result.finished) {
        peer.receiveAvailable();
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        if (co_await ruvia::sleepFor(worker, 1ms) != ruvia::TimerSleepResult::kElapsed) {
            co_return false;
        }
    }
    peer.receiveAvailable();
    co_return true;
}

ruvia::Task<void> exerciseCrossThreadConstructedHttp3Client(
    ruvia::EventLoopAttachment& attachment, QuicUdpBlackhole& peer,
    ruvia::HttpClient& client, SendResult& result) {
    try {
        const auto worker = attachment.loop().handle();
        ruvia::StopSource stop;
        const ruvia::HttpClientRequestView request{
            .method = "GET", .target = "/cross-thread-owner"};
        {
            ruvia::TaskScope task(worker);
            task.spawn(sendAndCapture(client.withOptions(
                                          {.timeout = 5s, .stopToken = stop.token()}),
                request, result));
            const bool dispatched = co_await waitForDatagrams(worker, peer, 1, 2s);
            if (dispatched) {
                stop.requestStop();
                (void)co_await waitForCompletion(worker, peer, result, 2s);
            }
            task.requestStop();
            co_await task.join();
        }
        co_await client.shutdown();
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exercisePublicHttp3Dispatch(
    ruvia::EventLoopAttachment& attachment, QuicUdpBlackhole& peer,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        const auto worker = attachment.loop().handle();
        ruvia::HttpClient client(attachment.loop(), ruvia::HttpClientConfig{
                                                        .scheme = ruvia::HttpScheme::kHttps,
                                                        .host = "127.0.0.1",
                                                        .port = peer.port(),
                                                        .connectionCount = 2,
                                                        .connectTimeout = 2s,
                                                        .requestTimeout = 5s,
                                                        .acquireTimeout = 1s,
                                                        .maxResponseBytes = 4096,
                                                        .protocol = ruvia::HttpClientProtocol::kHttp3Only,
                                                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
                                                    });

        ruvia::StopSource firstStop;
        ruvia::StopSource secondStop;
        const auto firstHandle = client.withOptions(
            {.timeout = 5s, .stopToken = firstStop.token()});
        const auto secondHandle = client.withOptions(
            {.timeout = 5s, .stopToken = secondStop.token()});
        const ruvia::HttpClientRequestView firstRequest{.method = "GET", .target = "/slot-one"};
        const ruvia::HttpClientRequestView secondRequest{.method = "GET", .target = "/slot-two"};
        SendResult firstResult;
        SendResult secondResult;
        {
            ruvia::TaskScope requests(worker);
            requests.spawn(sendAndCapture(firstHandle, firstRequest, firstResult));
            requests.spawn(sendAndCapture(secondHandle, secondRequest, secondResult));
            const auto sourceDeadline = std::chrono::steady_clock::now() + 2s;
            while (peer.sources().size() < 2 &&
                   std::chrono::steady_clock::now() < sourceDeadline) {
                peer.receiveAvailable();
                if (peer.sources().size() < 2) {
                    (void)co_await ruvia::sleepFor(worker, 1ms);
                }
            }
            peer.receiveAvailable();
            RUVIA_CHECK(peer.sources().size() >= 2);
            RUVIA_CHECK(peer.quicLongHeaders() >= 2);

            firstStop.requestStop();
            secondStop.requestStop();
            const bool firstFinished = co_await waitForCompletion(
                worker, peer, firstResult, 2s);
            const bool secondFinished = co_await waitForCompletion(
                worker, peer, secondResult, 2s);
            RUVIA_CHECK(firstFinished && secondFinished);
            co_await requests.join();
        }
        RUVIA_CHECK(firstResult.finished && secondResult.finished);
        RUVIA_CHECK(firstResult.error == ruvia::HttpClientError::Code::kCancelled);
        RUVIA_CHECK(secondResult.error == ruvia::HttpClientError::Code::kCancelled);
        RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});

        peer.receiveAvailable();
        const auto deadlineDatagramCount = peer.datagrams() + 1;
        SendResult deadlineResult;
        ruvia::StopSource deadlineSafetyStop;
        const ruvia::HttpClientRequestView deadlineRequest{
            .method = "GET", .target = "/deadline"};
        {
            ruvia::TaskScope request(worker);
            request.spawn(sendAndCapture(client.withOptions(
                                             {.timeout = 150ms, .stopToken = deadlineSafetyStop.token()}),
                deadlineRequest, deadlineResult));
            const bool dispatched = co_await waitForDatagrams(
                worker, peer, deadlineDatagramCount, 1s);
            RUVIA_CHECK(dispatched);
            const bool finished = co_await waitForCompletion(
                worker, peer, deadlineResult, 2s);
            RUVIA_CHECK(finished);
            if (!deadlineResult.finished) {
                deadlineSafetyStop.requestStop();
                (void)co_await waitForCompletion(
                    worker, peer, deadlineResult, 1s);
            }
            co_await request.join();
        }
        RUVIA_CHECK(deadlineResult.error == ruvia::HttpClientError::Code::kTimeout);
        RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});

        peer.receiveAvailable();
        const auto rotatedDatagramCount = peer.datagrams() + 1;
        ruvia::StopSource rotationStop;
        SendResult rotationResult;
        const ruvia::HttpClientRequestView rotationRequest{
            .method = "GET", .target = "/rotated-connection"};
        {
            ruvia::TaskScope request(worker);
            request.spawn(sendAndCapture(client.withOptions(
                                             {.timeout = 5s, .stopToken = rotationStop.token()}),
                rotationRequest, rotationResult));
            const bool rotated = co_await waitForDatagrams(
                worker, peer, rotatedDatagramCount, 2s);
            RUVIA_CHECK(rotated);
            rotationStop.requestStop();
            const bool finished = co_await waitForCompletion(
                worker, peer, rotationResult, 2s);
            RUVIA_CHECK(finished);
            co_await request.join();
        }
        RUVIA_CHECK(rotationResult.error == ruvia::HttpClientError::Code::kCancelled);
        RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});

        co_await client.shutdown();
        peer.receiveAvailable();
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

using TestQuicAddress = ruvia::quic_address;

[[nodiscard]] TestQuicAddress testQuicAddress(const Udp::endpoint& endpoint) {
    if (!endpoint.address().is_v4()) {
        throw std::runtime_error("HTTP/3 GOAWAY peer requires IPv4");
    }
    TestQuicAddress address;
    const auto bytes = endpoint.address().to_v4().to_bytes();
    std::transform(bytes.begin(), bytes.end(), address.bytes.begin(),
        [](unsigned char value) { return std::byte{value}; });
    address.port = endpoint.port();
    return address;
}

[[nodiscard]] Udp::endpoint testUdpEndpoint(const TestQuicAddress& address) {
    if (address.family != ruvia::quic_address_family::ipv4) {
        throw std::runtime_error("HTTP/3 GOAWAY peer received a non-IPv4 destination");
    }
    asio::ip::address_v4::bytes_type bytes{};
    std::transform(address.bytes.begin(), address.bytes.begin() + bytes.size(), bytes.begin(),
        [](std::byte value) { return std::to_integer<unsigned char>(value); });
    return {asio::ip::address_v4(bytes), address.port};
}

[[nodiscard]] std::vector<char> testHttp3Frame(
    std::uint64_t type, std::span<const char> payload) {
    std::array<char, 16> header{};
    const auto typeSize = ruvia::encodeHttp3VarInt(header, type);
    if ((typeSize.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 GOAWAY test frame type");
    }
    const auto payloadSize = ruvia::encodeHttp3VarInt(
        std::span<char>(header).subspan(std::get<0>(typeSize)), payload.size());
    if ((payloadSize.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 GOAWAY test frame length");
    }
    std::vector<char> output(header.begin(), header.begin() + std::get<0>(typeSize) + std::get<0>(payloadSize));
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

[[nodiscard]] ruvia::Http3LocalCriticalStreams makeTestCriticalStreams() {
    auto streams = ruvia::Http3LocalCriticalStreams::create();
    if ((streams.index() != 0)) {
        throw std::runtime_error("failed to create HTTP/3 GOAWAY test stream prefixes");
    }
    return std::move(std::get<0>(streams));
}

class TestIdentityFiles final {
public:
    TestIdentityFiles() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-h3-goaway-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create HTTP/3 GOAWAY test identity directory");
        }
        try {
            EVP_PKEY_CTX* rawContext = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            if (rawContext == nullptr) {
                throw std::runtime_error("failed to create HTTP/3 GOAWAY test key generator");
            }
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
                rawContext, EVP_PKEY_CTX_free);
            EVP_PKEY* rawKey = nullptr;
            if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
                EVP_PKEY_generate(context.get(), &rawKey) <= 0) {
                throw std::runtime_error("failed to generate HTTP/3 GOAWAY test key");
            }
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
            std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
            if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
                ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
                X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
                X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) == nullptr ||
                X509_set_pubkey(certificate.get(), key.get()) != 1 ||
                X509_set_issuer_name(certificate.get(), X509_get_subject_name(certificate.get())) != 1 ||
                ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0) {
                throw std::runtime_error("failed to create HTTP/3 GOAWAY test certificate");
            }
            certificateFile_ = directory_ / "cert.pem";
            privateKeyFile_ = directory_ / "key.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> certificateBio(
                BIO_new_file(certificateFile_.string().c_str(), "w"), BIO_free);
            std::unique_ptr<BIO, decltype(&BIO_free)> privateKeyBio(
                BIO_new_file(privateKeyFile_.string().c_str(), "w"), BIO_free);
            if (!certificateBio || !privateKeyBio ||
                PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1 ||
                ruvia::test::write_tls_private_key(privateKeyBio.get(), key.get()) != 1) {
                throw std::runtime_error("failed to write HTTP/3 GOAWAY test identity");
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

class GoAwayRotationPeer final {
    struct Request final {
        std::uint64_t stream{};
        std::size_t responseOffset{};
        bool requestFinished{};
        bool responseFinished{};
        bool rejectionResetSent{};
    };
    struct Connection final {
        ruvia::quic_connection_token id{};
        std::array<std::optional<std::uint64_t>, 3> criticalIds{};
        std::array<std::size_t, 3> criticalOffsets{};
        std::vector<Request> requests;
        std::size_t goAwayOffset{};
    };

public:
    static constexpr std::uint64_t kUnobservedStream =
        std::numeric_limits<std::uint64_t>::max();

    explicit GoAwayRotationPeer(const TestIdentityFiles& identity,
        bool rejectFirstRequestAsUnprocessed = false)
        : socket_(io_, Udp::endpoint(asio::ip::address_v4::loopback(), 0)),
          endpoint_(socket_.local_endpoint()),
          identity_(identity),
          rejectFirstRequestAsUnprocessed_(rejectFirstRequestAsUnprocessed),
          prefixes_(makeTestCriticalStreams()) {
        socket_.non_blocking(true);
        for (std::size_t index = 0; index < criticalBytes_.size(); ++index) {
            const std::array<std::span<const char>, 3> spans{
                prefixes_.controlPrefix(), prefixes_.qpackEncoderPrefix(),
                prefixes_.qpackDecoderPrefix()};
            criticalBytes_[index].assign(spans[index].begin(), spans[index].end());
        }
        std::pmr::monotonic_buffer_resource temporary;
        const ruvia::Http3FieldSectionFieldView fields[]{{":status", "200"},
            {"content-length", "2"}};
        const auto encodedHead = ruvia::encodeHttp3FieldSection(fields, &temporary);
        if ((encodedHead.index() != 0)) {
            throw std::runtime_error("failed to encode HTTP/3 GOAWAY test response");
        }
        responseBytes_ = testHttp3Frame(1, std::get<0>(encodedHead));
        const auto data = testHttp3Frame(0, std::span<const char>("ok", 2));
        responseBytes_.insert(responseBytes_.end(), data.begin(), data.end());
        std::array<char, ruvia::kHttp3VarIntMaxBytes> goAwayId{};
        const auto goAwayIdSize = ruvia::encodeHttp3VarInt(
            goAwayId, rejectFirstRequestAsUnprocessed_ ? 0 : 4);
        if ((goAwayIdSize.index() != 0)) {
            throw std::runtime_error("failed to encode HTTP/3 GOAWAY test identifier");
        }
        goAwayBytes_ = testHttp3Frame(7,
            std::span<const char>(goAwayId.data(), std::get<0>(goAwayIdSize)));
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 5s,
                [this] { return started_ || failure_ != nullptr; })) {
            lock.unlock();
            stop_.store(true, std::memory_order_release);
            thread_.join();
            throw std::runtime_error("HTTP/3 GOAWAY peer startup watchdog expired");
        }
        const auto failure = failure_;
        lock.unlock();
        if (failure != nullptr) {
            thread_.join();
            std::rethrow_exception(failure);
        }
    }

    ~GoAwayRotationPeer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    GoAwayRotationPeer(const GoAwayRotationPeer&) = delete;
    GoAwayRotationPeer& operator=(const GoAwayRotationPeer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }
    [[nodiscard]] std::size_t acceptedConnections() const noexcept {
        return acceptedConnections_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t firstRequestStream() const noexcept {
        return firstRequestStream_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t secondRequestStream() const noexcept {
        return secondRequestStream_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool firstConnectionSawStream4() const noexcept {
        return firstConnectionSawStream4_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool goAwaySent() const noexcept {
        return goAwaySent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool rejectionResetSent() const noexcept {
        return rejectionResetSent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool firstConnectionApplicationResponseStarted() const noexcept {
        return firstConnectionApplicationResponseStarted_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool synchronize() {
        std::unique_lock lock(mutex_);
        const auto generation = ++synchronizeRequested_;
        if (!condition_.wait_for(lock, 5s, [this, generation] {
                return synchronizeCompleted_ >= generation || failure_ != nullptr;
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
    void run() noexcept {
        try {
            std::pmr::unsynchronized_pool_resource resource;
            ruvia::detail::HttpServerListenerDefinition::Tls tls;
            tls.identity.certificateChainFile = identity_.certificate().string();
            tls.identity.privateKeyFile = identity_.privateKey().string();
            ruvia::detail::http3_quic_tls_context tlsContext(tls, &resource);
            ruvia::detail::http3_quic_server_transport server(tlsContext, {}, &resource);
            {
                std::lock_guard lock(mutex_);
                started_ = true;
            }
            condition_.notify_all();

            std::vector<Connection> connections;
            std::array<std::byte, 65536> packet{};
            std::array<char, 4096> requestBytes{};
            bool retiredRejectedConnection = false;
            const auto watchdog = std::chrono::steady_clock::now() + 30s;
            while (!stop_.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= watchdog) {
                    throw std::runtime_error("HTTP/3 GOAWAY peer watchdog expired");
                }
                for (unsigned count = 0; count != 32; ++count) {
                    Udp::endpoint source;
                    asio::error_code error;
                    const auto size = socket_.receive_from(asio::buffer(packet), source, 0, error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error) {
                        // The rejected connection can close while packets to its
                        // old UDP port are still in flight. Keep serving its replacement.
                        if (error == asio::error::connection_reset) {
                            continue;
                        }
                        throw std::system_error(error, "receive HTTP/3 GOAWAY test datagram");
                    }
                    if (size == 0) {
                        continue;
                    }
                    const auto bytes = std::span<const std::byte>(packet.data(), size);
                    const auto local = ruvia::detail::from_quic_address(testQuicAddress(endpoint_));
                    const auto remote = ruvia::detail::from_quic_address(testQuicAddress(source));
                    const auto routed = server.route_datagram(bytes, local, remote);
                    const auto now = std::chrono::steady_clock::now();
                    if (routed.kind == ruvia::quic_server_route_kind::initial_offer) {
                        const auto admitted = server.admit_initial(routed.offer, now);
                        if (admitted.status == ruvia::quic_operation_status::accepted) {
                            connections.push_back(Connection{.id = admitted.connection});
                            acceptedConnections_.store(connections.size(), std::memory_order_release);
                        }
                    } else if (routed.kind == ruvia::quic_server_route_kind::existing_connection) {
                        (void)server.server().receive(routed.connection,
                            {bytes, testQuicAddress(endpoint_), testQuicAddress(source)}, now);
                    }
                }
                (void)server.server().handle_expiry(std::chrono::steady_clock::now());

                for (std::size_t connectionIndex = 0;
                    connectionIndex < connections.size(); ++connectionIndex) {
                    auto& connection = connections[connectionIndex];
                    if (connectionIndex == 0 && retiredRejectedConnection) {
                        continue;
                    }
                    auto& transport = server.server().connection(connection.id);
                    const auto info = transport.info();
                    if (!info.quic_handshake_complete || info.state != ruvia::quic_connection_state::ready) {
                        continue;
                    }
                    for (std::size_t index = 0; index < connection.criticalIds.size(); ++index) {
                        if (!connection.criticalIds[index]) {
                            const auto opened = transport.open_stream(true);
                            if (opened.status != ruvia::quic_operation_status::accepted) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer critical stream open failed");
                            }
                            connection.criticalIds[index] = opened.stream_id;
                        }
                        auto& offset = connection.criticalOffsets[index];
                        if (offset < criticalBytes_[index].size()) {
                            const auto written = transport.write_stream(*connection.criticalIds[index],
                                std::as_bytes(std::span<const char>(criticalBytes_[index]).subspan(offset)));
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                offset += written.accepted;
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer critical stream write failed");
                            }
                        }
                    }

                    const auto newlyAccepted = transport.accept_streams();
                    if (newlyAccepted.status != ruvia::quic_operation_status::accepted &&
                        newlyAccepted.status != ruvia::quic_operation_status::need_input) {
                        throw std::runtime_error("HTTP/3 GOAWAY peer stream acceptance failed");
                    }
                    for (std::size_t index = 0; index < newlyAccepted.size; ++index) {
                        const auto& stream = newlyAccepted.streams[index];
                        if (!stream.readable || !stream.writable) {
                            continue;
                        }
                        connection.requests.push_back(Request{.stream = stream.stream_id});
                        if (connectionIndex == 0) {
                            std::uint64_t unobserved = kUnobservedStream;
                            static_cast<void>(firstRequestStream_.compare_exchange_strong(
                                unobserved, stream.stream_id, std::memory_order_release,
                                std::memory_order_relaxed));
                            if (stream.stream_id == 4) {
                                firstConnectionSawStream4_.store(true, std::memory_order_release);
                            }
                        } else if (connectionIndex == 1) {
                            std::uint64_t unobserved = kUnobservedStream;
                            static_cast<void>(secondRequestStream_.compare_exchange_strong(
                                unobserved, stream.stream_id, std::memory_order_release,
                                std::memory_order_relaxed));
                        }
                    }

                    for (auto& request : connection.requests) {
                        if (!request.requestFinished) {
                            for (;;) {
                                const auto read = transport.read_stream(request.stream, std::as_writable_bytes(std::span(requestBytes)));
                                if (read.status == ruvia::quic_stream_read_status::data) {
                                    continue;
                                }
                                if (read.status == ruvia::quic_stream_read_status::fin) {
                                    request.requestFinished = true;
                                } else if (read.status != ruvia::quic_stream_read_status::would_block) {
                                    throw std::runtime_error("HTTP/3 GOAWAY peer request read failed");
                                }
                                break;
                            }
                        }
                        if (rejectFirstRequestAsUnprocessed_ && connectionIndex == 0) {
                            if (request.stream != 0) {
                                throw std::runtime_error(
                                    "HTTP/3 GOAWAY replay peer expected request stream 0");
                            }
                            if (request.rejectionResetSent) {
                                continue;
                            }
                            if (!goAwaySent()) {
                                if (connection.criticalOffsets[0] != criticalBytes_[0].size()) {
                                    continue;
                                }
                                const auto written = transport.write_stream(*connection.criticalIds[0],
                                    std::as_bytes(std::span<const char>(goAwayBytes_).subspan(connection.goAwayOffset)));
                                if (written.status == ruvia::quic_operation_status::accepted) {
                                    connection.goAwayOffset += written.accepted;
                                    if (connection.goAwayOffset == goAwayBytes_.size()) {
                                        goAwaySent_.store(true, std::memory_order_release);
                                    }
                                } else if (written.status != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error(
                                        "HTTP/3 GOAWAY replay control write failed");
                                }
                                if (!goAwaySent()) {
                                    continue;
                                }
                            }
                            const auto reset = transport.reset_stream(request.stream,
                                static_cast<std::uint64_t>(
                                    ruvia::Http3ConnectionErrorCode::kRequestRejected));
                            if (reset != ruvia::quic_operation_status::accepted) {
                                throw std::runtime_error(
                                    "HTTP/3 GOAWAY replay request reset failed");
                            }
                            request.rejectionResetSent = true;
                            rejectionResetSent_.store(true, std::memory_order_release);
                            continue;
                        }
                        if (!request.requestFinished || request.responseFinished) {
                            continue;
                        }
                        if (connectionIndex == 0 && !goAwaySent()) {
                            if (connection.criticalOffsets[0] != criticalBytes_[0].size()) {
                                continue;
                            }
                            const auto written = transport.write_stream(*connection.criticalIds[0],
                                std::as_bytes(std::span<const char>(goAwayBytes_).subspan(connection.goAwayOffset)));
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                connection.goAwayOffset += written.accepted;
                                if (connection.goAwayOffset == goAwayBytes_.size()) {
                                    goAwaySent_.store(true, std::memory_order_release);
                                }
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer control write failed");
                            }
                            // Send the cutoff before releasing the first response.
                            // Completing that response must not race control delivery.
                            continue;
                        }
                        if (request.responseOffset < responseBytes_.size()) {
                            const auto written = transport.write_stream(request.stream,
                                std::as_bytes(std::span<const char>(responseBytes_).subspan(request.responseOffset)));
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                request.responseOffset += written.accepted;
                                if (connectionIndex == 0 && written.accepted != 0) {
                                    firstConnectionApplicationResponseStarted_.store(
                                        true, std::memory_order_release);
                                }
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer response write failed");
                            }
                        }
                        if (request.responseOffset != responseBytes_.size()) {
                            continue;
                        }
                        const auto finished = transport.finish_stream(request.stream);
                        if (finished == ruvia::quic_operation_status::accepted) {
                            request.responseFinished = true;
                        } else if (finished != ruvia::quic_operation_status::would_block) {
                            throw std::runtime_error("HTTP/3 GOAWAY peer response FIN failed");
                        }
                    }
                }

                for (std::size_t index = 0; index < connections.size(); ++index) {
                    if (index == 0 && retiredRejectedConnection) {
                        continue;
                    }
                    auto& transport = server.server().connection(connections[index].id);
                    for (unsigned count = 0; count != 64; ++count) {
                        const auto outbound = transport.write_packet(packet, std::chrono::steady_clock::now());
                        if (outbound.size == 0) {
                            break;
                        }
                        asio::error_code error;
                        const auto sent = socket_.send_to(
                            asio::buffer(packet.data(), outbound.size), testUdpEndpoint(outbound.peer), 0, error);
                        if (error || sent != outbound.size) {
                            throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                                "send HTTP/3 GOAWAY test datagram");
                        }
                    }
                }
                if (rejectFirstRequestAsUnprocessed_ && rejectionResetSent() &&
                    !retiredRejectedConnection) {
                    // The reset datagrams are sent; release the rejected QUIC
                    // connection before admitting the replay on a new one.
                    server.retire(connections.front().id);
                    retiredRejectedConnection = true;
                }
                {
                    std::lock_guard lock(mutex_);
                    synchronizeCompleted_ = synchronizeRequested_;
                }
                condition_.notify_all();
                std::this_thread::sleep_for(1ms);
            }
            for (const auto& connection : connections) {
                server.retire(connection.id);
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
                started_ = true;
            }
            condition_.notify_all();
        }
    }

    asio::io_context io_;
    Udp::socket socket_;
    Udp::endpoint endpoint_;
    const TestIdentityFiles& identity_;
    bool rejectFirstRequestAsUnprocessed_{};
    ruvia::Http3LocalCriticalStreams prefixes_;
    std::array<std::vector<char>, 3> criticalBytes_;
    std::vector<char> responseBytes_;
    std::vector<char> goAwayBytes_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::exception_ptr failure_;
    bool started_{};
    std::uint64_t synchronizeRequested_{};
    std::uint64_t synchronizeCompleted_{};
    std::atomic<bool> stop_{};
    std::atomic<std::size_t> acceptedConnections_{};
    std::atomic<std::uint64_t> firstRequestStream_{kUnobservedStream};
    std::atomic<std::uint64_t> secondRequestStream_{kUnobservedStream};
    std::atomic<bool> firstConnectionSawStream4_{};
    std::atomic<bool> goAwaySent_{};
    std::atomic<bool> rejectionResetSent_{};
    std::atomic<bool> firstConnectionApplicationResponseStarted_{};
};

template <typename Predicate>
ruvia::Task<bool> waitForGoAwayPeer(const ruvia::WorkerHandle& worker,
    GoAwayRotationPeer& peer, Predicate predicate, std::chrono::milliseconds timeout) {
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

class ClientWatchdog final {
    struct State final {
        ruvia::HttpClient* client{};
        bool expired{};
    };

public:
    ClientWatchdog(asio::io_context& io, ruvia::HttpClient& client)
        : timer_(io),
          state_(std::make_shared<State>(State{&client, false})) {
        timer_.expires_after(25s);
        timer_.async_wait([state = state_](const asio::error_code& error) noexcept {
            if (!error && state->client != nullptr) {
                state->expired = true;
                state->client->close();
            }
        });
    }

    ~ClientWatchdog() {
        disarm();
    }

    void disarm() noexcept {
        state_->client = nullptr;
        asio::error_code ignored;
        timer_.cancel();
    }

    [[nodiscard]] bool expired() const noexcept {
        return state_->expired;
    }

private:
    asio::steady_timer timer_;
    std::shared_ptr<State> state_;
};

ruvia::Task<bool> waitForHttpClientInFlightZero(const ruvia::WorkerHandle& worker,
    ruvia::HttpClient& client, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (client.stats().inFlightRequests != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        if (co_await ruvia::sleepFor(worker, 1ms) != ruvia::TimerSleepResult::kElapsed) {
            co_return false;
        }
    }
    co_return true;
}

void checkGoAwayRotationResponse(ruvia::HttpClientResponse& response,
    const ruvia::HttpClientResponseBytes& body, ruvia::testing::TestContext& ruvia_ctx) {
    RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{200});
    RUVIA_CHECK(response.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
    RUVIA_CHECK(response.body().complete());
    RUVIA_CHECK_EQ(body.size(), std::size_t{2});
    if (body.size() == 2) {
        const auto bytes = body.bytes();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            std::string_view("ok"));
    }
}

ruvia::Task<void> exercisePublicHttp3GoAwayRotation(
    asio::io_context& io, ruvia::EventLoopAttachment& attachment,
    GoAwayRotationPeer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    std::exception_ptr failure;
    std::string_view stage = "client setup";
    bool shutdownCompleted = false;
    bool inFlightZero = false;
    try {
        ruvia::HttpClient client(attachment.loop(), ruvia::HttpClientConfig{
                                                        .scheme = ruvia::HttpScheme::kHttps,
                                                        .host = "127.0.0.1",
                                                        .port = peer.port(),
                                                        .connectionCount = 1,
                                                        .connectTimeout = 3s,
                                                        .requestTimeout = 8s,
                                                        .acquireTimeout = 3s,
                                                        .maxResponseBytes = 64,
                                                        .protocol = ruvia::HttpClientProtocol::kHttp3Only,
                                                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
                                                    });
        ClientWatchdog watchdog(io, client);
        try {
            stage = "first response";
            const ruvia::HttpClientRequestView firstRequest{
                .method = "GET", .target = "/goaway-first"};
            {
                auto response = co_await client.send(firstRequest);
                auto body = co_await response.body().readAll(16);
                checkGoAwayRotationResponse(response, body, ruvia_ctx);
            }
            RUVIA_CHECK_EQ(peer.acceptedConnections(), std::size_t{1});
            RUVIA_CHECK_EQ(peer.firstRequestStream(), std::uint64_t{0});
            RUVIA_CHECK(peer.goAwaySent());

            stage = "second response";
            const ruvia::HttpClientRequestView secondRequest{
                .method = "GET", .target = "/goaway-second"};
            {
                auto response = co_await client.send(secondRequest);
                auto body = co_await response.body().readAll(16);
                checkGoAwayRotationResponse(response, body, ruvia_ctx);
            }
            stage = "peer synchronization";
            const auto worker = attachment.loop().handle();
            const bool secondConnectionReady = co_await waitForGoAwayPeer(worker, peer, [&] { return peer.acceptedConnections() >= 2 &&
                                                                                                     peer.secondRequestStream() != GoAwayRotationPeer::kUnobservedStream; }, 5s);
            RUVIA_CHECK(secondConnectionReady);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK(peer.acceptedConnections() >= 2);
            RUVIA_CHECK_EQ(peer.secondRequestStream(), std::uint64_t{0});
            RUVIA_CHECK(!peer.firstConnectionSawStream4());
            stage = "in-flight retirement";
            inFlightZero = co_await waitForHttpClientInFlightZero(
                worker, client, 2s);
            RUVIA_CHECK(inFlightZero);
        } catch (...) {
            failure = std::current_exception();
        }
        stage = "client shutdown";
        try {
            co_await client.shutdown();
            shutdownCompleted = true;
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        watchdog.disarm();
        RUVIA_CHECK(shutdownCompleted);
        RUVIA_CHECK(!watchdog.expired());
        peer.rethrowIfFailed();
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "HTTP/3 GOAWAY rotation test failed during %.*s: %s (connections=%zu, first=%llu, second=%llu, goaway=%d, stream4=%d)\n",
                static_cast<int>(stage.size()), stage.data(), error.what(), peer.acceptedConnections(),
                static_cast<unsigned long long>(peer.firstRequestStream()),
                static_cast<unsigned long long>(peer.secondRequestStream()),
                peer.goAwaySent(), peer.firstConnectionSawStream4());
        } catch (...) {
            std::fprintf(stderr, "HTTP/3 GOAWAY rotation test failed with a non-standard exception\n");
        }
        RUVIA_CHECK(false);
    }
}

ruvia::Task<void> exercisePublicHttp3GoAwayReplay(
    asio::io_context& io, ruvia::EventLoopAttachment& attachment,
    GoAwayRotationPeer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    std::exception_ptr failure;
    std::string_view stage = "client setup";
    bool shutdownCompleted = false;
    bool inFlightZero = false;
    std::size_t callerSuccesses = 0;
    try {
        ruvia::HttpClient client(attachment.loop(), ruvia::HttpClientConfig{
                                                        .scheme = ruvia::HttpScheme::kHttps,
                                                        .host = "127.0.0.1",
                                                        .port = peer.port(),
                                                        .connectionCount = 1,
                                                        .connectTimeout = 3s,
                                                        .requestTimeout = 8s,
                                                        .acquireTimeout = 3s,
                                                        .maxResponseBytes = 64,
                                                        .protocol = ruvia::HttpClientProtocol::kHttp3Only,
                                                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
                                                    });
        ClientWatchdog watchdog(io, client);
        try {
            stage = "unprocessed request replay";
            const ruvia::HttpClientRequestView request{
                .method = "GET", .target = "/goaway-replay"};
            const auto requestStarted = std::chrono::steady_clock::now();
            {
                auto response = co_await client.send(request);
                ++callerSuccesses;
                auto body = co_await response.body().readAll(16);
                checkGoAwayRotationResponse(response, body, ruvia_ctx);
            }
            RUVIA_CHECK(std::chrono::steady_clock::now() - requestStarted < 8s);
            RUVIA_CHECK_EQ(callerSuccesses, std::size_t{1});

            stage = "peer replay synchronization";
            const auto worker = attachment.loop().handle();
            const bool replayObserved = co_await waitForGoAwayPeer(worker, peer, [&] { return peer.acceptedConnections() >= 2 &&
                                                                                              peer.secondRequestStream() != GoAwayRotationPeer::kUnobservedStream &&
                                                                                              peer.rejectionResetSent(); }, 5s);
            RUVIA_CHECK(replayObserved);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK_EQ(peer.firstRequestStream(), std::uint64_t{0});
            RUVIA_CHECK_EQ(peer.secondRequestStream(), std::uint64_t{0});
            RUVIA_CHECK(peer.acceptedConnections() >= 2);
            RUVIA_CHECK(peer.goAwaySent());
            RUVIA_CHECK(peer.rejectionResetSent());
            RUVIA_CHECK(!peer.firstConnectionApplicationResponseStarted());

            stage = "in-flight retirement";
            inFlightZero = co_await waitForHttpClientInFlightZero(
                worker, client, 2s);
            RUVIA_CHECK(inFlightZero);
            RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});
        } catch (...) {
            failure = std::current_exception();
        }
        stage = "client shutdown";
        try {
            co_await client.shutdown();
            shutdownCompleted = true;
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        watchdog.disarm();
        RUVIA_CHECK(shutdownCompleted);
        RUVIA_CHECK(!watchdog.expired());
        peer.rethrowIfFailed();
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "HTTP/3 GOAWAY replay test failed during %.*s: %s (connections=%zu, first=%llu, second=%llu, goaway=%d, rejected=%d, response=%d, successes=%zu)\n",
                static_cast<int>(stage.size()), stage.data(), error.what(), peer.acceptedConnections(),
                static_cast<unsigned long long>(peer.firstRequestStream()),
                static_cast<unsigned long long>(peer.secondRequestStream()),
                peer.goAwaySent(), peer.rejectionResetSent(),
                peer.firstConnectionApplicationResponseStarted(), callerSuccesses);
        } catch (...) {
            std::fprintf(stderr, "HTTP/3 GOAWAY replay test failed with a non-standard exception\n");
        }
        RUVIA_CHECK(false);
    }
}
}  // namespace

RUVIA_TEST(http3PublicHttpClientConstructedBeforeWorkerLaunchBindsQuicOwnerOnWorker) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    QuicUdpBlackhole peer(io);
    ruvia::HttpClient client(attachment.loop(), ruvia::HttpClientConfig{
                                                    .scheme = ruvia::HttpScheme::kHttps,
                                                    .host = "127.0.0.1",
                                                    .port = peer.port(),
                                                    .connectionCount = 1,
                                                    .connectTimeout = 2s,
                                                    .requestTimeout = 5s,
                                                    .acquireTimeout = 1s,
                                                    .maxResponseBytes = 4096,
                                                    .protocol = ruvia::HttpClientProtocol::kHttp3Only,
                                                    .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
                                                });
    SendResult result;
    auto root = attachment.loop().start(
        exerciseCrossThreadConstructedHttp3Client(attachment, peer, client, result));
    std::thread worker([&attachment] { attachment.run(); });
    worker.join();
    root.get();
    RUVIA_CHECK(result.finished);
    RUVIA_CHECK(result.error == ruvia::HttpClientError::Code::kCancelled);
    RUVIA_CHECK(peer.quicLongHeaders() >= 1);
}

RUVIA_TEST(http3PublicHttpClientHandleDispatchesUdpAndMapsPoolSlotsToConnections) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    QuicUdpBlackhole peer(io);
    auto root = attachment.loop().start(
        exercisePublicHttp3Dispatch(attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3PublicHttpClientRotatesConnectionAfterPeerGoAway) {
    TestIdentityFiles identity;
    GoAwayRotationPeer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    auto root = attachment.loop().start(
        exercisePublicHttp3GoAwayRotation(io, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3PublicHttpClientRetriesPeerReportedUnprocessedRequest) {
    TestIdentityFiles identity;
    GoAwayRotationPeer peer(identity, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    auto root = attachment.loop().start(
        exercisePublicHttp3GoAwayReplay(io, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}
