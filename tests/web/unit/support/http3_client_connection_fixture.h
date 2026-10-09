#pragma once

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
#include <future>
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
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
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
#include "ruvia/http/Http3ClientRequestHead.h"
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

#include "client/HttpClientPool.h"
#include "client/HttpClientResponseState.h"
#include "http3/Http3ClientConnection.h"
#include "http3/Http3QuicClientTlsContext.h"
#include "http3/Http3QuicClientTransport.h"
#include "http3/Http3QuicSocketAddress.h"
#include "http3_quic_udp_pair.h"
#include "router/RouterImpl.h"
#include "server/HttpServerOptionsValidation.h"
#include "server/WebWorkerRuntime.h"
#include "server/acceptor.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

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

namespace http3_client_connection_test {

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
            EVP_PKEY_CTX* rawContext = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            if (rawContext == nullptr) {
                throw std::runtime_error("failed to create test key generator");
            }
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
                rawContext, EVP_PKEY_CTX_free);
            EVP_PKEY* rawKey = nullptr;
            if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
                EVP_PKEY_generate(context.get(), &rawKey) <= 0) {
                throw std::runtime_error("failed to generate HTTP/3 test key");
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
                throw std::runtime_error("failed to create HTTP/3 test certificate");
            }
            const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
            const auto* name = reinterpret_cast<const unsigned char*>("localhost");
            if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, name, -1, -1, 0) != 1 ||
                X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
                X509_set_issuer_name(certificate.get(), subject.get()) != 1) {
                throw std::runtime_error("failed to name HTTP/3 test identity");
            }
            const std::array extensions{std::pair{NID_subject_alt_name, "IP:127.0.0.1,DNS:localhost"},
                std::pair{NID_basic_constraints, "critical,CA:TRUE"}};
            for (const auto& [nid, value] : extensions) {
                std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                    X509V3_EXT_nconf_nid(nullptr, nullptr, nid, value), X509_EXTENSION_free);
                if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1) {
                    throw std::runtime_error("failed to authenticate HTTP/3 test identity");
                }
            }
            if (ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0) {
                throw std::runtime_error("failed to sign HTTP/3 test identity");
            }
            certificateFile_ = directory_ / "cert.pem";
            privateKeyFile_ = directory_ / "key.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> certBio(
                BIO_new_file(certificateFile_.string().c_str(), "w"), BIO_free);
            std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
                BIO_new_file(privateKeyFile_.string().c_str(), "w"), BIO_free);
            if (!certBio || !keyBio || PEM_write_bio_X509(certBio.get(), certificate.get()) != 1 ||
                ruvia::test::write_tls_private_key(keyBio.get(), key.get()) != 1) {
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

[[nodiscard]] inline ruvia::quic_stream_write_result write_quic_stream(
    ruvia::quic_connection& connection, std::uint64_t stream_id,
    std::span<const char> bytes, bool fin = false) {
    return connection.write_stream(stream_id, std::as_bytes(bytes), fin);
}

[[nodiscard]] inline ruvia::quic_stream_read_result read_quic_stream(
    ruvia::quic_connection& connection, std::uint64_t stream_id,
    std::span<char> bytes) {
    return connection.read_stream(stream_id, std::as_writable_bytes(bytes));
}

[[nodiscard]] inline bool is_quic_stream_closed(ruvia::quic_operation_status status) noexcept {
    return status == ruvia::quic_operation_status::stream_closed ||
           status == ruvia::quic_operation_status::closing ||
           status == ruvia::quic_operation_status::draining ||
           status == ruvia::quic_operation_status::retired;
}

[[nodiscard]] inline std::vector<char> test_http3_frame(
    std::uint64_t type, std::span<const char> payload) {
    std::array<char, 16> header{};
    const auto typeSize = ruvia::encodeHttp3VarInt(header, type);
    if ((typeSize.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 test frame type");
    }
    const auto payloadSize = ruvia::encodeHttp3VarInt(
        std::span<char>(header).subspan(std::get<0>(typeSize)), payload.size());
    if ((payloadSize.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 test frame length");
    }
    std::vector<char> output(header.begin(), header.begin() + std::get<0>(typeSize) + std::get<0>(payloadSize));
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
        if ((encodedHead.index() != 0)) {
            throw std::runtime_error("failed to encode HTTP/3 test response head");
        }
        first_part_ = test_http3_frame(1, std::get<0>(encodedHead));
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
            if ((wsHead.index() != 0)) {
                throw std::runtime_error("failed to encode WebSocket test response");
            }
            first_part_ = test_http3_frame(1, std::get<0>(wsHead));
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
            if ((head.index() != 0)) {
                throw std::runtime_error("CONNECT test head encoding failed");
            }
            first_part_ = test_http3_frame(1, std::get<0>(head));
            const std::string greeting(100003, 's');
            const auto data = test_http3_frame(0, std::span(greeting.data(), greeting.size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            final_part_ = test_http3_frame(0, std::span<const char>("ended", 5));
        }
        if (udp_) {
            const ruvia::Http3FieldSectionFieldView udpFields[]{{":status", "200"}, {"capsule-protocol", "?1"}};
            const auto head = ruvia::encodeHttp3FieldSection(udpFields, &temporary);
            if ((head.index() != 0)) {
                throw std::runtime_error("UDP test head encoding failed");
            }
            first_part_ = test_http3_frame(1, std::get<0>(head));
            std::string payload(1, '\0');
            payload.append(16003, 's');
            std::array<char, 16> header;
            const auto encoded = ruvia::encodeHttpCapsuleHeader(header, 0, payload.size());
            std::string capsule(header.data(), std::get<0>(encoded));
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
            if ((prefixes.index() != 0)) {
                throw std::runtime_error("failed to create local HTTP/3 critical stream prefixes");
            }
            const auto controlPrefix = std::get<0>(prefixes).controlPrefix();
            std::string serverControl(controlPrefix.data(), controlPrefix.size());
            if (advertiseOrigins_) {
                const std::array<std::string_view, 2> origins{"https://127.0.0.1", "https://localhost"};
                const auto frame = ruvia::encodeHttp3OriginFrame(origins);
                if ((frame.index() != 0)) {
                    throw std::runtime_error("failed to encode HTTP/3 ORIGIN fixture");
                }
                serverControl.append(std::get<0>(frame).data(), std::get<0>(frame).size());
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
                std::span<const char>(serverControl), std::get<0>(prefixes).qpackEncoderPrefix(), std::get<0>(prefixes).qpackDecoderPrefix()};
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
                            if (capsule.type != 0 || (datagram.index() != 0) || std::get<0>(datagram).contextId != 0 ||
                                !std::ranges::all_of(std::get<0>(datagram).payload, [](char ch) { return ch == 't'; })) {
                                throw std::runtime_error("invalid CONNECT-UDP client packet");
                            }
                            target.tunnel_bytes->fetch_add(std::get<0>(datagram).payload.size(), std::memory_order_release);
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
                            if ((head.index() != 0)) {
                                throw std::runtime_error("failed to encode dynamic WebSocket response");
                            }
                            const auto oldHeader = ruvia::decodeHttp3FrameHeader(first_part_);
                            if ((oldHeader.index() != 0)) {
                                throw std::runtime_error("invalid static WebSocket response fixture");
                            }
                            const auto prefix = test_http3_frame(1, std::get<0>(head).field_section.fieldSection);
                            first_part_.erase(first_part_.begin(), first_part_.begin() + static_cast<std::ptrdiff_t>(std::get<0>(oldHeader).encodedBytes + std::get<0>(oldHeader).length));
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
                                if ((promise.index() != 0)) {
                                    throw std::runtime_error("push fixture promise failed");
                                }
                                if (push_->cancel_before_promise) {
                                    auto cancel = wsRequest.prepareCancelPush(pushId);
                                    // The critical initial prefix has already completed; use the stable critical output lane below.
                                    qpackBytes[1].assign(std::get<0>(cancel).data(), std::get<0>(cancel).size());
                                    qpackOffsets[1] = 0;
                                }
                                if (push_->promise_only || push_->cancel_before_promise) {
                                    promises.emplace_back(std::get<0>(promise).begin(), std::get<0>(promise).end());
                                } else {
                                    auto prefix = wsRequest.preparePushStream(*openedPush, pushId);
                                    if ((prefix.index() != 0)) {
                                        throw std::runtime_error("push fixture stream prefix failed");
                                    }
                                    PushWire wire{*openedPush, std::vector<char>(std::get<0>(prefix).begin(), std::get<0>(prefix).end())};
                                    const std::string length = std::to_string(push_->body_bytes + (push_->malformed_response ? 1 : 0));
                                    const std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{":status", "200"}, {"content-length", length}}};
                                    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource_);
                                    auto head = test_http3_frame(1, std::get<0>(encoded));
                                    wire.bytes.insert(wire.bytes.end(), head.begin(), head.end());
                                    const std::string content(push_->body_bytes, 'p');
                                    auto body = test_http3_frame(0, content);
                                    wire.bytes.insert(wire.bytes.end(), body.begin(), body.end());
                                    if (push_->stream_before_promise) {
                                        wire.promise.assign(std::get<0>(promise).begin(), std::get<0>(promise).end());
                                        wire.promise_release = std::chrono::steady_clock::now() + 30ms;
                                    } else {
                                        promises.emplace_back(std::get<0>(promise).begin(), std::get<0>(promise).end());
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
inline ruvia::Task<bool> wait_for_peer(const ruvia::WorkerHandle& worker, local_quic_response_peer& peer,
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
        timer_.cancel();
    }

    [[nodiscard]] bool expired() const noexcept {
        return state_->expired;
    }

private:
    asio::steady_timer timer_;
    std::shared_ptr<State> state_;
};

inline ruvia::Task<void> exercisePublicHttp3ClientPool(asio::io_context& io,
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
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] {
                        retainedAdvertisement = pool.nextAdvertisement();
                        return retainedAdvertisement && retainedAdvertisement->origins(); }, 2s));
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

}  // namespace http3_client_connection_test

using namespace http3_client_connection_test;  // NOLINT(google-build-using-namespace)
