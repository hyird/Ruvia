#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include <openssl/x509v3.h>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/WebSocketConnection.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/WebSocketClient.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http3/Http3ClientConnection.h"
#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/server/ServerNetworkRuntime.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

#include "http3_quic_udp_pair.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace ruvia::detail {
struct http3_client_connection_test_access final {
    using http3_client_connection = Http3ClientConnection;

    static std::optional<std::uint64_t> request_stream_id(
        const http3_client_connection& connection,
        http3_client_connection::RequestId id) noexcept {
        const auto request = std::find_if(connection.requests_.begin(), connection.requests_.end(),
            [id](const auto& candidate) { return candidate.id == id; });
        return request == connection.requests_.end() ? std::nullopt : request->writer.streamId();
    }

    static bool has_pending_priority_update(
        const http3_client_connection& connection,
        http3_client_connection::RequestId id) noexcept {
        const auto request = std::find_if(connection.requests_.begin(), connection.requests_.end(),
            [id](const auto& candidate) { return candidate.id == id; });
        return request != connection.requests_.end() && request->pending_priority_update.has_value();
    }

    static bool early_request_finished(const http3_client_connection& connection,
        http3_client_connection::RequestId id) noexcept {
        if (!connection.session_) {
            return false;
        }
        const auto request = std::find_if(connection.requests_.begin(), connection.requests_.end(),
            [id](const auto& candidate) { return candidate.id == id; });
        if (request == connection.requests_.end()) {
            return false;
        }
        const auto info = connection.session_->transport().info();
        return connection.session_->early_data_enabled() &&
               info.early_data == ruvia::quic_early_data_state::available &&
               !info.quic_handshake_complete && request->responseParserRegistered &&
               request->writer.finished();
    }
};
}  // namespace ruvia::detail

namespace {
using Connection = ruvia::detail::Http3ClientConnection;
using connection_test_access = ruvia::detail::http3_client_connection_test_access;
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
            auto* subject = X509_get_subject_name(certificate.get());
            const auto* name = reinterpret_cast<const unsigned char*>("localhost");
            if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, name, -1, -1, 0) != 1 ||
                X509_set_issuer_name(certificate.get(), subject) != 1) {
                throw std::runtime_error("failed to name HTTP/3 test identity");
            }
            const std::array extensions{std::pair{NID_subject_alt_name, "IP:127.0.0.1,DNS:localhost"},
                std::pair{NID_basic_constraints, "critical,CA:TRUE"}};
            for (const auto& [nid, value] : extensions) {
                std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                    X509V3_EXT_conf_nid(nullptr, nullptr, nid, value), X509_EXTENSION_free);
                if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1) {
                    throw std::runtime_error("failed to authenticate HTTP/3 test identity");
                }
            }
            if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
                throw std::runtime_error("failed to sign HTTP/3 test identity");
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

[[nodiscard]] ruvia::quic_stream_write_result write_quic_stream(
    ruvia::quic_connection& connection, std::uint64_t stream_id,
    std::span<const char> bytes, bool fin = false) {
    return connection.write_stream(stream_id, std::as_bytes(bytes), fin);
}

[[nodiscard]] ruvia::quic_stream_read_result read_quic_stream(
    ruvia::quic_connection& connection, std::uint64_t stream_id,
    std::span<char> bytes) {
    return connection.read_stream(stream_id, std::as_writable_bytes(bytes));
}

[[nodiscard]] bool is_quic_stream_closed(ruvia::quic_operation_status status) noexcept {
    return status == ruvia::quic_operation_status::closing ||
           status == ruvia::quic_operation_status::draining ||
           status == ruvia::quic_operation_status::retired;
}

[[nodiscard]] std::vector<char> test_http3_frame(
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

struct quic_push_scenario final {
    std::size_t count{1};
    std::size_t body_bytes{5};
    bool stream_before_promise{false};
    bool promise_only{false};
    bool cancel_before_promise{false};
    bool malformed_response{false};
    bool cross_origin{false};
};

class local_quic_response_peer final {
public:
    using stream_id = std::uint64_t;

    explicit local_quic_response_peer(const TestIdentityFiles& identity,
        bool malformedTail = false, bool consumeRequest = true, bool webSocket = false,
        bool advertiseOrigins = false, std::optional<quic_push_scenario> push = {},
        bool tunnel = false, bool udp = false, bool early_data = false,
        ruvia::detail::http3_quic_tls_context* shared_server_tls = nullptr,
        bool hold_handshake_on_early_decision = false)
        : consumeRequest_(consumeRequest),
          webSocket_(webSocket),
          advertiseOrigins_(advertiseOrigins),
          push_(push),
          tunnel_(tunnel || udp),
          udp_(udp),
          early_data_(early_data),
          shared_server_tls_(shared_server_tls),
          hold_handshake_on_early_decision_(hold_handshake_on_early_decision) {
        ruvia::detail::HttpServerListenerDefinition::Tls tls;
        tls.identity.certificateChainFile = identity.certificate().string();
        tls.identity.privateKeyFile = identity.privateKey().string();
        tls.http3_early_data = early_data_;
        const std::string_view contentLength = malformedTail ? "67" : "6";
        ruvia::Http3FieldSectionFieldView fields[]{{":status", "200"}, {"content-length", contentLength}};
        std::pmr::monotonic_buffer_resource temporary;
        const auto encodedHead = ruvia::encodeHttp3FieldSection(fields, &temporary);
        if (!encodedHead) {
            throw std::runtime_error("failed to encode HTTP/3 test response head");
        }
        first_part_ = test_http3_frame(1, *encodedHead);
        const auto firstData = test_http3_frame(0, std::span<const char>("abc", 3));
        first_part_.insert(first_part_.end(), firstData.begin(), firstData.end());
        if (malformedTail) {
            const std::string body(64, 'x');
            final_part_ = test_http3_frame(0, std::span<const char>(body.data(), body.size()));
            const auto unexpected = test_http3_frame(4, {});
            final_part_.insert(final_part_.end(), unexpected.begin(), unexpected.end());
        } else {
            final_part_ = test_http3_frame(0, std::span<const char>("def", 3));
        }
        if (webSocket_) {
            const ruvia::Http3FieldSectionFieldView wsFields[]{
                {":status", "200"}, {"sec-websocket-protocol", "chat"},
                {"sec-websocket-extensions", "permessage-deflate; server_no_context_takeover; client_no_context_takeover"}};
            const auto wsHead = ruvia::encodeHttp3FieldSection(wsFields, &temporary);
            if (!wsHead) {
                throw std::runtime_error("failed to encode WebSocket test response");
            }
            first_part_ = test_http3_frame(1, *wsHead);
            ruvia::WebSocketConnection websocket({.resource = &temporary});
            const std::string greeting(100000, 'w');
            if (websocket.submitFrame(ruvia::WebSocketOpcode::kBinary, greeting, false) != ruvia::WebSocketFrameSubmitStatus::kAccepted) {
                throw std::runtime_error("failed to encode WebSocket test greeting");
            }
            const auto output = websocket.outputPlan();
            const auto data = test_http3_frame(0, std::span<const char>(output.bytes().data(), output.bytes().size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            (void)websocket.consumeOutput(output.bytes().size());
            if (websocket.submitClose(1000, {}) != ruvia::WebSocketCloseSubmitStatus::kAccepted) {
                throw std::runtime_error("failed to encode WebSocket test Close");
            }
            const auto close = websocket.outputPlan();
            final_part_ = test_http3_frame(0, std::span<const char>(close.bytes().data(), close.bytes().size()));
        }
        if (tunnel_) {
            const ruvia::Http3FieldSectionFieldView tunnelFields[]{{":status", "200"}, {"x-tunnel", "owned-metadata"}};
            const auto head = ruvia::encodeHttp3FieldSection(tunnelFields, &temporary);
            if (!head) {
                throw std::runtime_error("CONNECT test head encoding failed");
            }
            first_part_ = test_http3_frame(1, *head);
            const std::string greeting(100003, 's');
            const auto data = test_http3_frame(0, std::span(greeting.data(), greeting.size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            final_part_ = test_http3_frame(0, std::span<const char>("ended", 5));
        }
        if (udp_) {
            const ruvia::Http3FieldSectionFieldView udpFields[]{{":status", "200"}, {"capsule-protocol", "?1"}};
            const auto head = ruvia::encodeHttp3FieldSection(udpFields, &temporary);
            if (!head) {
                throw std::runtime_error("UDP test head encoding failed");
            }
            first_part_ = test_http3_frame(1, *head);
            std::string payload(1, '\0');
            payload.append(16003, 's');
            std::array<char, 16> header;
            const auto encoded = ruvia::encodeHttpCapsuleHeader(header, 0, payload.size());
            std::string capsule(header.data(), *encoded);
            capsule.append(payload);
            const auto data = test_http3_frame(0, std::span(capsule.data(), capsule.size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            final_part_ = test_http3_frame(0, std::span<const char>("\0\1\0", 3));
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

    ~local_quic_response_peer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        pair_.reset();
    }

    local_quic_response_peer(const local_quic_response_peer&) = delete;
    local_quic_response_peer& operator=(const local_quic_response_peer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool first_part_ready() const noexcept {
        return first_part_ready_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool final_part_sent() const noexcept {
        return final_part_sent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool handshake_observed() const noexcept {
        return handshake_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool early_data_rejected() const noexcept {
        return early_data_rejected_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool early_data_accepted() const noexcept {
        return early_data_accepted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool handshake_paused() const noexcept {
        return handshake_paused_.load(std::memory_order_acquire);
    }
    void allow_handshake() noexcept {
        allow_handshake_.store(true, std::memory_order_release);
    }
    [[nodiscard]] std::size_t request_payload_bytes() const noexcept {
        return request_payload_bytes_.load(std::memory_order_acquire);
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
    [[nodiscard]] unsigned websocket_messages() const noexcept {
        return websocket_messages_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool client_end_observed() const noexcept {
        return client_end_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t cancelled_pushes() const noexcept {
        return cancelled_pushes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t promised_pushes() const noexcept {
        return promised_pushes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int push_priority_observed() const noexcept {
        return push_priority_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int priority_observed() const noexcept {
        return priority_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<std::uint64_t> peer_max_push_id() const noexcept {
        if (!peer_max_push_id_observed_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return peer_max_push_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<std::uint64_t> priority_element_id() const noexcept {
        if (!priority_element_id_observed_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return priority_element_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t tunnel_bytes() const noexcept {
        return tunnel_bytes_.load(std::memory_order_acquire);
    }
    void allow_final_part() noexcept {
        allow_final_part_.store(true, std::memory_order_release);
    }
    void rethrow_if_failed() const {
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
            if (shared_server_tls_ != nullptr) {
                pair_ = std::make_unique<ruvia::testing::http3_quic_udp_pair>(
                    *shared_server_tls_, ruvia::testing::http3_quic_udp_pair::server_only_t{}, &resource_);
            } else {
                pair_ = std::make_unique<ruvia::testing::http3_quic_udp_pair>(tls,
                    ruvia::testing::http3_quic_udp_pair::server_only_t{}, &resource_);
            }
            auto& pair = *pair_;
            port_.store(pair.server_endpoint().port(), std::memory_order_release);
            auto prefixes = ruvia::Http3LocalCriticalStreams::create({.enableConnectProtocol = webSocket_ || tunnel_});
            if (!prefixes) {
                throw std::runtime_error("failed to create local HTTP/3 critical stream prefixes");
            }
            const auto controlPrefix = prefixes->controlPrefix();
            std::string serverControl(controlPrefix.data(), controlPrefix.size());
            if (advertiseOrigins_) {
                const std::array<std::string_view, 2> origins{"https://127.0.0.1", "https://localhost"};
                const auto frame = ruvia::encodeHttp3OriginFrame(origins);
                if (!frame) {
                    throw std::runtime_error("failed to encode HTTP/3 ORIGIN fixture");
                }
                serverControl.append(frame->data(), frame->size());
            }
            {
                std::lock_guard lock(mutex_);
                started_ = true;
            }
            startedCondition_.notify_all();

            std::optional<ruvia::quic_connection_token> connectionId;
            std::array<std::optional<stream_id>, 3> localCriticalIds;
            std::array<std::size_t, 3> localCriticalOffsets{};
            std::array<std::span<const char>, 3> localCriticalBytes{
                std::span<const char>(serverControl), prefixes->qpackEncoderPrefix(), prefixes->qpackDecoderPrefix()};
            std::optional<stream_id> requestStream;
            struct PushWire final {
                stream_id stream{};
                std::vector<char> bytes;
                std::size_t offset{};
                std::vector<char> promise;
                std::chrono::steady_clock::time_point promise_release{};
                bool finished{};
            };
            std::deque<PushWire> pushStreams;
            std::deque<std::vector<char>> promises;
            std::size_t promiseOffset{};
            std::size_t nextPush{};
            std::optional<std::chrono::steady_clock::time_point> requestHeadAt;
            bool requestFinished = false;
            bool firstPrepared = false;
            bool dynamicHeadPrepared = false;
            std::vector<stream_id> peerCriticalStreams;
            std::array<std::string, 2> qpackBytes;
            std::array<std::size_t, 2> qpackOffsets{};
            std::optional<std::chrono::steady_clock::time_point> encoderRelease;
            std::size_t firstOffset = 0;
            std::size_t responseHeadBytes = 0;
            std::size_t finalOffset = 0;
            bool finalFinSent = false;
            std::array<char, 4096> requestBytes{};
            ruvia::Http3Connection wsRequest(ruvia::Http3PeerRole::kServer, &resource_, {.enableConnectProtocol = webSocket_ || tunnel_});
            struct WsReceive final {
                std::pmr::memory_resource* resource;
                std::atomic<int>* priority;
                std::atomic<int>* pushPriority;
                std::atomic<std::uint64_t>* priority_element_id;
                std::atomic<bool>* priority_element_id_observed;
                std::atomic<std::size_t>* cancelled;
                bool webSocket;
                bool tunnel;
                bool udp;
                std::atomic<std::size_t>* tunnel_bytes;

                ruvia::HttpCapsuleDecoder capsules{};
                std::pmr::string capsulePayload{resource};
                std::optional<ruvia::WebSocketConnection> websocket{};
                bool headReady{false};
                unsigned messages{0};
            } wsReceive{&resource_, &priority_observed_, &push_priority_observed_,
                &priority_element_id_, &priority_element_id_observed_, &cancelled_pushes_, webSocket_,
                tunnel_, udp_, &tunnel_bytes_};
            const auto onWsEvent = [](void* context, const ruvia::Http3ConnectionEvent& event) {
                auto& state = *static_cast<WsReceive*>(context);
                if (event.priorityUpdate) {
                    const auto priority = event.priorityUpdate->fields.requestPriority();
                    (event.priorityUpdate->push ? state.pushPriority : state.priority)->store(priority.urgency | (priority.incremental ? 0x100 : 0), std::memory_order_release);
                    if (!event.priorityUpdate->push) {
                        state.priority_element_id->store(event.priorityUpdate->elementId, std::memory_order_release);
                        state.priority_element_id_observed->store(true, std::memory_order_release);
                    }
                } else if (event.kind == ruvia::Http3ConnectionEventKind::kPushCanceled) {
                    state.cancelled->fetch_add(1, std::memory_order_release);
                } else if (event.kind == ruvia::Http3ConnectionEventKind::kRequestHead) {
                    if (!state.webSocket) {
                        state.headReady = true;
                        return;
                    }
                    if (!event.head || event.head->method != "CONNECT" || event.head->protocol != "websocket") {
                        throw std::runtime_error("invalid WebSocket Extended CONNECT");
                    }
                    state.headReady = true;
                    state.websocket.emplace(ruvia::WebSocketConnectionOptions{.resource = state.resource, .compression = {.enabled = true}});
                } else if (event.kind == ruvia::Http3ConnectionEventKind::kTunnelData) {
                    if (state.udp) {
                        const auto collect = [](void* raw, ruvia::HttpCapsuleEvent capsule) {
                            auto& target = *static_cast<WsReceive*>(raw);
                            if (!capsule.payload.empty()) {
                                target.capsulePayload.append(capsule.payload.data(), capsule.payload.size());
                            }
                            if (!capsule.endCapsule) {
                                return;
                            }
                            const auto datagram = ruvia::decodeHttpUdpDatagram(std::span(target.capsulePayload.data(), target.capsulePayload.size()));
                            if (capsule.type != 0 || !datagram || datagram->contextId != 0 ||
                                !std::ranges::all_of(datagram->payload, [](char ch) { return ch == 't'; })) {
                                throw std::runtime_error("invalid CONNECT-UDP client packet");
                            }
                            target.tunnel_bytes->fetch_add(datagram->payload.size(), std::memory_order_release);
                            target.capsulePayload.clear();
                        };
                        const auto decoded = state.capsules.feed(event.body, false, collect, &state);
                        if (decoded != ruvia::HttpCapsuleStatus::kNeedMoreData) {
                            throw std::runtime_error("invalid client capsule stream");
                        }
                        return;
                    }
                    if (state.tunnel) {
                        if (!std::ranges::all_of(event.body, [](char ch) { return ch == 't'; })) {
                            throw std::runtime_error("invalid CONNECT client payload");
                        }
                        state.tunnel_bytes->fetch_add(event.body.size(), std::memory_order_release);
                        return;
                    }
                    (void)state.websocket->feed(std::string_view(event.body.data(), event.body.size()));
                    while (auto message = state.websocket->nextEvent()) {
                        if (auto* payload = message->message()) {
                            if (payload->payload() != std::string(100000, 'c')) {
                                throw std::runtime_error("invalid WebSocket client payload");
                            }
                            ++state.messages;
                        } else if (message->protocolError()) {
                            throw std::runtime_error("invalid WebSocket client frame");
                        } else if (message->close() || message->transportEnd()) {
                            break;
                        }
                    }
                }
            };

            bool handshake_gate_released = false;
            while (!stop_.load(std::memory_order_acquire)) {
                handshake_gate_released |= allow_handshake_.load(std::memory_order_acquire);
                const bool drop_handshake_packets =
                    hold_handshake_on_early_decision_ && !handshake_gate_released;
                pair.pump(drop_handshake_packets, drop_handshake_packets ? 1 : 32);
                if (!connectionId) {
                    connectionId = pair.maybe_connection_token();
                }
                if (connectionId) {
                    auto& server = pair.server();
                    const auto info = server.info();
                    if (info.early_data == ruvia::quic_early_data_state::rejected) {
                        early_data_rejected_.store(true, std::memory_order_release);
                    } else if (info.early_data == ruvia::quic_early_data_state::accepted) {
                        early_data_accepted_.store(true, std::memory_order_release);
                    }
                    if (hold_handshake_on_early_decision_ && !handshake_gate_released &&
                        !info.quic_handshake_complete) {
                        handshake_paused_.store(true, std::memory_order_release);
                    }
                    if (info.quic_handshake_complete && info.state == ruvia::quic_connection_state::ready) {
                        handshake_observed_.store(true, std::memory_order_release);
                        for (std::size_t i = 0; i < localCriticalIds.size(); ++i) {
                            if (!localCriticalIds[i]) {
                                const auto opened = server.open_stream(true);
                                if (opened.status != ruvia::quic_operation_status::accepted) {
                                    throw std::runtime_error("HTTP/3 local peer critical stream open failed");
                                }
                                localCriticalIds[i] = opened.stream_id;
                            }
                            auto& offset = localCriticalOffsets[i];
                            if (offset < localCriticalBytes[i].size()) {
                                const auto written = write_quic_stream(server, *localCriticalIds[i],
                                    localCriticalBytes[i].subspan(offset));
                                if (written.status == ruvia::quic_operation_status::accepted) {
                                    offset += written.accepted;
                                } else if (written.status != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error("HTTP/3 local peer critical stream write failed");
                                }
                            }
                        }
                        auto accepted = server.accept_streams();
                        if (accepted.status != ruvia::quic_operation_status::accepted &&
                            accepted.status != ruvia::quic_operation_status::need_input &&
                            accepted.status != ruvia::quic_operation_status::would_block) {
                            throw std::runtime_error("HTTP/3 local peer stream acceptance failed");
                        }
                        for (std::size_t i = 0; i < accepted.size; ++i) {
                            const auto& stream = accepted.streams[i];
                            if (stream.readable && stream.writable) {
                                requestStream = stream.stream_id;
                            } else if (stream.readable) {
                                peerCriticalStreams.push_back(stream.stream_id);
                            }
                        }
                        if (!finalFinSent) {
                            for (const auto id : peerCriticalStreams) {
                                const auto read = read_quic_stream(server, id, requestBytes);
                                if (read.status == ruvia::quic_stream_read_status::data) {
                                    const auto parsed = wsRequest.feed(id, std::span<const char>(requestBytes.data(), read.size), false, false, onWsEvent, &wsReceive);
                                    if (parsed.scope != ruvia::Http3ConnectionErrorScope::kNone) {
                                        throw std::runtime_error("invalid WebSocket peer critical input");
                                    }
                                    if (const auto max_push_id = wsRequest.peerMaxPushId()) {
                                        peer_max_push_id_.store(*max_push_id, std::memory_order_relaxed);
                                        peer_max_push_id_observed_.store(true, std::memory_order_release);
                                    }
                                } else if (read.status != ruvia::quic_stream_read_status::would_block && !finalFinSent) {
                                    throw std::runtime_error("WebSocket peer critical transport failed");
                                }
                            }
                        }
                        if (requestStream && !requestFinished && consumeRequest_) {
                            for (;;) {
                                const auto read = read_quic_stream(server, *requestStream, requestBytes);
                                if (read.status == ruvia::quic_stream_read_status::data) {
                                    request_payload_bytes_.fetch_add(read.size, std::memory_order_release);
                                    if (webSocket_ || push_ || tunnel_) {
                                        const auto parsed = wsRequest.feed(*requestStream,
                                            std::span<const char>(requestBytes.data(), read.size), false, false, onWsEvent, &wsReceive);
                                        if (parsed.status == ruvia::Http3ConnectionStatus::kConnectionError || parsed.status == ruvia::Http3ConnectionStatus::kStreamError) {
                                            throw std::runtime_error("invalid HTTP/3 WebSocket test input");
                                        }
                                        firstPrepared = wsReceive.headReady;
                                        if (firstPrepared && !requestHeadAt) {
                                            requestHeadAt = std::chrono::steady_clock::now();
                                        }
                                        websocket_messages_.store(wsReceive.messages, std::memory_order_release);
                                        if (wsReceive.messages == 2) {
                                            allow_final_part_.store(true, std::memory_order_release);
                                        }
                                    }
                                    continue;
                                }
                                if (read.status == ruvia::quic_stream_read_status::fin) {
                                    requestFinished = true;
                                    client_end_observed_.store(true, std::memory_order_release);
                                } else if (tunnel_ && finalFinSent && read.status == ruvia::quic_stream_read_status::reset &&
                                           read.peer_reset_error_code == static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestCancelled)) {
                                    requestFinished = true;
                                } else if (read.status != ruvia::quic_stream_read_status::would_block) {
                                    throw std::runtime_error("HTTP/3 local peer request stream read failed");
                                }
                                break;
                            }
                        }
                        if (requestFinished && !firstPrepared) {
                            firstPrepared = true;
                        }
                        if (webSocket_ && firstPrepared && !dynamicHeadPrepared) {
                            ruvia::HttpResponse response({.resource = &resource_});
                            response.header("sec-websocket-protocol", "chat");
                            response.header("sec-websocket-extensions", "permessage-deflate; server_no_context_takeover; client_no_context_takeover");
                            const auto head = wsRequest.encodeResponseHead(*requestStream, response,
                                ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kConnect, response));
                            if (!head) {
                                throw std::runtime_error("failed to encode dynamic WebSocket response");
                            }
                            const auto oldHeader = ruvia::decodeHttp3FrameHeader(first_part_);
                            if (!oldHeader) {
                                throw std::runtime_error("invalid static WebSocket response fixture");
                            }
                            const auto prefix = test_http3_frame(1, head->fieldSection);
                            first_part_.erase(first_part_.begin(), first_part_.begin() + static_cast<std::ptrdiff_t>(oldHeader->encodedBytes + oldHeader->length));
                            first_part_.insert(first_part_.begin(), prefix.begin(), prefix.end());
                            responseHeadBytes = prefix.size();
                            dynamicHeadPrepared = true;
                        }
                        if (push_ && requestFinished && wsRequest.peerMaxPushId() && nextPush < push_->count && nextPush <= *wsRequest.peerMaxPushId()) {
                            const auto prepareNextPush = [&] {
                                std::optional<stream_id> openedPush;
                                if (!push_->promise_only && !push_->cancel_before_promise) {
                                    const auto opened = server.open_stream(true);
                                    if (opened.status == ruvia::quic_operation_status::would_block || opened.status == ruvia::quic_operation_status::need_input) {
                                        return;
                                    }
                                    if (opened.status != ruvia::quic_operation_status::accepted) {
                                        throw std::runtime_error("push fixture stream open failed: " + std::to_string(static_cast<unsigned>(opened.status)));
                                    }
                                    openedPush = opened.stream_id;
                                }
                                const auto pushId = nextPush++;
                                const std::string path = "/push/" + std::to_string(pushId);
                                const std::string authority = push_->cross_origin ? "other.test" : "127.0.0.1:" + std::to_string(port());
                                const std::array<ruvia::HttpHeaderView, 1> headers{ruvia::HttpHeaderView("x-promise", "owned")};
                                auto promise = wsRequest.preparePushPromise(*requestStream, pushId, {.authority = authority, .path = path, .headers = headers});
                                if (!promise) {
                                    throw std::runtime_error("push fixture promise failed");
                                }
                                if (push_->cancel_before_promise) {
                                    auto cancel = wsRequest.prepareCancelPush(pushId);
                                    // The critical initial prefix has already completed; use the stable critical output lane below.
                                    qpackBytes[1].assign(cancel->data(), cancel->size());
                                    qpackOffsets[1] = 0;
                                }
                                if (push_->promise_only || push_->cancel_before_promise) {
                                    promises.emplace_back(promise->begin(), promise->end());
                                } else {
                                    auto prefix = wsRequest.preparePushStream(*openedPush, pushId);
                                    if (!prefix) {
                                        throw std::runtime_error("push fixture stream prefix failed");
                                    }
                                    PushWire wire{*openedPush, std::vector<char>(prefix->begin(), prefix->end())};
                                    const std::string length = std::to_string(push_->body_bytes + (push_->malformed_response ? 1 : 0));
                                    const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{":status", "200"}, {"content-length", length}}};
                                    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource_);
                                    auto head = test_http3_frame(1, *encoded);
                                    wire.bytes.insert(wire.bytes.end(), head.begin(), head.end());
                                    const std::string content(push_->body_bytes, 'p');
                                    auto body = test_http3_frame(0, content);
                                    wire.bytes.insert(wire.bytes.end(), body.begin(), body.end());
                                    if (push_->stream_before_promise) {
                                        wire.promise.assign(promise->begin(), promise->end());
                                        wire.promise_release = std::chrono::steady_clock::now() + 30ms;
                                    } else {
                                        promises.emplace_back(promise->begin(), promise->end());
                                    }
                                    pushStreams.push_back(std::move(wire));
                                }
                                promised_pushes_.store(nextPush, std::memory_order_release);
                            };
                            prepareNextPush();
                        }
                        for (auto it = pushStreams.begin(); it != pushStreams.end();) {
                            if (!it->promise.empty() && std::chrono::steady_clock::now() >= it->promise_release) {
                                promises.push_back(std::move(it->promise));
                                it->promise.clear();
                            }
                            bool ended = it->finished;
                            if (it->offset < it->bytes.size()) {
                                const auto written = write_quic_stream(server, it->stream, std::span<const char>(it->bytes).subspan(it->offset));
                                if (written.status == ruvia::quic_operation_status::accepted) {
                                    it->offset += written.accepted;
                                } else if (is_quic_stream_closed(written.status)) {
                                    cancelled_pushes_.fetch_add(1, std::memory_order_release);
                                    ended = true;
                                } else if (written.status != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error("push fixture response write failed");
                                }
                            }
                            if (it->offset == it->bytes.size() && !it->finished) {
                                const auto fin = server.finish_stream(it->stream);
                                if (fin == ruvia::quic_operation_status::accepted) {
                                    ended = true;
                                    it->finished = true;
                                } else if (fin != ruvia::quic_operation_status::would_block && !is_quic_stream_closed(fin)) {
                                    throw std::runtime_error("push fixture FIN failed");
                                }
                            }
                            if (ended && it->finished && it->promise.empty()) {
                                // FIN acceptance only queues it. Retire after submission;
                                // ngtcp2 retains the output until ACK without an abortive reset.
                                ended = server.retire_completed_stream(it->stream) == ruvia::quic_operation_status::accepted;
                            }
                            if (ended && it->promise.empty()) {
                                if (!it->finished) {
                                    (void)server.close_stream(it->stream);
                                }
                                it = pushStreams.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        if (push_ && !qpackBytes[1].empty()) {
                            auto& bytes = qpackBytes[1];
                            auto& offset = qpackOffsets[1];
                            const auto written = write_quic_stream(server, *localCriticalIds[0], std::span<const char>(bytes).subspan(offset));
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                offset += written.accepted;
                                if (offset == bytes.size()) {
                                    bytes.clear();
                                }
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("push fixture cancellation failed");
                            }
                        }
                        if (push_ && !promises.empty() && (firstOffset == 0 || firstOffset == first_part_.size())) {
                            const auto written = write_quic_stream(server, *requestStream, std::span<const char>(promises.front()).subspan(promiseOffset));
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                promiseOffset += written.accepted;
                                if (promiseOffset == promises.front().size()) {
                                    promises.pop_front();
                                    promiseOffset = 0;
                                }
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("push fixture promise write failed");
                            }
                        }
                        const bool pushPrepared = !push_ || nextPush != 0 || (requestHeadAt && std::chrono::steady_clock::now() >= *requestHeadAt + 100ms);
                        if (firstPrepared && pushPrepared && promises.empty() && firstOffset < first_part_.size()) {
                            const auto offered = std::span<const char>(first_part_).subspan(firstOffset, std::min(requestBytes.size(), first_part_.size() - firstOffset));
                            const auto written = write_quic_stream(server, *requestStream, offered);
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                firstOffset += written.accepted;
                                if (webSocket_ && !encoderRelease && firstOffset >= responseHeadBytes) {
                                    encoderRelease = std::chrono::steady_clock::now() + 30ms;
                                }
                                if (firstOffset == first_part_.size()) {
                                    first_part_ready_.store(true, std::memory_order_release);
                                }
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 local peer initial response write failed");
                            }
                        }
                        if (webSocket_ && encoderRelease && std::chrono::steady_clock::now() >= *encoderRelease) {
                            for (std::size_t index = 0; index != qpackBytes.size(); ++index) {
                                auto& queued = qpackBytes[index];
                                auto& offset = qpackOffsets[index];
                                const auto pending = index == 0 ? wsRequest.pendingQpackEncoderOutput() : wsRequest.pendingQpackDecoderOutput();
                                if (queued.empty() && !pending.empty()) {
                                    queued.assign(pending.data(), pending.size());
                                    offset = 0;
                                }
                                if (queued.empty()) {
                                    continue;
                                }
                                const auto written = write_quic_stream(server, *localCriticalIds[index + 1], std::span<const char>(queued.data() + offset, queued.size() - offset));
                                if (written.status == ruvia::quic_operation_status::accepted) {
                                    const bool consumed = index == 0 ? wsRequest.consumeQpackEncoderOutput(written.accepted) : wsRequest.consumeQpackDecoderOutput(written.accepted);
                                    if (!consumed) {
                                        throw std::runtime_error("invalid WebSocket QPACK output credit");
                                    }
                                    offset += written.accepted;
                                    if (offset == queued.size()) {
                                        queued.clear();
                                    }
                                } else if (written.status != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error("WebSocket QPACK output failed");
                                }
                            }
                        }
                        if (first_part_ready() && promises.empty() && allow_final_part_.load(std::memory_order_acquire) &&
                            finalOffset < final_part_.size()) {
                            const auto written = write_quic_stream(server, *requestStream,
                                std::span<const char>(final_part_).subspan(finalOffset));
                            if (written.status == ruvia::quic_operation_status::accepted) {
                                finalOffset += written.accepted;
                            } else if (written.status != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 local peer final response write failed");
                            }
                        }
                        if (finalOffset == final_part_.size() && !finalFinSent) {
                            const auto finished = server.finish_stream(*requestStream);
                            if (finished == ruvia::quic_operation_status::accepted) {
                                finalFinSent = true;
                                final_part_sent_.store(true, std::memory_order_release);
                            } else if (finished != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 local peer response FIN failed");
                            }
                        }
                    }
                }
                const bool drop_handshake_packets_at_end =
                    hold_handshake_on_early_decision_ && !handshake_gate_released;
                pair.pump(drop_handshake_packets_at_end,
                    drop_handshake_packets_at_end ? 1 : 32);
                {
                    std::lock_guard lock(mutex_);
                    synchronizationCompleted_ = synchronizationRequested_;
                }
                startedCondition_.notify_all();
                std::this_thread::sleep_for(1ms);
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

    std::atomic<std::uint16_t> port_{};
    std::pmr::unsynchronized_pool_resource resource_;
    std::unique_ptr<ruvia::testing::http3_quic_udp_pair> pair_;
    std::vector<char> first_part_;
    std::vector<char> final_part_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable startedCondition_;
    std::exception_ptr failure_;
    bool started_{};
    std::uint64_t synchronizationRequested_{};
    std::uint64_t synchronizationCompleted_{};
    std::atomic<bool> stop_{};
    std::atomic<bool> handshake_observed_{};
    std::atomic<bool> early_data_rejected_{};
    std::atomic<bool> early_data_accepted_{};
    std::atomic<bool> handshake_paused_{};
    std::atomic<bool> allow_handshake_{};
    std::atomic<std::size_t> request_payload_bytes_{};
    std::atomic<bool> first_part_ready_{};
    std::atomic<bool> allow_final_part_{};
    std::atomic<bool> final_part_sent_{};
    bool consumeRequest_{};
    bool webSocket_{};
    bool advertiseOrigins_{};
    std::optional<quic_push_scenario> push_{};
    bool tunnel_{};
    bool udp_{};
    bool early_data_{};
    ruvia::detail::http3_quic_tls_context* shared_server_tls_{};
    bool hold_handshake_on_early_decision_{};
    std::atomic<std::size_t> tunnel_bytes_{};
    std::atomic<std::size_t> cancelled_pushes_{};
    std::atomic<std::size_t> promised_pushes_{};
    std::atomic<int> push_priority_observed_{-1};
    std::atomic<unsigned> websocket_messages_{};
    std::atomic<bool> client_end_observed_{};
    std::atomic<int> priority_observed_{-1};
    std::atomic<std::uint64_t> priority_element_id_{};
    std::atomic<bool> priority_element_id_observed_{};
    std::atomic<std::uint64_t> peer_max_push_id_{};
    std::atomic<bool> peer_max_push_id_observed_{};
};

template <typename Predicate>
ruvia::Task<bool> wait_for_peer(const ruvia::WorkerHandle& worker, local_quic_response_peer& peer,
    Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        peer.rethrow_if_failed();
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        if (co_await ruvia::sleepFor(worker, 1ms) != ruvia::TimerSleepResult::kElapsed) {
            co_return false;
        }
    }
    peer.rethrow_if_failed();
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

ruvia::detail::HttpClientResponseState* reject_early_test_push(void*, std::size_t,
    Connection&, std::uint64_t, const ruvia::Http3MessageHead&) {
    return nullptr;
}

void finish_early_test_push(void*) noexcept {}

ruvia::Task<void> exerciseEarlyDataRejectionRecovery(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& ticketPeer, local_quic_response_peer& acceptingPeer,
    local_quic_response_peer& rejectingPeer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        ruvia::detail::ClientTransportConfigView tls_config{
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
        ruvia::detail::http3_quic_client_tls_context tls(tls_config);
        std::optional<ruvia::detail::http3_quic_client_tls_context::ticket_lease> ticket;
        {
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = ticketPeer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, nullptr, 30s, {}, {}, {}, {},
                ruvia::quic_version::v1, true);
            ruvia::detail::HttpClientRequestStorage request("GET", "/ticket", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            ticketPeer.allow_final_part();
            co_await connection.wait(submitted.id);
            const auto completed = connection.result(submitted.id);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
            }

            const bool ticket_ready = co_await wait_for_peer(worker, ticketPeer, [&] {
                if (!ticket) {
                    auto captured = tls.take_ticket("127.0.0.1", ruvia::quic_version::v1);
                    if (captured) {
                        ticket.emplace(std::move(*captured));
                    }
                }
                return ticket.has_value(); }, 8s);
            RUVIA_CHECK(ticket_ready);
            if (ticket) {
                RUVIA_CHECK(SSL_SESSION_get_max_early_data(ticket->session.get()) != 0);
                ruvia::detail::http3_quic_client_tls_context no_early_tls(tls_config);
                ruvia::detail::http3_quic_client_tls_context::ssl_session_owner no_early_session(
                    SSL_SESSION_dup(ticket->session.get()));
                RUVIA_CHECK(no_early_session != nullptr);
                if (no_early_session) {
                    SSL_SESSION_set_max_early_data(no_early_session.get(), 0);
                    no_early_tls.remember_ticket(no_early_session.get(), ticket->host,
                        ticket->version, ticket->transport_parameters, ticket->settings);
                    asio::ip::udp::socket reservation(io,
                        {asio::ip::address_v4::loopback(), 0});
                    const auto local = ruvia::detail::to_http3_quic_datagram_address(
                        reservation.local_endpoint());
                    const auto peer = ruvia::detail::to_http3_quic_datagram_address(
                        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), ticketPeer.port()));
                    RUVIA_CHECK(local.has_value() && peer.has_value());
                    if (local && peer) {
                        ruvia::quic_connection_config quic_config;
                        quic_config.local_address = ruvia::detail::to_quic_address(*local);
                        quic_config.peer_address = ruvia::detail::to_quic_address(*peer);
                        ruvia::detail::http3_quic_client_transport transport(
                            no_early_tls, quic_config, "127.0.0.1",
                            std::chrono::steady_clock::now(), &memory, true);
                        RUVIA_CHECK(!transport.early_data_enabled());
                    }
                }
                tls.remember_ticket(ticket->session.get(), ticket->host, ticket->version,
                    ticket->transport_parameters, ticket->settings);
            }
            connection.requestStop();
            co_await tasks.join();
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
        }
        // Reuse the original server TLS context so this peer accepts the real
        // ticket while its handshake is gated after the early-data decision.
        if (ticket) {
            tls.remember_ticket(ticket->session.get(), ticket->host, ticket->version,
                ticket->transport_parameters, ticket->settings);
        }
        {
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = acceptingPeer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, nullptr, 30s, {}, {}, {}, {},
                ruvia::quic_version::v1, true);
            ruvia::detail::HttpClientRequestStorage request("GET", "/accepted-early", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();

            const bool handshake_paused = co_await wait_for_peer(worker, acceptingPeer, [&] { return acceptingPeer.handshake_paused(); }, 8s);
            RUVIA_CHECK(handshake_paused);
            const bool early_request_finished = co_await wait_for_peer(worker, acceptingPeer, [&] { return connection_test_access::early_request_finished(connection, submitted.id); }, 8s);
            RUVIA_CHECK(early_request_finished);
            const auto early_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            RUVIA_CHECK(early_stream_id.has_value());
            RUVIA_CHECK(connection.reprioritize(submitted.id,
                {.urgency = 1, .incremental = true}));
            RUVIA_CHECK(connection_test_access::has_pending_priority_update(connection, submitted.id));

            acceptingPeer.allow_handshake();
            const bool priority_observed = co_await wait_for_peer(worker, acceptingPeer, [&] {
                const auto stream_id = connection_test_access::request_stream_id(connection, submitted.id);
                const auto element_id = acceptingPeer.priority_element_id();
                return acceptingPeer.priority_observed() == 0x101 && stream_id && element_id == stream_id; }, 8s);
            RUVIA_CHECK(priority_observed);
            RUVIA_CHECK(acceptingPeer.handshake_observed());
            RUVIA_CHECK(acceptingPeer.early_data_accepted());
            const auto accepted_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            const auto accepted_priority_id = acceptingPeer.priority_element_id();
            RUVIA_CHECK(accepted_stream_id.has_value() && accepted_priority_id == accepted_stream_id);

            acceptingPeer.allow_final_part();
            co_await connection.wait(submitted.id);
            const auto completed = connection.result(submitted.id);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
                RUVIA_CHECK_EQ(completed->status, std::uint16_t{200});
            }
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});
            connection.requestStop();
            co_await tasks.join();
        }

        // The second peer has independent ticket keys and must reject the same
        // valid ticket after the client has completely written its 0-RTT request.
        if (ticket) {
            tls.remember_ticket(ticket->session.get(), ticket->host, ticket->version,
                ticket->transport_parameters, ticket->settings);
        }
        {
            ruvia::detail::Http3ClientBodyBudget pushBudget(1024 * 1024);
            int pushObserverContext{};
            const ruvia::detail::Http3ClientPushObserver pushObserver{
                .context = &pushObserverContext,
                .config = {.enabled = true, .maxConcurrentPushes = 1},
                .receive = reject_early_test_push,
                .finished = finish_early_test_push};
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = rejectingPeer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, &pushBudget, 30s, {}, {}, {},
                pushObserver, ruvia::quic_version::v1, true);
            ruvia::detail::HttpClientRequestStorage request("GET", "/recover", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            const bool handshake_paused = co_await wait_for_peer(worker, rejectingPeer, [&] { return rejectingPeer.handshake_paused(); }, 8s);
            RUVIA_CHECK(handshake_paused);
            const bool early_request_finished = co_await wait_for_peer(worker, rejectingPeer, [&] { return connection_test_access::early_request_finished(connection, submitted.id); }, 8s);
            RUVIA_CHECK(early_request_finished);
            const auto early_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            RUVIA_CHECK(early_stream_id.has_value());
            RUVIA_CHECK(connection.reprioritize(submitted.id,
                {.urgency = 1, .incremental = true}));
            RUVIA_CHECK(connection_test_access::has_pending_priority_update(connection, submitted.id));

            rejectingPeer.allow_handshake();
            const bool peer_control_ready = co_await wait_for_peer(worker, rejectingPeer, [&] {
                const auto max_push_id = rejectingPeer.peer_max_push_id();
                const auto stream_id = connection_test_access::request_stream_id(connection, submitted.id);
                const auto element_id = rejectingPeer.priority_element_id();
                return max_push_id == std::optional<std::uint64_t>{0} &&
                       rejectingPeer.priority_observed() == 0x101 && stream_id && element_id == stream_id; }, 8s);
            RUVIA_CHECK(peer_control_ready);
            const auto replayed_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            const auto priority_element_id = rejectingPeer.priority_element_id();
            RUVIA_CHECK(replayed_stream_id.has_value() && priority_element_id == replayed_stream_id);
            RUVIA_CHECK(rejectingPeer.early_data_rejected());
            rejectingPeer.allow_final_part();
            co_await connection.wait(submitted.id);
            const auto completed = connection.result(submitted.id);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
                RUVIA_CHECK_EQ(completed->status, std::uint16_t{200});
            }
            RUVIA_CHECK(rejectingPeer.handshake_observed());
            RUVIA_CHECK(rejectingPeer.early_data_rejected());
            RUVIA_CHECK(rejectingPeer.request_payload_bytes() != 0);
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});
            RUVIA_CHECK(connection.accepting());

            ruvia::detail::HttpClientRequestStorage cancelled_request("GET", "/cancel", &memory);
            cancelled_request.set_replay_safe(true);
            const auto cancelled = connection.submit(std::move(cancelled_request),
                std::chrono::steady_clock::now() + 5s);
            RUVIA_CHECK(cancelled.outcome == Connection::Outcome::kPending);
            const bool cancel_stream_opened = co_await wait_for_peer(worker, rejectingPeer, [&] { return connection.active_response_streams() != 0; }, 2s);
            RUVIA_CHECK(cancel_stream_opened);
            connection.cancel(cancelled.id);
            co_await connection.wait(cancelled.id);
            const auto cancelled_result = connection.result(cancelled.id);
            RUVIA_CHECK(cancelled_result != nullptr &&
                        cancelled_result->outcome == Connection::Outcome::kCancelled);
            RUVIA_CHECK(connection.release(cancelled.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});

            ruvia::detail::HttpClientRequestStorage timed_request("GET", "/timeout", &memory);
            timed_request.set_replay_safe(true);
            const auto timed = connection.submit(std::move(timed_request),
                std::chrono::steady_clock::now() + 500ms);
            RUVIA_CHECK(timed.outcome == Connection::Outcome::kPending);
            const bool timeout_stream_opened = co_await wait_for_peer(worker, rejectingPeer, [&] { return connection.active_response_streams() != 0; }, 2s);
            RUVIA_CHECK(timeout_stream_opened);
            co_await connection.wait(timed.id);
            const auto timed_result = connection.result(timed.id);
            RUVIA_CHECK(timed_result != nullptr &&
                        timed_result->outcome == Connection::Outcome::kDeadline);
            RUVIA_CHECK(connection.release(timed.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});

            connection.requestStop();
            co_await tasks.join();
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

ruvia::Task<void> exerciseRealResponse(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, local_quic_response_peer& peer,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
                const bool gotHead = co_await wait_for_peer(worker, peer, [&] { return peer.first_part_ready() && response.headReady; }, 8s);
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
                        peer.allow_final_part();
                        const bool gotFinal = co_await wait_for_peer(worker, peer, [&] { return peer.final_part_sent() && response.pending.size() == 3; }, 3s);
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
    local_quic_response_peer& firstPeer, local_quic_response_peer& secondPeer,
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
                    ruvia::detail::http3_quic_client_tls_context tls(config);
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
                            const bool gotHead = co_await wait_for_peer(worker, firstPeer, [&] { return firstPeer.first_part_ready() && firstState.headReady; }, 8s);
                            RUVIA_CHECK(gotHead);
                            firstPeer.allow_final_part();
                            const bool completed = co_await wait_for_peer(worker, firstPeer, [&] { return firstPeer.final_part_sent() && firstState.complete; }, 5s);
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
                    ruvia::detail::http3_quic_client_tls_context tls(config);
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
                            const bool blockedAtBudget = co_await wait_for_peer(worker, secondPeer, [&] { return secondPeer.first_part_ready() && secondState.headReady &&
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
                            secondPeer.allow_final_part();
                            const bool completed = co_await wait_for_peer(worker, secondPeer, [&] { return secondPeer.final_part_sent() && secondState.complete; }, 5s);
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
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        CountingResource response_memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState response(worker, &response_memory);
            ruvia::detail::HttpClientRequestStorage request("GET", "/protocol-priority", &memory);
            const auto deadline = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool gotHead = co_await wait_for_peer(worker, peer, [&] { return peer.first_part_ready() && response.headReady; }, 8s);
                RUVIA_CHECK(gotHead);
                if (gotHead) {
                    RUVIA_CHECK_EQ(response.pending, std::string_view("abc"));
                    response_memory.rejectAllocations();
                    peer.allow_final_part();
                    const bool terminal = co_await wait_for_peer(
                        worker, peer, [&] { return response.complete; }, 4s);
                    RUVIA_CHECK(terminal);
                    if (terminal) {
                        RUVIA_CHECK(response_memory.rejectedAllocations != 0);
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

            response_memory.rejectAllocations(false);
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
        RUVIA_CHECK_EQ(response_memory.allocations, response_memory.returns);
        RUVIA_CHECK_EQ(response_memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseParserRegistrationAllocationFailure(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
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
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
            // Registration failure can close the client before the peer has
            // confirmed the TLS handshake. Only the no-request-payload contract
            // is synchronized here; peer handshake readiness is not guaranteed.
            RUVIA_CHECK_EQ(peer.request_payload_bytes(), std::size_t{0});

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
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
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
            RUVIA_CHECK(peer.handshake_observed());
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

ruvia::Task<void> exercise_migration_retirement(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    CountingResource memory;
    std::optional<std::uint64_t> migration_id;
    {
        ruvia::detail::ClientTransportConfigView tls_config{
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
        ruvia::detail::http3_quic_client_tls_context tls(tls_config);
        ruvia::TaskScope tasks(worker, {.resource = &memory});
        Connection connection(io, worker, tasks, tls,
            ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
            &memory, 4, 16 * 1024 * 1024, 2s);
        ruvia::detail::HttpClientRequestStorage request("GET", "/public-pool", &memory);
        const auto submitted = connection.submit(
            std::move(request), std::chrono::steady_clock::now() + 10s);
        RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
        connection.start();
        peer.allow_final_part();
        co_await connection.wait(submitted.id);
        const auto completed = connection.result(submitted.id);
        RUVIA_CHECK(completed != nullptr);
        if (completed != nullptr) {
            RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
        }

        asio::ip::udp::socket reservation(
            io, {asio::ip::address_v4::loopback(), 0});
        const auto local_endpoint = reservation.local_endpoint();
        reservation.close();
        const auto migration = connection.start_path_migration(local_endpoint);
        RUVIA_CHECK(migration.status == ruvia::quic_migration_status::started ||
                    migration.status == ruvia::quic_migration_status::validated);
        if (migration.status == ruvia::quic_migration_status::started ||
            migration.status == ruvia::quic_migration_status::validated) {
            migration_id = migration.id;
            const bool validated = co_await wait_for_peer(worker, peer, [&] {
                const auto status = connection.path_migration(*migration_id);
                return status && status->status != ruvia::quic_migration_status::started; }, 8s);
            RUVIA_CHECK(validated);
            const auto status = connection.path_migration(*migration_id);
            RUVIA_CHECK(status.has_value());
            if (status) {
                RUVIA_CHECK(status->status == ruvia::quic_migration_status::validated);
            }
        }

        const bool retired = co_await wait_for_peer(
            worker, peer, [&] { return !connection.running(); }, 5s);
        RUVIA_CHECK(retired);
        if (migration_id) {
            const auto result = connection.path_migration(*migration_id);
            RUVIA_CHECK(result.has_value());
            if (result) {
                RUVIA_CHECK(result->status == ruvia::quic_migration_status::validated);
            }
        }
        connection.requestStop();
        co_await tasks.join();
        if (submitted.outcome == Connection::Outcome::kPending) {
            RUVIA_CHECK(connection.release(submitted.id));
        }
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    attachment.stop();
}

ruvia::Task<void> exercisePublicHttp3ClientPool(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx,
    bool observeOrigins = false, std::string_view caFile = {}, bool migrate_quic_path = false) {
    CountingResource memory;
    std::exception_ptr failure;
    std::optional<ruvia::HttpClientAdvertisement> retainedAdvertisement;
    std::optional<std::uint64_t> migration_id;
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
            .advertisements = {.receiveOrigins = observeOrigins},
            .tlsPeerVerification = observeOrigins ? ruvia::TlsPeerVerificationPolicy::kVerify : ruvia::TlsPeerVerificationPolicy::kSkipVerification,
            .caFile = std::string(caFile),
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
                RUVIA_CHECK(peer.first_part_ready());
                RUVIA_CHECK(!peer.final_part_sent());

                if (observeOrigins) {
                    retainedAdvertisement = pool.nextAdvertisement();
                    RUVIA_CHECK(retainedAdvertisement && retainedAdvertisement->origins());
                    if (retainedAdvertisement && retainedAdvertisement->origins()) {
                        RUVIA_CHECK(retainedAdvertisement->origins()->origins.size() == 2);
                        RUVIA_CHECK(retainedAdvertisement->origins()->origins[0] == "https://127.0.0.1");
                    }
                }

                response.reprioritize({.urgency = 1, .incremental = true});
                const bool priorityReceived = co_await wait_for_peer(worker, peer, [&] { return peer.priority_observed() == 0x101; }, 2s);
                RUVIA_CHECK(priorityReceived);

                auto first = co_await response.body().text();
                RUVIA_CHECK(first.has_value());
                if (first) {
                    RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                }
                peer.allow_final_part();
                auto second = co_await response.body().text();
                RUVIA_CHECK(second.has_value());
                if (second) {
                    RUVIA_CHECK_EQ(*second, std::string_view("def"));
                }
                auto end = co_await response.body().text();
                RUVIA_CHECK(!end.has_value());
                RUVIA_CHECK(response.body().complete());
                bool retiredPriorityRejected = false;
                try {
                    response.reprioritize({});
                } catch (const ruvia::HttpClientError& error) {
                    retiredPriorityRejected = error.code() == ruvia::HttpClientError::Code::kClosing;
                }
                RUVIA_CHECK(retiredPriorityRejected);
            }
            const bool completed = co_await wait_for_peer(worker, peer, [&] { return pool.stats().completedRequests == 1; }, 2s);
            RUVIA_CHECK(completed);
            const auto stats = pool.stats();
            RUVIA_CHECK_EQ(stats.completedRequests, std::size_t{1});
            RUVIA_CHECK_EQ(stats.failedRequests, std::size_t{0});
            RUVIA_CHECK_EQ(stats.inFlightRequests, std::size_t{0});
            if (migrate_quic_path) {
                asio::ip::udp::socket reservation(
                    io, {asio::ip::address_v4::loopback(), 0});
                const auto local_endpoint = reservation.local_endpoint();
                reservation.close();
                const auto migration = pool.start_quic_path_migration(local_endpoint);
                RUVIA_CHECK(migration.status == ruvia::quic_migration_status::started ||
                            migration.status == ruvia::quic_migration_status::validated);
                if (migration.status == ruvia::quic_migration_status::started ||
                    migration.status == ruvia::quic_migration_status::validated) {
                    migration_id = migration.id;
                    const bool settled = co_await wait_for_peer(worker, peer, [&] {
                        const auto status = pool.path_migration(*migration_id);
                        return status && status->status != ruvia::quic_migration_status::started; }, 8s);
                    RUVIA_CHECK(settled);
                    const auto status = pool.path_migration(*migration_id);
                    RUVIA_CHECK(status.has_value());
                    if (status) {
                        RUVIA_CHECK(status->status == ruvia::quic_migration_status::validated);
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        pool.closeNow();
        co_await pool.join();
        if (migration_id) {
            const auto retired = pool.path_migration(*migration_id);
            RUVIA_CHECK(retired.has_value());
            if (retired) {
                RUVIA_CHECK(retired->status == ruvia::quic_migration_status::validated);
            }
        }
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    if (observeOrigins && retainedAdvertisement && retainedAdvertisement->origins()) {
        RUVIA_CHECK(retainedAdvertisement->origins()->origins[1] == "https://localhost");
        RUVIA_CHECK(retainedAdvertisement->protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
    }
    retainedAdvertisement.reset();
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
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseRealResponse(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientReplaysBodylessGetAfterRejectedEarlyDataOnRebuiltCriticalStreams) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    auto serverTlsConfig = ruvia::detail::HttpServerListenerDefinition::Tls{};
    serverTlsConfig.identity.certificateChainFile = identity.certificate().string();
    serverTlsConfig.identity.privateKeyFile = identity.privateKey().string();
    serverTlsConfig.http3_early_data = true;
    ruvia::detail::http3_quic_tls_context shared_server_tls(
        serverTlsConfig, std::pmr::get_default_resource());
    local_quic_response_peer ticketPeer(identity, false, true, false, false, {}, false, false,
        true, &shared_server_tls);
    local_quic_response_peer acceptingPeer(identity, false, true, false, false, {}, false, false,
        true, &shared_server_tls, true);
    // This independent server TLS context must reject the otherwise valid ticket.
    local_quic_response_peer rejectingPeer(identity, false, true, false, false, {}, false, false,
        true, nullptr, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseEarlyDataRejectionRecovery(
        io, worker, attachment, ticketPeer, acceptingPeer, rejectingPeer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientPoolPublishesIncrementalResponseThroughPublicResponseApi) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionRetainsValidatedMigrationAfterIdleSessionRetirement) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_migration_retirement(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientPoolRetainsValidatedMigrationAfterSessionRetirement) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx, false, {}, true));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3_client_observes_origin_frame_over_authenticated_quic_and_retains_after_shutdown) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, false, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    const auto caFile = identity.certificate().string();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx, true, caFile));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionResponseReleasePreservesDataAndWakesTheNextBudgetGeneration) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer firstPeer(identity);
    local_quic_response_peer secondPeer(identity);
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
    local_quic_response_peer peer(identity);
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
    local_quic_response_peer peer(identity, true);
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
    local_quic_response_peer peer(identity, false, false);
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

RUVIA_TEST(http3_websocket_client_drives_extended_connect_dynamic_qpack_deflate_and_fin) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    const auto run = [&]() -> ruvia::Task<void> {
        ruvia::WebSocketClient client(attachment.loop(), {.scheme = ruvia::WebSocketScheme::kWss,
                                                             .protocol = ruvia::WebSocketClientProtocol::kHttp3,
                                                             .host = "127.0.0.1",
                                                             .port = peer.port(),
                                                             .subprotocols = {"chat"},
                                                             .deflate = {.enabled = true},
                                                             .connectTimeout = 5s,
                                                             .readTimeout = 2s,
                                                             .writeTimeout = 2s,
                                                             .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            RUVIA_CHECK_EQ(client.subprotocol(), std::string_view("chat"));
            auto greeting = co_await client.read();
            RUVIA_CHECK(greeting.has_value());
            if (greeting) {
                RUVIA_CHECK_EQ(greeting->payload(), std::string(100000, 'w'));
            }
            const std::string payload(100000, 'c');
            co_await client.binary(payload, {.compress = false});
            co_await client.binary(payload, {.compress = true});
            RUVIA_CHECK(!(co_await client.read()).has_value());
            const bool finObserved = co_await wait_for_peer(worker, peer, [&] { return peer.client_end_observed(); }, 2s);
            RUVIA_CHECK(finObserved);
            RUVIA_CHECK_EQ(peer.websocket_messages(), 2U);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
}

RUVIA_TEST(http3ClientUploadExchangeWakesQuicDriverAndWaitsForContinueWithoutWriteTimeout) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps,
                                                        .host = "127.0.0.1",
                                                        .port = peer.port(),
                                                        .connectTimeout = 10s,
                                                        .writeTimeout = 100ms,
                                                        .requestTimeout = 12s,
                                                        .protocol = ruvia::HttpClientProtocol::kHttp3Only,
                                                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification});
        std::exception_ptr failure;
        try {
            auto exchange = co_await client.openRequest({.method = "POST", .target = "/upload"},
                {.contentLength = 6, .expectation = ruvia::HttpClientRequestExpectation::kContinue, .continueTimeout = 200ms});
            co_await exchange.body().write("abc");
            // Waiting for an application producer does not consume write inactivity.
            (void)co_await ruvia::sleepFor(worker, 150ms);
            co_await exchange.body().write("def");
            const std::array<ruvia::HttpHeaderView, 1> trailers{{{"x-end", "retained"}}};
            co_await exchange.body().end(trailers);
            RUVIA_CHECK(exchange.body().complete());
            auto response = co_await exchange.response();
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto first = co_await response.body().text();
            RUVIA_CHECK(first && *first == "abc");
            peer.allow_final_part();
            auto rest = co_await response.body().readAll(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(rest.bytes().data()), rest.size()), "def");
            RUVIA_CHECK(peer.request_payload_bytes() > 6);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
#endif
}

RUVIA_TEST(http3_client_push_owns_promises_reclaims_repeated_streams_and_retains_results_after_shutdown) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, false, false, quic_push_scenario{.count = 100, .stream_before_promise = true});
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    CountingResource memory;
    auto task = [&]() -> ruvia::Task<void> {
        std::exception_ptr failure;
        std::optional<ruvia::HttpClientPush> saved;
        std::optional<ruvia::HttpClientResponse> savedResponse;
        {
            ruvia::HttpClientConfig config{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .connectTimeout = 5s, .requestTimeout = 10s, .maxResponseBytes = 2 * 1024 * 1024, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .push = {.enabled = true, .maxConcurrentPushes = 1, .timeout = 5s}, .caFile = identity.certificate().string()};
            ruvia::detail::HttpClientPool client(io, worker, ruvia::detail::HttpClientConfigStorage(config, &memory), ruvia::HttpClientResultBudgetConfig{}, &memory);
            std::exception_ptr operationFailure;
            try {
                auto parent = co_await client.execute(ruvia::detail::HttpClientRequestStorage("GET", "/parent", &memory), {});
                for (std::size_t index = 0; index < 100; ++index) {
                    std::optional<ruvia::HttpClientPush> push;
                    const auto available = co_await wait_for_peer(worker, peer, [&] { push = client.nextPush(); return push.has_value(); }, 5s);
                    RUVIA_CHECK(available);
                    if (!push) {
                        throw std::runtime_error("HTTP/3 push did not arrive");
                    }
                    RUVIA_CHECK(std::string_view(push->request().path) == "/push/" + std::to_string(index));
                    RUVIA_CHECK(push->request().headers[0].name() == "x-promise");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        (void)push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    RUVIA_CHECK(response.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
                    auto bytes = co_await response.body().readAll(32);
                    RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "ppppp");
                    if (index == 0) {
                        saved.emplace(std::move(moved));
                        savedResponse.emplace(std::move(response));
                    }
                    RUVIA_CHECK(saved->request().path == "/push/0");
                }
                peer.allow_final_part();
                auto bytes = co_await parent.body().readAll(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "abcdef");
                RUVIA_CHECK(client.stats().receivedPushes == 100 && client.stats().rejectedPushes == 0);
            } catch (...) {
                operationFailure = std::current_exception();
            }
            client.closeNow();
            co_await client.join();
            failure = operationFailure;
        }
        if (saved && savedResponse) {
            RUVIA_CHECK(saved->request().path == "/push/0");
            RUVIA_CHECK(savedResponse->status().value() == 200 && savedResponse->body().complete());
        }
        savedResponse.reset();
        saved.reset();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(task());
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
    RUVIA_CHECK(memory.liveBytes == 0 && memory.allocations == memory.returns);
}

RUVIA_TEST(http3_client_push_bounds_origin_validation_errors_and_cancellation_preserve_parent) {
    TestIdentityFiles identity;
    const std::array scenarios{
        quic_push_scenario{.count = 2, .body_bytes = 1024 * 1024},
        quic_push_scenario{.promise_only = true},
        quic_push_scenario{.cancel_before_promise = true},
        quic_push_scenario{.malformed_response = true},
        quic_push_scenario{.cross_origin = true},
    };
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        local_quic_response_peer peer(identity, false, true, false, false, scenarios[index]);
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        const auto worker = attachment.loop().handle();
        auto task = [&]() -> ruvia::Task<void> {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .connectTimeout = 5s, .requestTimeout = 10s, .maxResponseBytes = 2 * 1024 * 1024, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .push = {.enabled = true, .maxQueuedPushes = 1, .maxConcurrentPushes = 2, .timeout = 100ms}, .caFile = identity.certificate().string()});
            std::exception_ptr operationFailure;
            try {
                auto parent = co_await client.send({.method = "GET", .target = "/parent"});
                std::optional<ruvia::HttpClientPush> push;
                if (index == 0) {
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().rejectedPushes == 1; }, 2s));
                }
                if (index != 2 && index != 4) {
                    const auto available = co_await wait_for_peer(worker, peer, [&] { push = client.nextPush(); return push.has_value(); }, 5s);
                    RUVIA_CHECK(available);
                    if (!push) {
                        throw std::runtime_error("expected HTTP/3 promise did not arrive");
                    }
                    if (index == 0) {
                        push.reset();
                        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.cancelled_pushes() >= 1; }, 2s));
                        RUVIA_CHECK(client.stats().rejectedPushes == 1);
                    } else {
                        bool failed = false;
                        try {
                            auto response = co_await push->response();
                            (void)co_await response.body().readAll(32);
                        } catch (const ruvia::HttpClientError& error) {
                            failed = error.code() == (index == 1 ? ruvia::HttpClientError::Code::kTimeout : ruvia::HttpClientError::Code::kProtocolError);
                        }
                        RUVIA_CHECK(failed);
                    }
                } else {
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.promised_pushes() == 1; }, 2s));
                    (void)co_await ruvia::sleepFor(worker, 50ms);
                    auto cancelled_push = client.nextPush();
                    if (index == 2) {
                        // QUIC does not order the control and request streams.
                        // A promise may arrive before its cancellation, but must
                        // never expose a usable response after cancellation.
                        if (cancelled_push) {
                            bool cancelled{};
                            try {
                                (void)co_await cancelled_push->response();
                            } catch (const ruvia::HttpClientError& error) {
                                cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
                            }
                            RUVIA_CHECK(cancelled);
                        }
                        RUVIA_CHECK(client.stats().receivedPushes <= 1);
                    } else {
                        RUVIA_CHECK(!cancelled_push);
                        RUVIA_CHECK(client.stats().receivedPushes == 0);
                        RUVIA_CHECK(client.stats().rejectedPushes == 1);
                    }
                }
                peer.allow_final_part();
                auto bytes = co_await parent.body().readAll(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "abcdef");
            } catch (...) {
                operationFailure = std::current_exception();
            }
            client.close();
            co_await client.shutdown();
            attachment.stop();
            if (operationFailure) {
                std::rethrow_exception(operationFailure);
            }
        };
        auto root = attachment.loop().start(task());
        attachment.run();
        root.get();
        peer.rethrow_if_failed();
    }
}

RUVIA_TEST(http3_client_push_streaming_priority_disabled_permission_and_shutdown_are_worker_owned) {
    TestIdentityFiles identity;
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        local_quic_response_peer peer(identity, false, true, false, false,
            quic_push_scenario{.body_bytes = 1280 * 1024, .promise_only = scenario == 1});
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        const auto worker = attachment.loop().handle();
        auto task = [&]() -> ruvia::Task<void> {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .connectTimeout = 5s, .requestTimeout = 10s, .maxResponseBytes = 2 * 1024 * 1024, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .push = {.enabled = scenario != 2, .maxConcurrentPushes = 1, .timeout = std::nullopt}, .caFile = identity.certificate().string()});
            std::exception_ptr failure;
            std::optional<ruvia::HttpClientPush> retained;
            try {
                auto parent = co_await client.send({.method = "GET", .target = "/parent"});
                if (scenario != 2) {
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { retained = client.nextPush(); return retained.has_value(); }, 2s));
                    if (!retained) {
                        throw std::runtime_error("expected push missing");
                    }
                    if (scenario == 0) {
                        auto response = co_await retained->response();
                        response.reprioritize({.urgency = 1, .incremental = true});
                        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.push_priority_observed() == 0x101; }, 2s));
                        std::size_t bytes{};
                        while (auto part = co_await response.body().text()) {
                            RUVIA_CHECK(part->find_first_not_of('p') == std::string_view::npos);
                            bytes += part->size();
                        }
                        RUVIA_CHECK(bytes == 1280 * 1024);
                    }
                } else {
                    RUVIA_CHECK(!client.nextPush());
                    RUVIA_CHECK(client.stats().receivedPushes == 0 && peer.promised_pushes() == 0);
                }
                peer.allow_final_part();
                auto bytes = co_await parent.body().readAll(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "abcdef");
            } catch (...) {
                failure = std::current_exception();
            }
            client.close();
            co_await client.shutdown();
            if (scenario == 1 && retained) {
                bool cancelled = false;
                try {
                    (void)co_await retained->response();
                } catch (const ruvia::HttpClientError& error) {
                    cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
                }
                RUVIA_CHECK(cancelled);
                RUVIA_CHECK(retained->request().path == "/push/0");
            }
            retained.reset();
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto root = attachment.loop().start(task());
        attachment.run();
        root.get();
        peer.rethrow_if_failed();
    }
}

RUVIA_TEST(http3_client_tunnel_preserves_metadata_inputs_and_both_half_close_orders) {
    for (unsigned round = 0; round != 4; ++round) {
        TestIdentityFiles identity;
        local_quic_response_peer peer(identity, false, true, false, false, {}, true);
        if (round >= 2) {
            peer.allow_final_part();
        }
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::Task<void> {
            const auto& worker = attachment.loop().handle();
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .requestTimeout = 5s, .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
            std::optional<ruvia::HttpClientTunnel> retired_tunnel;
            try {
                const bool extended = (round & 1) != 0;
                auto result = co_await client.openTunnel({.authority = extended ? "proxy.test" : "target.test:443", .protocol = extended ? "test-protocol" : "", .target = extended ? "/tunnel" : ""});
                RUVIA_CHECK(result.tunnel() && !result.response());
                if (!result.tunnel()) {
                    throw std::runtime_error("missing CONNECT tunnel");
                }
                retired_tunnel.emplace(std::move(*result.tunnel()));
                auto& tunnel = *retired_tunnel;
                RUVIA_CHECK(tunnel.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                std::string greeting;
                while (greeting.size() < 100003) {
                    const auto bytes = co_await tunnel.read();
                    if (!bytes) {
                        throw std::runtime_error("early tunnel FIN");
                    }
                    greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                }
                RUVIA_CHECK(greeting.starts_with(std::string(100003, 's')));
                if (round >= 2) {
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                }
                std::string payload(120003, 't');
                for (std::size_t offset = 0; offset != payload.size();) {
                    const auto count = std::min<std::size_t>(16384, payload.size() - offset);
                    std::string owned = payload.substr(offset, count);
                    auto output = tunnel.write(std::string_view(owned));
                    owned.assign("mutated");
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await tunnel.finish();
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.client_end_observed(); }, 2s));
                RUVIA_CHECK_EQ(peer.tunnel_bytes(), payload.size());
                peer.allow_final_part();
                if (round < 2) {
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                }
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().completedRequests == 1; }, 2s));
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().inFlightRequests == 0; }, 2s));
                // Normal bidirectional retirement must not undo a successfully
                // completed sending direction or make finish non-idempotent.
                co_await tunnel.finish();
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)tunnel.write("late"); }));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (!failure && retired_tunnel) {
                try {
                    co_await retired_tunnel->finish();
                    RUVIA_CHECK(retired_tunnel->header("x-tunnel") == "owned-metadata");
                    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)retired_tunnel->write("late after shutdown"); }));
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
        RUVIA_CHECK(peer.synchronize());
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}

RUVIA_TEST(http3_client_udp_tunnel_negotiates_capsules_owns_packets_and_keeps_send_open_after_peer_fin) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, false, false, {}, false, true);
    peer.allow_final_part();
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        std::optional<ruvia::HttpUdpDatagram> retained;
        {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .requestTimeout = 5s, .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
            try {
                auto result = co_await client.openUdpTunnel({.target = "/udp"}, {.maxChunkBytes = 1024});
                if (!result.tunnel()) {
                    throw std::runtime_error("CONNECT-UDP rejected");
                }
                RUVIA_CHECK(result.tunnel()->header("capsule-protocol") == "?1");
                auto udp = std::move(*result.tunnel()).udp();
                retained = co_await udp.read();
                RUVIA_CHECK(retained && retained->payload().size() == 16003);
                RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte byte) { return byte == std::byte{'s'}; }));
                auto empty = co_await udp.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await udp.read()));
                std::string bytes(16003, 't');
                auto output = udp.send(bytes);
                bytes.assign("mutated");
                co_await std::move(output);
                co_await udp.send("");
                co_await udp.finish();
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.client_end_observed(); }, 2s));
                RUVIA_CHECK_EQ(peer.tunnel_bytes(), std::size_t{16003});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload().size() == 16003);
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    RUVIA_CHECK(peer.synchronize());
    if (failure) {
        std::rethrow_exception(failure);
    }
}

RUVIA_TEST(http3_public_client_pool_preserves_owned_response_and_priority) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exercisePublicHttp3ClientPool(
        io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
}

namespace {
ruvia::Task<void> nativeDatagramEcho(void*, ruvia::Context& context) {
    auto datagrams = context.tunnel().datagrams();
    while (auto packet = co_await datagrams.read()) {
        co_await datagrams.send(packet->payload());
    }
    co_await datagrams.finish();
}
ruvia::Task<void> nativeUdpEcho(void*, ruvia::Context& context) {
    ruvia::HttpUdpTunnel datagrams(context.tunnel().datagrams());
    while (auto packet = co_await datagrams.read()) {
        co_await datagrams.send(packet->payload());
    }
    co_await datagrams.finish();
}
}  // namespace
RUVIA_TEST(http3_native_datagram_server_and_client_route_packets_capsules_verify_tls_and_retire_fixed_workers) {
    TestIdentityFiles identity;
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    routes.registerTunnelRoute("test-datagram", std::pmr::string("/datagrams"), {nullptr, nativeDatagramEcho}, {}, {}, {.datagrams = true});
    routes.registerTunnelRoute("connect-udp", std::pmr::string("/udp/:host/:port"), {nullptr, nativeUdpEcho}, {}, {});
    routes.finalize();
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = identity.certificate().string();
    tls.identity.privateKeyFile = identity.privateKey().string();
    const std::array listeners{ruvia::detail::HttpServerListenerDefinition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::Http3ListenConfig{})};
    auto configuration = ruvia::detail::validateHttpServerConfiguration(listeners,
        {.workerMailboxCapacity = 32, .maxConnections = 4, .maxRequestsPerConnection = 64});
    ruvia::detail::WebWorkerRuntime first(configuration, routes.routeTable(), {});
    ruvia::detail::WebWorkerRuntime second(configuration, routes.routeTable(), {});
    first.prepare();
    second.prepare();
    const auto target = [](ruvia::detail::WebWorkerRuntime& worker) {
        return ruvia::detail::ServerNetworkRuntime::Target{
            .submission = worker.networkSubmission(), .object = &worker, .available = [](void* raw) noexcept { return static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->availableForNetworkDispatch(); }, .accept = [](void* raw, ruvia::detail::NativeAcceptedSocketTicket&& ticket) noexcept { static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->acceptTransferredConnection(std::move(ticket)); }, .http3Server = worker.http3Server(), .http3MaxConnections = 4, .http3MailboxCapacity = 32, .http3MaxRequestsPerConnection = 64};
    };
    const std::array targets{target(first), target(second)};
    ruvia::detail::ServerNetworkRuntime network(listeners, targets);
    network.prepare();
    network.launch();
    network.waitUntilReady();
    first.launch();
    second.launch();
    first.waitUntilReady();
    second.waitUntilReady();
    first.requestServe();
    second.requestServe();
    RUVIA_CHECK(first.waitUntilServing());
    RUVIA_CHECK(second.waitUntilServing());
    network.requestServe();
    RUVIA_CHECK(network.waitUntilServing());
    std::exception_ptr failure;
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        std::optional<ruvia::HttpDatagram> retained;
        {
            ruvia::HttpClient client(attachment.loop(), {.host = "127.0.0.1", .port = network.localEndpoint(0).port(), .requestTimeout = 5s, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
            try {
                auto opened = co_await client.openTunnel({.authority = "target.test:443", .protocol = "test-datagram", .target = "/datagrams"}, {.datagrams = true});
                if (!opened.tunnel()) {
                    throw std::runtime_error("native HTTP Datagram tunnel rejected");
                }
                auto datagrams = std::move(*opened.tunnel()).datagrams();
                std::string input(1000, 'n');
                auto send = datagrams.send(input);
                input.assign("changed");
                co_await std::move(send);
                retained = co_await datagrams.read();
                RUVIA_CHECK(retained && retained->payload().size() == 1000);
                RUVIA_CHECK(retained && retained->transport() == ruvia::HttpDatagramTransport::kQuic);
                RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte b) { return b == std::byte{'n'}; }));
                co_await datagrams.send("");
                auto empty = co_await datagrams.read();
                RUVIA_CHECK(empty && empty->payload().empty() && empty->transport() == ruvia::HttpDatagramTransport::kQuic);
                co_await datagrams.send(std::string(16003, 'c'));
                auto reliable = co_await datagrams.read();
                RUVIA_CHECK(reliable && reliable->payload().size() == 16003 && reliable->transport() == ruvia::HttpDatagramTransport::kCapsule);
                co_await datagrams.finish();
                RUVIA_CHECK(!(co_await datagrams.read()));
                auto udpOpened = co_await client.openUdpTunnel({.target = "/udp/target.test/443"});
                if (!udpOpened.tunnel()) {
                    throw std::runtime_error("native CONNECT-UDP rejected");
                }
                auto udp = std::move(*udpOpened.tunnel()).udp();
                co_await udp.send("native UDP");
                auto packet = co_await udp.read();
                RUVIA_CHECK(packet && packet->payload().size() == 10 && packet->transport() == ruvia::HttpDatagramTransport::kQuic);
                co_await udp.send("");
                auto udpEmpty = co_await udp.read();
                RUVIA_CHECK(udpEmpty && udpEmpty->payload().empty());
                co_await udp.finish();
                RUVIA_CHECK(!(co_await udp.read()));
                auto pendingOpened = co_await client.openUdpTunnel({.target = "/udp/target.test/443"});
                if (!pendingOpened.tunnel()) {
                    throw std::runtime_error("pending native CONNECT-UDP rejected");
                }
                auto pending = std::move(*pendingOpened.tunnel()).udp();
                ruvia::TaskScope reads(worker);
                bool cancelled{};
                auto read = [&]() -> ruvia::Task<void> {try {static_cast<void>(co_await pending.read());} catch(const ruvia::HttpClientError&) {cancelled=true;} };
                reads.spawn(read());
                co_await ruvia::sleepFor(worker, 1ms);
                pending.abort();
                co_await reads.join();
                RUVIA_CHECK(cancelled);
                // Start another connection so both fixed worker bindings are exercised.
                ruvia::HttpClient other(attachment.loop(), {.host = "127.0.0.1", .port = network.localEndpoint(0).port(), .requestTimeout = 5s, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
                try {
                    auto openedOther = co_await other.openUdpTunnel({.target = "/udp/target.test/443"});
                    if (!openedOther.tunnel()) {
                        throw std::runtime_error("second worker UDP rejected");
                    }
                    auto packets = std::move(*openedOther.tunnel()).udp();
                    co_await packets.send("second worker");
                    auto result = co_await packets.read();
                    RUVIA_CHECK(result && result->payload().size() == 13);
                    packets.abort();
                } catch (...) {
                    failure = std::current_exception();
                }
                co_await other.shutdown();
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload().size() == 1000);
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    first.stopAdmission();
    second.stopAdmission();
    network.stop();
    network.join();
    first.finalizeAfterNetworkQuiesced();
    second.finalizeAfterNetworkQuiesced();
    first.join();
    second.join();
    network.rethrowFailure();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
