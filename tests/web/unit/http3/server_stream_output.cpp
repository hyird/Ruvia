#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/http/http3_buffered_response_cursor.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/Http3ServerStreamOutput.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

#include "http3_quic_udp_pair.h"
#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
namespace {

using namespace ruvia::detail;
using Output = Http3ServerStreamOutput;
using Mailbox = http3_stream_buffer;
using BorrowedBlock = http3_stream_buffer::borrowed_block;

inline constexpr std::uint64_t kEpoch = 17;
inline constexpr std::uint64_t kGeneration = 29;

class IdentityFiles final {
public:
    IdentityFiles() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-http3-output-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create temporary TLS directory");
        }

        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* rawKey = nullptr;
        if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
            EVP_PKEY_keygen(context.get(), &rawKey) <= 0) {
            throw std::runtime_error("failed to generate TLS key");
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
            throw std::runtime_error("failed to create self-signed test certificate");
        }

        certificateFile_ = directory_ / "cert.pem";
        privateKeyFile_ = directory_ / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certificateBio(
            BIO_new_file(certificateFile_.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
            BIO_new_file(privateKeyFile_.string().c_str(), "w"), BIO_free);
        if (!certificateBio || !keyBio ||
            PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1 ||
            PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            throw std::runtime_error("failed to write test certificate");
        }
    }

    ~IdentityFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] const std::filesystem::path& certificateFile() const noexcept {
        return certificateFile_;
    }
    [[nodiscard]] const std::filesystem::path& privateKeyFile() const noexcept {
        return privateKeyFile_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificateFile_;
    std::filesystem::path privateKeyFile_;
};

HttpServerListenerDefinition::Tls serverTlsConfig(const IdentityFiles& files) {
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificateFile().string();
    config.identity.privateKeyFile = files.privateKeyFile().string();
    return config;
}

class QuicPair final {
public:
    using StreamId = std::uint64_t;
    using ReadStatus = ruvia::quic_stream_read_status;
    using ReadResult = ruvia::quic_stream_read_result;

    QuicPair()
        : files_(),
          pair_(std::make_unique<ruvia::testing::http3_quic_udp_pair>(
              serverTlsConfig(files_))) {}

    QuicPair(const QuicPair&) = delete;
    QuicPair& operator=(const QuicPair&) = delete;

    void connect() {
        if (!pair_->run_until([this] { return pair_->connected(); })) {
            throw std::runtime_error("QUIC test handshake deadline");
        }
    }

    [[nodiscard]] StreamId openRequestStream(bool finishRequest = true,
        std::span<const char> requestBytes = {}) {
        request_streams_requested_ = true;
        const auto opened = pair_->client().open_stream(false);
        if (opened.status != ruvia::quic_operation_status::accepted) {
            throw std::runtime_error("failed to open client request stream");
        }
        constexpr std::array<char, 1> defaultTrigger{'x'};
        const auto trigger = requestBytes.empty() ? std::span<const char>(defaultTrigger) : requestBytes;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool written = false;
        while (!written && std::chrono::steady_clock::now() < deadline) {
            const auto result = pair_->client().write_stream(opened.stream_id, std::as_bytes(std::span(trigger)));
            if (result.status == ruvia::quic_operation_status::accepted &&
                result.accepted == trigger.size()) {
                written = true;
            } else if (result.status != ruvia::quic_operation_status::would_block &&
                       result.status != ruvia::quic_operation_status::need_input) {
                throw std::runtime_error("failed to write client request stream trigger");
            }
            if (!written) {
                pair_->pump();
            }
        }
        if (!written || (finishRequest &&
                            pair_->client().finish_stream(opened.stream_id) !=
                                ruvia::quic_operation_status::accepted)) {
            throw std::runtime_error("failed to finish client request stream");
        }
        while (!hasAcceptedStream(opened.stream_id) && std::chrono::steady_clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!hasAcceptedStream(opened.stream_id)) {
            throw std::runtime_error("server did not accept client bidi stream");
        }
        if (finishRequest) {
            const auto received = readServerToTerminal(opened.stream_id);
            if (received.status != ReadStatus::fin ||
                received.bytes != std::string_view(trigger.data(), trigger.size())) {
                throw std::runtime_error("server did not drain the finished request stream");
            }
        }
        return opened.stream_id;
    }

    struct ServerReceive final {
        std::string bytes;
        ReadStatus status{ReadStatus::would_block};
        std::optional<std::uint64_t> peer_reset_error_code{};
    };

    [[nodiscard]] ServerReceive readServerToTerminal(StreamId stream_id) {
        ServerReceive received;
        std::array<char, 256> buffer{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto read = pair_->server().read_stream(stream_id, std::as_writable_bytes(std::span(buffer)));
            if (read.status == ReadStatus::data) {
                received.bytes.append(buffer.data(), read.size);
            } else if (read.status == ReadStatus::fin || read.status == ReadStatus::reset) {
                received.status = read.status;
                received.peer_reset_error_code = read.peer_reset_error_code;
                return received;
            } else if (read.status == ReadStatus::would_block) {
                pair_->pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                throw std::runtime_error("server request stream read failed");
            }
        }
        throw std::runtime_error("server request stream terminal watchdog");
    }

    void writeClientStream(StreamId stream_id, std::span<const char> bytes) {
        std::size_t offset{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (offset != bytes.size() && std::chrono::steady_clock::now() < deadline) {
            const auto write = pair_->client().write_stream(
                stream_id, std::as_bytes(bytes.subspan(offset)));
            if (write.status == ruvia::quic_operation_status::accepted &&
                write.accepted != 0 && write.accepted <= bytes.size() - offset) {
                offset += write.accepted;
            } else if (write.status == ruvia::quic_operation_status::would_block ||
                       write.status == ruvia::quic_operation_status::need_input) {
                pair_->pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                throw std::runtime_error("client request tail write failed");
            }
        }
        if (offset != bytes.size()) {
            throw std::runtime_error("client request tail write watchdog");
        }
    }

    void finishClientStream(StreamId stream_id) {
        if (pair_->client().finish_stream(stream_id) != ruvia::quic_operation_status::accepted) {
            throw std::runtime_error("client request tail FIN failed");
        }
    }

    [[nodiscard]] ruvia::quic_connection& server() {
        return pair_->server();
    }
    [[nodiscard]] ruvia::quic_connection& client() noexcept {
        return pair_->client();
    }
    [[nodiscard]] const ruvia::quic_stream_metadata* acceptedStream(StreamId stream_id) const noexcept {
        for (std::size_t i = 0; i < accepted_count_; ++i) {
            if (accepted_[i].stream_id == stream_id) {
                return &accepted_[i];
            }
        }
        return nullptr;
    }

    void pump() {
        pair_->pump();
        if (!request_streams_requested_) {
            return;
        }
        const auto accepted = pair_->server().accept_streams();
        if (accepted.status != ruvia::quic_operation_status::accepted &&
            accepted.status != ruvia::quic_operation_status::need_input &&
            accepted.status != ruvia::quic_operation_status::would_block) {
            throw std::runtime_error("QUIC test server stream acceptance failed");
        }
        for (std::size_t i = 0; i < accepted.size; ++i) {
            if (accepted_count_ >= accepted_.size()) {
                throw std::runtime_error("QUIC test accepted-stream fixture is full");
            }
            accepted_[accepted_count_++] = accepted.streams[i];
        }
    }

private:
    [[nodiscard]] bool hasAcceptedStream(StreamId stream_id) const noexcept {
        return acceptedStream(stream_id) != nullptr;
    }

    IdentityFiles files_;
    std::unique_ptr<ruvia::testing::http3_quic_udp_pair> pair_;
    std::array<ruvia::quic_stream_metadata, 32> accepted_{};
    std::size_t accepted_count_{};
    bool request_streams_requested_{};
};

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t deallocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

[[nodiscard]] http3_stream_id messageId(std::uint64_t streamId,
    std::uint64_t epoch = kEpoch, std::uint64_t generation = kGeneration) noexcept {
    return {.epoch = epoch, .connection_generation = generation, .stream_id = streamId};
}

[[nodiscard]] BorrowedBlock enqueueBlock(Mailbox& mailbox, http3_stream_id id,
    std::span<const char> bytes) {
    const auto input = std::as_bytes(bytes);
    const auto sent = mailbox.try_send(id, input);
    if (sent != Mailbox::send_result::sent) {
        throw std::runtime_error("failed to enqueue HTTP/3 test block");
    }
    BorrowedBlock block;
    if (!mailbox.try_receive(block)) {
        throw std::runtime_error("failed to receive HTTP/3 test block");
    }
    return block;
}

[[nodiscard]] std::size_t available_blocks(Mailbox& buffer) {
    std::vector<BorrowedBlock> retained;
    retained.reserve(buffer.block_capacity());
    const std::array<std::byte, 1> bytes{std::byte{0}};
    while (true) {
        const auto result = buffer.try_send(messageId(0), bytes);
        if (result == Mailbox::send_result::no_block) {
            return retained.size();
        }
        if (result != Mailbox::send_result::sent) {
            throw std::runtime_error("buffer credit probe could not publish");
        }
        BorrowedBlock block;
        if (!buffer.try_receive(block)) {
            throw std::runtime_error("buffer credit probe could not consume");
        }
        retained.push_back(std::move(block));
    }
}

[[nodiscard]] std::pmr::vector<char> encodeResponse(std::string_view body,
    std::pmr::memory_resource* resource) {
    ruvia::HttpResponse response;
    response.body(body);
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    auto created = ruvia::http3_buffered_response_cursor::create(response, plan, resource);
    if (!created) {
        throw std::runtime_error("failed to create HTTP/3 buffered response write cursor");
    }
    auto cursor = std::move(*created);
    std::pmr::vector<char> bytes(resource);
    for (std::size_t iteration = 0; iteration < 16; ++iteration) {
        const auto segment = cursor.next();
        if (!segment) {
            throw std::runtime_error("failed to read HTTP/3 response segment");
        }
        if (segment->empty()) {
            break;
        }
        bytes.insert(bytes.end(), segment->begin(), segment->end());
        if (!cursor.acknowledge(segment->size())) {
            throw std::runtime_error("failed to acknowledge HTTP/3 response segment");
        }
    }
    if (!cursor.fin_ready() || !cursor.acknowledge_fin(true) || !cursor.finished()) {
        throw std::runtime_error("HTTP/3 response cursor did not finish");
    }
    return bytes;
}

struct DecodedResponse final {
    std::uint16_t status{};
    std::string body;
    std::size_t finalHeads{};
    std::size_t messageEnds{};
};

void onDecodedResponse(void* context, const ruvia::Http3ClientResponseEvent& event) {
    auto& response = *static_cast<DecodedResponse*>(context);
    if (event.kind == ruvia::Http3ClientResponseEventKind::kFinalHead) {
        response.status = event.head->status;
        ++response.finalHeads;
    } else if (event.kind == ruvia::Http3ClientResponseEventKind::kBody) {
        response.body.append(event.body.data(), event.body.size());
    } else if (event.kind == ruvia::Http3ClientResponseEventKind::kMessageEnd) {
        ++response.messageEnds;
    }
}

struct ReceivedWire final {
    std::string bytes;
    bool fin{};
    std::optional<std::uint64_t> peer_reset_error_code;
};

void readAvailable(QuicPair& pair, std::uint64_t streamId, ReceivedWire& received) {
    std::array<char, 2048> buffer{};
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto read = pair.client().read_stream(streamId, std::as_writable_bytes(std::span(buffer)));
        if (read.status == ruvia::quic_stream_read_status::data) {
            received.bytes.append(buffer.data(), read.size);
        } else if (read.status == ruvia::quic_stream_read_status::fin) {
            received.fin = true;
            return;
        } else if (read.status == ruvia::quic_stream_read_status::reset) {
            received.peer_reset_error_code = read.peer_reset_error_code;
            return;
        } else if (read.status == ruvia::quic_stream_read_status::would_block) {
            return;
        } else {
            throw std::runtime_error("QUIC test response stream read failed");
        }
    }
}

[[nodiscard]] DecodedResponse decodeResponse(std::uint64_t streamId,
    const ReceivedWire& wire, std::pmr::memory_resource* resource) {
    ruvia::Http3ClientResponse decoder(streamId, ruvia::HttpKnownMethod::kGet, resource);
    DecodedResponse response;
    const auto result = decoder.feed(wire.bytes, wire.fin, false, onDecodedResponse, &response);
    if (result.status != ruvia::Http3ClientResponseStatus::kMessageEnd) {
        throw std::runtime_error("QUIC client did not decode a complete HTTP/3 response");
    }
    return response;
}

[[nodiscard]] char patternByte(std::uint64_t offset) noexcept {
    return static_cast<char>((offset * 131U + (offset >> 8U) + 17U) & 0xffU);
}

void fillPattern(std::span<char> output, std::uint64_t offset) noexcept {
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = patternByte(offset + i);
    }
}

[[nodiscard]] bool matchesPattern(std::string_view bytes, std::uint64_t offset) noexcept {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != patternByte(offset + i)) {
            return false;
        }
    }
    return true;
}

struct PatternSupply final {
    std::uint64_t bytes{};
    const char* lastAddress{};
    std::size_t lastSize{};
};

[[nodiscard]] bool offerPatternBlock(Output& output, Mailbox& mailbox, std::uint64_t streamId,
    PatternSupply& supply, std::uint64_t limit) {
    if (supply.bytes >= limit || output.queuedBlockCount() >= 2) {
        return false;
    }
    const auto info = output.streamInfo(streamId);
    if (info && info->queuedBlocks != 0) {
        return false;
    }
    std::array<char, Mailbox::max_block_bytes> bytes{};
    const auto size = static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes.size(), limit - supply.bytes));
    fillPattern(std::span<char>(bytes.data(), size), supply.bytes);
    auto block = enqueueBlock(mailbox, messageId(streamId), std::span<const char>(bytes.data(), size));
    supply.lastAddress = reinterpret_cast<const char*>(block.bytes().data());
    supply.lastSize = block.bytes().size();
    const auto accepted = output.acceptData(block);
    if (accepted.status != Output::Status::kAccepted) {
        return false;
    }
    supply.bytes += size;
    return true;
}

}  // namespace
#endif

RUVIA_TEST(http3ServerStreamOutputWritesFairlyAndClientDecodesResponse) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto firstId = pair.openRequestStream();
    const auto secondId = pair.openRequestStream();
    const auto firstWire = encodeResponse("first response", std::pmr::get_default_resource());
    const auto secondWire = encodeResponse("second response", std::pmr::get_default_resource());

    ruvia::WorkerMemory worker;
    Mailbox mailbox(8, 8, 8);
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 8, .maxQueuedBlocks = 8, .maxDriveWorkItems = 1});

    auto foreign = enqueueBlock(mailbox, messageId(firstId, kEpoch + 1), firstWire);
    RUVIA_CHECK(output.acceptData(foreign).status == Output::Status::kForeignEpoch);
    RUVIA_CHECK(foreign);
    foreign.release();
    RUVIA_CHECK_EQ(output.trackedStreamCount(), std::size_t{0});
    RUVIA_CHECK(pair.server().info().state != ruvia::quic_connection_state::retired);

    auto firstBlock = enqueueBlock(mailbox, messageId(firstId), firstWire);
    auto secondBlock = enqueueBlock(mailbox, messageId(secondId), secondWire);
    const http3_stream_control firstFin{.kind = http3_stream_control::kind::stream_fin,
        .id = messageId(firstId),
        .value = firstWire.size()};
    RUVIA_CHECK(output.acceptControl(firstFin).status == Output::Status::kFinDeferred);
    RUVIA_CHECK(output.acceptControl(firstFin).status == Output::Status::kDuplicateFin);
    RUVIA_CHECK(output.acceptData(firstBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!firstBlock);
    RUVIA_CHECK(output.acceptData(secondBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!secondBlock);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(secondId),
                                         .value = secondWire.size()})
                    .status == Output::Status::kFinDeferred);

    ReceivedWire firstReceived;
    ReceivedWire secondReceived;
    std::array<bool, 2> attempted{};
    auto firstTurn = output.drive();
    RUVIA_CHECK_EQ(firstTurn.writeCalls, std::size_t{1});
    RUVIA_CHECK(firstTurn.needsReschedule);
    if (firstTurn.writeCalls != 0) {
        attempted[firstTurn.lastStreamId == firstId ? 0 : 1] = true;
    }
    pair.pump();
    output.notifyTransportActivity();
    readAvailable(pair, firstId, firstReceived);
    readAvailable(pair, secondId, secondReceived);
    for (std::size_t turnIndex = 0; turnIndex < 128 && (!attempted[0] || !attempted[1]); ++turnIndex) {
        const auto turn = output.drive();
        if (turn.writeCalls != 0) {
            attempted[turn.lastStreamId == firstId ? 0 : 1] = true;
        }
        pair.pump();
        output.notifyTransportActivity();
        readAvailable(pair, firstId, firstReceived);
        readAvailable(pair, secondId, secondReceived);
    }
    RUVIA_CHECK(attempted[0] && attempted[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline &&
           (!firstReceived.fin || !secondReceived.fin)) {
        (void)output.drive();
        pair.pump();
        readAvailable(pair, firstId, firstReceived);
        readAvailable(pair, secondId, secondReceived);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(firstReceived.fin && secondReceived.fin);
    output.notifyTransportActivity();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
    }
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    const auto firstInfo = output.streamInfo(firstId);
    const auto secondInfo = output.streamInfo(secondId);
    RUVIA_CHECK(firstInfo && firstInfo->state == Output::StreamState::kFinished);
    RUVIA_CHECK(secondInfo && secondInfo->state == Output::StreamState::kFinished);
    RUVIA_CHECK(firstInfo && firstInfo->sendFinAccepted);
    RUVIA_CHECK(secondInfo && secondInfo->sendFinAccepted);
    RUVIA_CHECK(firstInfo && firstInfo->acceptedWireBytes == firstWire.size());
    RUVIA_CHECK(secondInfo && secondInfo->acceptedWireBytes == secondWire.size());

    const auto decodedFirst = decodeResponse(firstId, firstReceived, worker.resource());
    const auto decodedSecond = decodeResponse(secondId, secondReceived, worker.resource());
    RUVIA_CHECK_EQ(decodedFirst.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(decodedFirst.body, std::string("first response"));
    RUVIA_CHECK_EQ(decodedFirst.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(decodedFirst.messageEnds, std::size_t{1});
    RUVIA_CHECK_EQ(decodedSecond.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(decodedSecond.body, std::string("second response"));
    RUVIA_CHECK_EQ(decodedSecond.finalHeads, std::size_t{1});
    RUVIA_CHECK_EQ(decodedSecond.messageEnds, std::size_t{1});

    const auto conflict = output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
        .id = messageId(firstId),
        .value = firstWire.size() + 1});
    RUVIA_CHECK(conflict.status == Output::Status::kFinalSizeError);
    RUVIA_CHECK(output.connectionRetired());
    RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3ServerStreamOutputIdlePumpDoesNotRequestContinuation) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 8, .maxDriveWorkItems = 2, .writeTimeout = std::chrono::milliseconds(5)});
    for (unsigned turn = 0; turn < 100; ++turn) {
        const auto result = output.drive();
        RUVIA_CHECK_EQ(result.operations, std::size_t{0});
        RUVIA_CHECK_EQ(result.scannedSlots, std::size_t{0});
        RUVIA_CHECK(!result.needsReschedule);
    }
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3ServerStreamOutputDoesNotTimeoutACompletedTombstone) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream();
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.writeTimeout = std::chrono::milliseconds(5)});
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kAccepted);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
        pair.pump();
        output.notifyTransportActivity();
        const auto info = output.streamInfo(streamId);
        if (info && info->state == Output::StreamState::kFinished) {
            break;
        }
    }
    const auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinished);
    RUVIA_CHECK_EQ(output.trackedStreamCount(), std::size_t{1});
    RUVIA_CHECK_EQ(output.liveStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(output.pendingStreamCount(), std::size_t{0});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto afterTimeout = output.drive();
    RUVIA_CHECK_EQ(afterTimeout.timedOutStreams, std::size_t{0});
    RUVIA_CHECK(!output.connectionRetired());
    RUVIA_CHECK(pair.server().info().state != ruvia::quic_connection_state::retired);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kDuplicateFin);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3ServerStreamOutputDoesNotTimeoutADeferredFinWithoutPendingBytes) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream(false);
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.writeTimeout = std::chrono::milliseconds(5)});
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = 1})
                    .status == Output::Status::kFinDeferred);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto idle = output.drive();
    RUVIA_CHECK_EQ(idle.timedOutStreams, std::size_t{0});
    RUVIA_CHECK(!output.connectionRetired());
    RUVIA_CHECK_EQ(output.pendingStreamCount(), std::size_t{0});
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3ServerStreamOutputKeepsReceiveHalfOpenAfterResponseFin) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream(false);
    ruvia::WorkerMemory worker;
    Mailbox mailbox(4, 4, 4);
    Output output(pair.server(), worker, kEpoch, kGeneration);
    const auto responseWire = encodeResponse("response before request end", worker.resource());
    auto block = enqueueBlock(mailbox, messageId(streamId), responseWire);
    RUVIA_CHECK(output.acceptData(block).status == Output::Status::kAccepted);
    RUVIA_CHECK(!block);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = responseWire.size()})
                    .status == Output::Status::kFinDeferred);

    ReceivedWire response;
    const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!response.fin && std::chrono::steady_clock::now() < responseDeadline) {
        (void)output.drive();
        pair.pump();
        output.notifyTransportActivity();
        readAvailable(pair, streamId, response);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(response.fin);
    RUVIA_CHECK_EQ(response.bytes.size(), responseWire.size());
    auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->sendFinAccepted);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinPending);
    RUVIA_CHECK(pair.server().retire_completed_stream(streamId) ==
                ruvia::quic_operation_status::would_block);

    constexpr std::array<char, 9> tail{'-', 't', 'a', 'i', 'l', '-', 'o', 'k', '!'};
    pair.writeClientStream(streamId, tail);
    pair.finishClientStream(streamId);
    const auto request = pair.readServerToTerminal(streamId);
    RUVIA_CHECK(request.status == ruvia::quic_stream_read_status::fin);
    RUVIA_CHECK_EQ(request.bytes, std::string("x-tail-ok!"));
    output.notifyTransportActivity();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
        info = output.streamInfo(streamId);
        if (info && info->state == Output::StreamState::kFinished) {
            break;
        }
    }
    info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinished);
    RUVIA_CHECK_EQ(pair.server().read_health(streamId).status,
        ruvia::quic_stream_read_status::closed);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = responseWire.size()})
                    .status == Output::Status::kDuplicateFin);
    const auto repeated = output.drive();
    RUVIA_CHECK_EQ(repeated.finishedStreams, std::size_t{0});
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3ServerStreamOutputDrainsBufferedRequestBeforeNormalRetirement) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream(false);
    pair.finishClientStream(streamId);
    pair.pump();

    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kAccepted);
    (void)output.drive();
    auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->sendFinAccepted);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinPending);
    RUVIA_CHECK(pair.server().retire_completed_stream(streamId) ==
                ruvia::quic_operation_status::would_block);

    const auto request = pair.readServerToTerminal(streamId);
    RUVIA_CHECK(request.status == ruvia::quic_stream_read_status::fin);
    RUVIA_CHECK_EQ(request.bytes, std::string("x"));
    pair.pump();
    output.notifyTransportActivity();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
        info = output.streamInfo(streamId);
        if (info && info->state == Output::StreamState::kFinished) {
            break;
        }
    }
    info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinished);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3ServerStreamOutputRetiresAfterPeerReset) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream(false);
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration);
    const auto fin = output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
        .id = messageId(streamId),
        .value = 0});
    RUVIA_CHECK(fin.status == Output::Status::kAccepted);
    (void)output.drive();
    auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->sendFinAccepted);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinPending);

    ReceivedWire response;
    const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!response.fin && std::chrono::steady_clock::now() < responseDeadline) {
        pair.pump();
        readAvailable(pair, streamId, response);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(response.fin);
    constexpr auto resetCode = ruvia::Http3ConnectionErrorCode::kMessageError;
    RUVIA_CHECK(pair.client().reset_stream(streamId, static_cast<std::uint64_t>(resetCode)) ==
                ruvia::quic_operation_status::accepted);
    const auto request = pair.readServerToTerminal(streamId);
    RUVIA_CHECK(request.status == ruvia::quic_stream_read_status::reset);
    RUVIA_CHECK(request.peer_reset_error_code == static_cast<std::uint64_t>(resetCode));
    output.notifyTransportActivity();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
        info = output.streamInfo(streamId);
        if (info && info->state == Output::StreamState::kFinished) {
            break;
        }
    }
    info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinished);
    RUVIA_CHECK_EQ(pair.server().read_health(streamId).status,
        ruvia::quic_stream_read_status::closed);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3ServerStreamOutputStopForceClosesAnOpenReceiveHalf) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream(false);
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kAccepted);
    (void)output.drive();
    auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->sendFinAccepted);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinPending);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kCancelled);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    RUVIA_CHECK_EQ(pair.server().read_health(streamId).status,
        ruvia::quic_stream_read_status::closed);
#endif
}

RUVIA_TEST(http3ServerStreamOutputSendsTypedPeerResetCode) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream();
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration);
    constexpr auto resetCode = ruvia::Http3ConnectionErrorCode::kMessageError;

    const auto reset = output.acceptControl({.kind = http3_stream_control::kind::stream_reset,
        .id = messageId(streamId),
        .stream_reset_error_code = resetCode});
    RUVIA_CHECK(reset.status == Output::Status::kReset);
    RUVIA_CHECK(reset.termination.send == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(reset.termination.close == ruvia::quic_operation_status::accepted);

    ruvia::quic_stream_read_result peerReset;
    std::array<char, 16> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline &&
           peerReset.status != ruvia::quic_stream_read_status::reset) {
        pair.pump();
        peerReset = pair.client().read_stream(streamId, std::as_writable_bytes(std::span(buffer)));
        if (peerReset.status == ruvia::quic_stream_read_status::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    RUVIA_CHECK(peerReset.status == ruvia::quic_stream_read_status::reset);
    RUVIA_CHECK(peerReset.peer_reset_error_code.has_value());
    RUVIA_CHECK(peerReset.peer_reset_error_code ==
                static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kReset);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3ServerStreamOutputCancellationTerminatesUnfinishedBidirectionalStream) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream(false);
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration);
    constexpr auto cancelCode = ruvia::Http3ConnectionErrorCode::kRequestCancelled;
    const auto cancelled = output.cancelStream(streamId,
        static_cast<std::uint64_t>(cancelCode));
    RUVIA_CHECK(cancelled.status == Output::Status::kCancelled);
    RUVIA_CHECK(cancelled.termination.send == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(cancelled.termination.close == ruvia::quic_operation_status::accepted);

    bool peerReceivedReset{};
    bool peerSendStopped{};
    std::uint64_t peerResetCode{};
    std::array<char, 16> readBuffer{};
    constexpr std::array<char, 0> emptyWrite{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline &&
           (!peerReceivedReset || !peerSendStopped)) {
        pair.pump();
        if (!peerReceivedReset) {
            const auto read = pair.client().read_stream(streamId, std::as_writable_bytes(std::span(readBuffer)));
            if (read.status == ruvia::quic_stream_read_status::reset) {
                peerReceivedReset = true;
                peerResetCode = read.peer_reset_error_code.value_or(0);
            }
        }
        if (!peerSendStopped) {
            const auto write = pair.client().write_stream(streamId, std::as_bytes(std::span(emptyWrite)));
            peerSendStopped =
                write.status == ruvia::quic_operation_status::stream_closed;
        }
        if (!peerReceivedReset || !peerSendStopped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    RUVIA_CHECK(peerReceivedReset);
    RUVIA_CHECK_EQ(peerResetCode, static_cast<std::uint64_t>(cancelCode));
    RUVIA_CHECK(peerSendStopped);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
#endif
}

RUVIA_TEST(http3_peer_response_cancel_releases_only_its_blocks_and_preserves_sibling_response) {
#if OPENSSL_VERSION_NUMBER >= 0x30600000L
    QuicPair pair;
    pair.connect();
    const auto cancelled_id = pair.openRequestStream();
    const auto sibling_id = pair.openRequestStream();
    std::array<std::byte, 32> request_bytes{};
    const auto request_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (const auto stream_id : {cancelled_id, sibling_id}) {
        bool request_finished{};
        while (!request_finished && std::chrono::steady_clock::now() < request_deadline) {
            pair.pump();
            const auto result = pair.server().read_stream(stream_id, request_bytes);
            request_finished = result.status == ruvia::quic_stream_read_status::fin;
            if (!request_finished && result.status != ruvia::quic_stream_read_status::data &&
                result.status != ruvia::quic_stream_read_status::would_block) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(request_finished);
    }
    Mailbox buffer(4, 2, 2);
    ruvia::WorkerMemory worker;
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 4, .maxQueuedBlocks = 2});
    const std::array<char, 4> cancelled_bytes{'d', 'r', 'o', 'p'};
    const std::array<char, 4> sibling_bytes{'s', 'a', 'f', 'e'};
    auto cancelled_block = enqueueBlock(buffer, messageId(cancelled_id), cancelled_bytes);
    auto sibling_block = enqueueBlock(buffer, messageId(sibling_id), sibling_bytes);
    RUVIA_CHECK(output.acceptData(cancelled_block).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.acceptData(sibling_block).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.acceptControl(
                          {.kind = http3_stream_control::kind::stream_fin,
                              .id = messageId(cancelled_id),
                              .value = cancelled_bytes.size()})
                    .status == Output::Status::kFinDeferred);
    RUVIA_CHECK(output.acceptControl(
                          {.kind = http3_stream_control::kind::stream_fin,
                              .id = messageId(sibling_id),
                              .value = sibling_bytes.size()})
                    .status == Output::Status::kFinDeferred);
    RUVIA_CHECK(pair.client().stop_sending(cancelled_id,
                    static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestCancelled)) ==
                ruvia::quic_operation_status::accepted);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (pair.server().write_health(cancelled_id) == ruvia::quic_operation_status::accepted &&
           std::chrono::steady_clock::now() < deadline) {
        pair.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(pair.server().write_health(cancelled_id) != ruvia::quic_operation_status::accepted);
    bool connection_failure{};
    ReceivedWire received;
    while ((output.streamInfo(cancelled_id)->state != Output::StreamState::kCancelled ||
               !received.fin) &&
           std::chrono::steady_clock::now() < deadline) {
        const auto result = output.drive();
        connection_failure = result.status == Output::Status::kTransportError ||
                             result.status == Output::Status::kUnsafeToRelease ||
                             result.status == Output::Status::kConnectionClosed;
        if (connection_failure) {
            break;
        }
        pair.pump();
        readAvailable(pair, sibling_id, received);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(!connection_failure);
    RUVIA_CHECK(output.streamInfo(cancelled_id)->state == Output::StreamState::kCancelled);
    RUVIA_CHECK(received.bytes == std::string_view(sibling_bytes.data(), sibling_bytes.size()));
    RUVIA_CHECK(received.fin);
    RUVIA_CHECK(output.stop().status != Output::Status::kUnsafeToRelease);
    RUVIA_CHECK(available_blocks(buffer) == buffer.block_capacity());
#endif
}

RUVIA_TEST(http3ServerStreamOutputBackpressureCancelStopAndPmrLifetime) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto firstId = pair.openRequestStream();
    const auto secondId = pair.openRequestStream();
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        Mailbox mailbox(4, 4, 4);
        {
            Output output(pair.server(), worker, kEpoch, kGeneration,
                {.maxTrackedStreams = 4, .maxQueuedBlocks = 1, .maxDriveWorkItems = 2});
            constexpr std::array<char, 4> bytes{'d', 'a', 't', 'a'};
            auto queued = enqueueBlock(mailbox, messageId(firstId), bytes);
            RUVIA_CHECK(output.acceptData(queued).status == Output::Status::kAccepted);
            RUVIA_CHECK(!queued);
            auto backpressured = enqueueBlock(mailbox, messageId(firstId), bytes);
            RUVIA_CHECK(output.acceptData(backpressured).status == Output::Status::kBackpressured);
            RUVIA_CHECK(backpressured);
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});

            const auto cancelled = output.cancelStream(firstId);
            RUVIA_CHECK(cancelled.status == Output::Status::kCancelled);
            RUVIA_CHECK(cancelled.termination.send == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(cancelled.termination.close == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK(!output.streamInfo(firstId)->queuedBlocks);
            RUVIA_CHECK(backpressured);
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount() - 1);
            backpressured.release();
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());

            auto stopBlock = enqueueBlock(mailbox, messageId(secondId), bytes);
            RUVIA_CHECK(output.acceptData(stopBlock).status == Output::Status::kAccepted);
            RUVIA_CHECK(!stopBlock);
            RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
            RUVIA_CHECK(output.stopped());
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
        }
        RUVIA_CHECK(mailbox.stop());
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.deallocations);
#endif
}

RUVIA_TEST(http3ServerStreamOutputTimesOutOnlyTheFlowControlledStream) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto blockedId = pair.openRequestStream();
    const auto siblingId = pair.openRequestStream();
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        Mailbox mailbox(4, 4, 4);
        {
            constexpr auto writeTimeout = std::chrono::milliseconds(100);
            Output output(pair.server(), worker, kEpoch, kGeneration,
                {.maxTrackedStreams = 4,
                    .maxQueuedBlocks = 1,
                    .maxDriveWorkItems = 16,
                    .writeTimeout = writeTimeout});
            constexpr std::uint64_t maxBytes = 64U * 1024U * 1024U;
            constexpr std::size_t maxSteps = 8192;
            const auto blockedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            PatternSupply supply;
            bool wouldBlock{};
            for (std::size_t step = 0; step < maxSteps &&
                                       std::chrono::steady_clock::now() < blockedDeadline && !wouldBlock &&
                                       supply.bytes < maxBytes;
                ++step) {
                if (output.queuedBlockCount() == 0) {
                    (void)offerPatternBlock(output, mailbox, blockedId, supply, maxBytes);
                }
                const auto turn = output.drive();
                wouldBlock = turn.wouldBlockWrites != 0 && turn.lastStreamId == blockedId;
                if (!wouldBlock) {
                    pair.pump();
                    output.notifyTransportActivity();
                }
            }
            RUVIA_CHECK(wouldBlock);
            RUVIA_CHECK(supply.lastAddress != nullptr && supply.lastSize != 0);
            RUVIA_CHECK_EQ(output.liveStreamCount(), std::size_t{1});
            RUVIA_CHECK_EQ(output.pendingStreamCount(), std::size_t{1});
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());

            // The WANT retry still borrows the exact queued block; no replacement or
            // return is allowed before stream cancellation retires its SSL wrapper.
            const auto borrowedAddress = supply.lastAddress;
            const auto borrowedSize = supply.lastSize;
            auto settled = output.drive();
            for (std::size_t retry = 0; settled.needsReschedule && retry < 32; ++retry) {
                settled = output.drive();
            }
            const auto beforeRetry = output.streamInfo(blockedId);
            RUVIA_CHECK(beforeRetry.has_value());
            const auto acceptedBeforeRetry = beforeRetry ? beforeRetry->acceptedWireBytes : 0;
            output.notifyTransportActivity();
            const auto retry = output.drive();
            RUVIA_CHECK_EQ(retry.wouldBlockWrites, std::size_t{1});
            RUVIA_CHECK_EQ(supply.lastAddress, borrowedAddress);
            RUVIA_CHECK_EQ(supply.lastSize, borrowedSize);
            const auto afterRetry = output.streamInfo(blockedId);
            RUVIA_CHECK(afterRetry.has_value());
            RUVIA_CHECK(afterRetry && afterRetry->acceptedWireBytes == acceptedBeforeRetry);
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());

            std::this_thread::sleep_for(writeTimeout + std::chrono::milliseconds(10));
            std::size_t timedOut{};
            for (std::size_t attempt = 0; attempt < 32 && timedOut == 0; ++attempt) {
                timedOut += output.drive().timedOutStreams;
            }
            RUVIA_CHECK_EQ(timedOut, std::size_t{1});
            const auto timedOutInfo = output.streamInfo(blockedId);
            RUVIA_CHECK(timedOutInfo && timedOutInfo->state == Output::StreamState::kCancelled);
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
            RUVIA_CHECK(!output.connectionRetired());
            RUVIA_CHECK(pair.server().info().state != ruvia::quic_connection_state::retired);

            ReceivedWire timedOutPeer;
            const auto resetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!timedOutPeer.peer_reset_error_code &&
                   std::chrono::steady_clock::now() < resetDeadline) {
                pair.pump();
                readAvailable(pair, blockedId, timedOutPeer);
                if (!timedOutPeer.peer_reset_error_code) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            RUVIA_CHECK(timedOutPeer.peer_reset_error_code.has_value());
            RUVIA_CHECK(timedOutPeer.peer_reset_error_code ==
                        static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestCancelled));

            const auto siblingWire = encodeResponse("sibling survives timeout", worker.resource());
            auto siblingBlock = enqueueBlock(
                mailbox, messageId(siblingId), siblingWire);
            RUVIA_CHECK(output.acceptData(siblingBlock).status == Output::Status::kAccepted);
            RUVIA_CHECK(!siblingBlock);
            RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                                 .id = messageId(siblingId),
                                                 .value = siblingWire.size()})
                            .status == Output::Status::kFinDeferred);

            const auto lateDataBytes = std::as_bytes(std::span<const char>("late", 4));
            const auto lateSent = mailbox.try_send(messageId(blockedId), lateDataBytes);
            RUVIA_CHECK(lateSent == Mailbox::send_result::sent);
            BorrowedBlock lateBlock;
            RUVIA_CHECK(mailbox.try_receive(lateBlock));
            RUVIA_CHECK(output.acceptData(lateBlock).status == Output::Status::kClosedStream);
            RUVIA_CHECK(lateBlock);
            lateBlock.release();
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
            RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                                 .id = messageId(blockedId),
                                                 .value = supply.bytes})
                            .status == Output::Status::kClosedStream);

            ReceivedWire siblingReceived;
            const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < responseDeadline && !siblingReceived.fin) {
                const auto turn = output.drive();
                timedOut += turn.timedOutStreams;
                pair.pump();
                output.notifyTransportActivity();
                readAvailable(pair, siblingId, siblingReceived);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            output.notifyTransportActivity();
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                (void)output.drive();
            }
            const auto blockedInfo = output.streamInfo(blockedId);
            const auto siblingInfo = output.streamInfo(siblingId);
            RUVIA_CHECK_EQ(timedOut, std::size_t{1});
            RUVIA_CHECK(blockedInfo && blockedInfo->state == Output::StreamState::kCancelled);
            RUVIA_CHECK(blockedInfo &&
                        blockedInfo->termination.send == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(siblingInfo && siblingInfo->state == Output::StreamState::kFinished);
            RUVIA_CHECK(siblingReceived.fin);
            RUVIA_CHECK_EQ(siblingReceived.bytes.size(), siblingWire.size());
            RUVIA_CHECK(!output.connectionRetired());
            RUVIA_CHECK(pair.server().info().state != ruvia::quic_connection_state::retired);
            RUVIA_CHECK_EQ(output.trackedStreamCount(), std::size_t{2});
            RUVIA_CHECK_EQ(output.liveStreamCount(), std::size_t{0});
            RUVIA_CHECK_EQ(output.pendingStreamCount(), std::size_t{0});
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());

            const auto freshId = pair.openRequestStream();
            std::this_thread::sleep_for(writeTimeout + std::chrono::milliseconds(10));
            const auto idleFresh = output.drive();
            RUVIA_CHECK_EQ(idleFresh.timedOutStreams, std::size_t{0});
            // Opening a QUIC stream does not register it with the response writer.
            // An idle, unregistered stream must not inherit the old stream's deadline.
            RUVIA_CHECK(!output.streamInfo(freshId));
            const auto freshWire = encodeResponse("fresh write gets a fresh deadline", worker.resource());
            auto freshBlock = enqueueBlock(mailbox, messageId(freshId), freshWire);
            RUVIA_CHECK(output.acceptData(freshBlock).status == Output::Status::kAccepted);
            RUVIA_CHECK(!freshBlock);
            RUVIA_CHECK(output.streamInfo(freshId)->state == Output::StreamState::kOpen);
            RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                                 .id = messageId(freshId),
                                                 .value = freshWire.size()})
                            .status == Output::Status::kFinDeferred);
            ReceivedWire freshReceived;
            const auto freshDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < freshDeadline && !freshReceived.fin) {
                const auto turn = output.drive();
                timedOut += turn.timedOutStreams;
                pair.pump();
                output.notifyTransportActivity();
                readAvailable(pair, freshId, freshReceived);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            RUVIA_CHECK(freshReceived.fin);
            output.notifyTransportActivity();
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                (void)output.drive();
            }
            RUVIA_CHECK_EQ(freshReceived.bytes.size(), freshWire.size());
            RUVIA_CHECK_EQ(timedOut, std::size_t{1});
            RUVIA_CHECK(output.streamInfo(freshId)->state == Output::StreamState::kFinished);
            RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
        }
        RUVIA_CHECK(mailbox.stop());
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.deallocations);
#endif
}

RUVIA_TEST(http3ServerStreamOutputKeepsSiblingAliveAfterExternalStreamClose) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto closedId = pair.openRequestStream();
    const auto siblingId = pair.openRequestStream();
    ruvia::WorkerMemory worker;
    Mailbox mailbox(4, 4, 4);
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 4, .maxQueuedBlocks = 2, .maxDriveWorkItems = 1});
    constexpr std::array<char, 4> bytes{'d', 'a', 't', 'a'};

    auto closedBlock = enqueueBlock(mailbox, messageId(closedId), bytes);
    RUVIA_CHECK(output.acceptData(closedBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!closedBlock);
    RUVIA_CHECK(pair.server().close_stream(closedId) ==
                ruvia::quic_operation_status::accepted);
    const auto cancelled = output.cancelStream(closedId);
    RUVIA_CHECK(cancelled.status == Output::Status::kCancelled);
    RUVIA_CHECK(cancelled.termination.close == ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(pair.server().info().state != ruvia::quic_connection_state::retired);

    auto siblingBlock = enqueueBlock(mailbox, messageId(siblingId), bytes);
    RUVIA_CHECK(output.acceptData(siblingBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!siblingBlock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline &&
           output.streamInfo(siblingId)->acceptedWireBytes == 0) {
        (void)output.drive();
        pair.pump();
        output.notifyTransportActivity();
    }
    const auto siblingInfo = output.streamInfo(siblingId);
    RUVIA_CHECK(siblingInfo && siblingInfo->acceptedWireBytes == bytes.size());
    RUVIA_CHECK(pair.server().info().state != ruvia::quic_connection_state::retired);
    RUVIA_CHECK(!output.connectionRetired());
    RUVIA_CHECK(output.cancelStream(siblingId).status == Output::Status::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3ServerStreamOutputParksAfterBoundedWouldBlockScansAndCancelsSafely) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto firstId = pair.openRequestStream();
    const auto secondId = pair.openRequestStream();
    ruvia::WorkerMemory worker;
    Mailbox mailbox(2, 2, 4);
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 4, .maxQueuedBlocks = 2, .maxDriveWorkItems = 1});
    constexpr std::uint64_t kMaxSuppliedBytes = 64U * 1024U * 1024U;
    constexpr std::size_t kMaxSteps = 8192;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    PatternSupply firstSupply;
    PatternSupply secondSupply;
    std::size_t steps{};
    bool firstWouldBlock{};
    bool secondWouldBlock{};

    while (steps < kMaxSteps && std::chrono::steady_clock::now() < deadline &&
           !firstWouldBlock && firstSupply.bytes < kMaxSuppliedBytes) {
        if (output.queuedBlockCount() == 0) {
            (void)offerPatternBlock(output, mailbox, firstId, firstSupply, kMaxSuppliedBytes);
        }
        const auto turn = output.drive();
        firstWouldBlock = turn.wouldBlockWrites != 0 && turn.lastStreamId == firstId;
        if (!firstWouldBlock) {
            pair.pump();
            output.notifyTransportActivity();
        }
        ++steps;
    }

    while (steps < kMaxSteps && std::chrono::steady_clock::now() < deadline &&
           firstWouldBlock && !secondWouldBlock &&
           firstSupply.bytes + secondSupply.bytes < kMaxSuppliedBytes) {
        if (output.queuedBlockCount() < 2) {
            (void)offerPatternBlock(output, mailbox, secondId, secondSupply,
                kMaxSuppliedBytes - firstSupply.bytes);
        }
        const auto turn = output.drive();
        secondWouldBlock = turn.wouldBlockWrites != 0 && turn.lastStreamId == secondId;
        if (!secondWouldBlock) {
            pair.pump();
            output.notifyTransportActivity();
        }
        ++steps;
    }

    RUVIA_CHECK(firstWouldBlock);
    RUVIA_CHECK(secondWouldBlock);
    const auto firstInfo = output.streamInfo(firstId);
    const auto secondInfo = output.streamInfo(secondId);
    RUVIA_CHECK(firstInfo && firstInfo->lastWriteStatus == ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(secondInfo && secondInfo->lastWriteStatus == ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(firstSupply.lastAddress != nullptr && firstSupply.lastSize != 0);
    RUVIA_CHECK(secondSupply.lastAddress != nullptr && secondSupply.lastSize != 0);
    RUVIA_CHECK(firstInfo && firstInfo->queuedBlocks == 1);
    RUVIA_CHECK(secondInfo && secondInfo->queuedBlocks == 1);
    RUVIA_CHECK(secondInfo && secondInfo->acceptedWireBytes != 0);

    // Drain the persistent, budget-1 scan to a park point without new transport
    // events. This loop is bounded even when every attempted write is WANT.
    auto settled = output.drive();
    std::size_t settleCalls{};
    while (settled.needsReschedule && settleCalls < 64) {
        settled = output.drive();
        ++settleCalls;
    }
    RUVIA_CHECK(!settled.needsReschedule);
    auto idle = output.drive();
    RUVIA_CHECK_EQ(idle.operations, std::size_t{0});
    RUVIA_CHECK_EQ(idle.writeCalls, std::size_t{0});
    RUVIA_CHECK(!idle.needsReschedule);

    output.notifyTransportActivity();
    const auto firstBlockedTurn = output.drive();
    RUVIA_CHECK_EQ(firstBlockedTurn.operations, std::size_t{1});
    RUVIA_CHECK_EQ(firstBlockedTurn.writeCalls, std::size_t{1});
    RUVIA_CHECK_EQ(firstBlockedTurn.wouldBlockWrites, std::size_t{1});
    RUVIA_CHECK(firstBlockedTurn.status == Output::Status::kIdle);
    RUVIA_CHECK(!firstBlockedTurn.madeProgress);
    RUVIA_CHECK(firstBlockedTurn.needsReschedule);
    const auto unvisitedStream = firstBlockedTurn.lastStreamId == firstId ? secondId : firstId;
    const auto secondBlockedTurn = output.drive();
    RUVIA_CHECK_EQ(secondBlockedTurn.operations, std::size_t{1});
    RUVIA_CHECK_EQ(secondBlockedTurn.writeCalls, std::size_t{1});
    RUVIA_CHECK_EQ(secondBlockedTurn.wouldBlockWrites, std::size_t{1});
    RUVIA_CHECK(!secondBlockedTurn.madeProgress);
    RUVIA_CHECK_EQ(secondBlockedTurn.lastStreamId, unvisitedStream);

    settled = output.drive();
    settleCalls = 0;
    while (settled.needsReschedule && settleCalls < 64) {
        settled = output.drive();
        ++settleCalls;
    }
    RUVIA_CHECK(!settled.needsReschedule);
    output.notifyTransportActivity();
    bool retriedAfterActivity{};
    auto activityTurn = output.drive();
    std::size_t activityCalls{};
    while (activityCalls < 32) {
        retriedAfterActivity = retriedAfterActivity || activityTurn.wouldBlockWrites != 0;
        if (!activityTurn.needsReschedule) {
            break;
        }
        activityTurn = output.drive();
        ++activityCalls;
    }
    RUVIA_CHECK(retriedAfterActivity);
    RUVIA_CHECK(!activityTurn.needsReschedule);
    const auto firstRetriedInfo = output.streamInfo(firstId);
    const auto secondRetriedInfo = output.streamInfo(secondId);
    RUVIA_CHECK(firstRetriedInfo &&
                firstRetriedInfo->lastWriteStatus == ruvia::quic_operation_status::would_block &&
                firstRetriedInfo->state == Output::StreamState::kOpen);
    RUVIA_CHECK(secondRetriedInfo &&
                secondRetriedInfo->lastWriteStatus == ruvia::quic_operation_status::would_block &&
                secondRetriedInfo->state == Output::StreamState::kOpen);
    idle = output.drive();
    RUVIA_CHECK_EQ(idle.operations, std::size_t{0});
    RUVIA_CHECK(!idle.needsReschedule);

    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(output.cancelStream(firstId).status == Output::Status::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(output.cancelStream(secondId).status == Output::Status::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(!output.connectionRetired());
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3ServerStreamOutputResumesWouldBlockAndDeliversExactBytes) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto streamId = pair.openRequestStream();
    ruvia::WorkerMemory worker;
    Mailbox mailbox(2, 2, 4);
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 2, .maxQueuedBlocks = 1, .maxDriveWorkItems = 1});
    constexpr std::uint64_t kMaxSuppliedBytes = 64U * 1024U * 1024U;
    constexpr std::size_t kMaxSteps = 8192;
    const auto blockedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    PatternSupply supply;
    bool sawWouldBlock{};

    // QUIC flow control and packetization determine whether a write is partial;
    // this checks real WANT retries and does not assume any particular partial size.
    for (std::size_t step = 0; step < kMaxSteps &&
                               std::chrono::steady_clock::now() < blockedDeadline && !sawWouldBlock &&
                               supply.bytes < kMaxSuppliedBytes;
        ++step) {
        if (output.queuedBlockCount() == 0) {
            (void)offerPatternBlock(output, mailbox, streamId, supply, kMaxSuppliedBytes);
        }
        const auto turn = output.drive();
        sawWouldBlock = turn.wouldBlockWrites != 0 && turn.lastStreamId == streamId;
        if (!sawWouldBlock) {
            pair.pump();
            output.notifyTransportActivity();
        }
    }
    RUVIA_CHECK(sawWouldBlock);
    RUVIA_CHECK(supply.lastAddress != nullptr && supply.lastSize != 0);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    const auto blockedInfo = output.streamInfo(streamId);
    RUVIA_CHECK(blockedInfo && blockedInfo->lastWriteStatus == ruvia::quic_operation_status::would_block);

    const auto fin = output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
        .id = messageId(streamId),
        .value = supply.bytes});
    RUVIA_CHECK(fin.status == Output::Status::kFinDeferred);

    ReceivedWire received;
    const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool complete{};
    for (std::size_t step = 0; step < kMaxSteps &&
                               std::chrono::steady_clock::now() < recoveryDeadline && !complete;
        ++step) {
        readAvailable(pair, streamId, received);
        pair.pump();
        output.notifyTransportActivity();
        (void)output.drive();
        readAvailable(pair, streamId, received);
        const auto info = output.streamInfo(streamId);
        complete = info && info->state == Output::StreamState::kFinished && received.fin;
    }
    RUVIA_CHECK(complete);
    RUVIA_CHECK_EQ(received.bytes.size(), static_cast<std::size_t>(supply.bytes));
    RUVIA_CHECK(matchesPattern(received.bytes, 0));
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3ServerStreamOutputClosesOnTrackingExhaustionAndRejectsInvalidIds) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    {
        QuicPair pair;
        pair.connect();
        const auto streamId = pair.openRequestStream();
        ruvia::WorkerMemory worker;
        Mailbox mailbox(4, 4, 4);
        Output output(pair.server(), worker, kEpoch, kGeneration,
            {.maxTrackedStreams = 1, .maxQueuedBlocks = 2, .maxDriveWorkItems = 2});
        constexpr std::array<char, 2> bytes{'o', 'k'};
        auto stale = enqueueBlock(mailbox, messageId(streamId, kEpoch, kGeneration + 1), bytes);
        RUVIA_CHECK(output.acceptData(stale).status == Output::Status::kStaleConnection);
        RUVIA_CHECK(stale);
        stale.release();

        auto accepted = enqueueBlock(mailbox, messageId(streamId), bytes);
        RUVIA_CHECK(output.acceptData(accepted).status == Output::Status::kAccepted);
        RUVIA_CHECK(!accepted);
        const auto exhausted = output.acceptControl({.kind = http3_stream_control::kind::writable,
            .id = messageId(streamId + 4)});
        RUVIA_CHECK(exhausted.status == Output::Status::kCapacityExhausted);
        RUVIA_CHECK(output.connectionRetired());
        RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
        RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
        RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
        RUVIA_CHECK(mailbox.stop());
    }

    {
        QuicPair pair;
        pair.connect();
        (void)pair.openRequestStream();
        ruvia::WorkerMemory worker;
        Output output(pair.server(), worker, kEpoch, kGeneration);
        const auto invalid = output.acceptControl({.kind = http3_stream_control::kind::writable,
            .id = messageId(2)});
        RUVIA_CHECK(invalid.status == Output::Status::kInvalidStreamId);
        RUVIA_CHECK(output.connectionRetired());
        RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
    }
#endif
}

RUVIA_TEST(http3ServerStreamOutputPublishesTypedCriticalStreamWithoutFinAndReturnsCredit) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto opened = pair.server().open_stream(true);
    RUVIA_CHECK(opened.status == ruvia::quic_operation_status::accepted);
    ruvia::WorkerMemory worker;
    Mailbox mailbox(1, 1, 1);
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 3, .maxQueuedBlocks = 1, .maxDriveWorkItems = 2});
    constexpr std::array<char, 3> instructions{3, '\x80', 1};
    const auto sent = mailbox.try_send_critical({kEpoch, kGeneration, ruvia::http3_critical_stream_output::stream_kind::qpack_decoder}, std::as_bytes(std::span(instructions)));
    RUVIA_CHECK(sent == Mailbox::send_result::sent);
    BorrowedBlock block;
    RUVIA_CHECK(mailbox.try_receive(block));
    RUVIA_CHECK(block.critical() != nullptr);
    RUVIA_CHECK(output.acceptData(block).status == Output::Status::kInvalidInput);
    RUVIA_CHECK(block);
    RUVIA_CHECK(output.acceptCriticalData(block, opened.stream_id).status == Output::Status::kAccepted);
    RUVIA_CHECK(!block);
    ReceivedWire wire;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool acceptedByPeer = false;
    while (wire.bytes.size() < instructions.size() && std::chrono::steady_clock::now() < deadline) {
        (void)output.drive();
        pair.pump();
        const auto streams = pair.client().accept_streams();
        for (std::size_t i = 0; i < streams.size; ++i) {
            acceptedByPeer |= streams.streams[i].stream_id == opened.stream_id;
        }
        if (acceptedByPeer) {
            readAvailable(pair, opened.stream_id, wire);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK_EQ(wire.bytes, std::string(instructions.data(), instructions.size()));
    RUVIA_CHECK(!wire.fin);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3ServerStreamOutputBindsPushStreamsFinishesAndCancelsWithoutClosingParent) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto parent = pair.openRequestStream();
    const auto first = pair.server().open_stream(true);
    const auto second = pair.server().open_stream(true);
    RUVIA_CHECK(first.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(second.status == ruvia::quic_operation_status::accepted);
    ruvia::WorkerMemory worker;
    Mailbox mailbox(3, 3, 3);
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 3, .maxQueuedBlocks = 3, .maxDriveWorkItems = 4});
    RUVIA_CHECK(output.registerPushStream(parent, 0).status == Output::Status::kInvalidInput);
    RUVIA_CHECK(output.registerPushStream(first.stream_id, 0).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.registerPushStream(second.stream_id, 1).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.registerPushStream(first.stream_id, 2).status == Output::Status::kInvalidInput);
    auto firstId = messageId(first.stream_id);
    firstId.push_id = 0;
    auto secondId = messageId(second.stream_id);
    secondId.push_id = 1;
    constexpr std::array<char, 4> pushWire{1, 0, 'o', 'k'};
    auto firstBlock = enqueueBlock(mailbox, firstId, pushWire);
    auto cancelledBlock = enqueueBlock(mailbox, secondId, pushWire);
    RUVIA_CHECK(output.acceptData(firstBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.acceptData(cancelledBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = firstId,
                                         .value = pushWire.size()})
                    .status == Output::Status::kFinDeferred);
    RUVIA_CHECK(output.cancelStream(second.stream_id).status == Output::Status::kCancelled);
    RUVIA_CHECK(!output.connectionRetired());
    auto parentWire = encodeResponse("parent", worker.resource());
    auto parentBlock = enqueueBlock(mailbox, messageId(parent), parentWire);
    RUVIA_CHECK(output.acceptData(parentBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(output.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                         .id = messageId(parent),
                                         .value = parentWire.size()})
                    .status == Output::Status::kFinDeferred);
    ReceivedWire push;
    ReceivedWire response;
    bool acceptedPush = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while ((!push.fin || !response.fin) && std::chrono::steady_clock::now() < deadline) {
        (void)output.drive();
        pair.pump();
        const auto accepted = pair.client().accept_streams();
        for (std::size_t i = 0; i < accepted.size; ++i) {
            acceptedPush |= accepted.streams[i].stream_id == first.stream_id;
        }
        if (acceptedPush && !push.fin) {
            readAvailable(pair, first.stream_id, push);
        }
        if (!response.fin) {
            readAvailable(pair, parent, response);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(push.fin);
    RUVIA_CHECK(response.fin);
    output.notifyTransportActivity();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
    }
    RUVIA_CHECK_EQ(push.bytes, std::string(pushWire.data(), pushWire.size()));
    RUVIA_CHECK_EQ(response.bytes, std::string(parentWire.data(), parentWire.size()));
    RUVIA_CHECK(output.streamInfo(first.stream_id)->state == Output::StreamState::kFinished);
    RUVIA_CHECK(output.streamInfo(second.stream_id)->state == Output::StreamState::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(mailbox), mailbox.block_capacity() - output.queuedBlockCount());
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    RUVIA_CHECK(mailbox.stop());
#endif
}

RUVIA_TEST(http3QuicClientStreamReadHealthObservesPeerResetBeforeReadingBufferedBytes) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    const auto id = pair.openRequestStream();
    const std::array<char, 4> bytes{'d', 'a', 't', 'a'};
    RUVIA_CHECK(pair.server().write_stream(id, std::as_bytes(std::span(bytes))).status == ruvia::quic_operation_status::accepted);
    const auto code = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    RUVIA_CHECK(pair.server().reset_stream(id, code) == ruvia::quic_operation_status::accepted);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    ruvia::quic_stream_read_result health;
    while (std::chrono::steady_clock::now() < deadline) {
        pair.pump();
        health = pair.client().read_health(id);
        if (health.status == ruvia::quic_stream_read_status::reset) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(health.status == ruvia::quic_stream_read_status::reset);
    RUVIA_CHECK_EQ(health.peer_reset_error_code, std::optional{code});
    std::array<char, 1> output{};
    RUVIA_CHECK(pair.client().read_stream(id, std::as_writable_bytes(std::span(output))).status == ruvia::quic_stream_read_status::reset);
#endif
}

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
namespace {
struct PushRouteState final {
    std::filesystem::path filePath;
    std::string body;
    std::size_t accepted{};
    bool metadataOwned{true};
};
ruvia::Task<ruvia::HttpResponse> producePushRoutes(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<PushRouteState*>(raw);
    if (context.req().path() == "/push") {
        for (auto path : {"/asset", "/stream", "/file"}) {
            std::string authority(context.req().authority());
            std::string target = path;
            std::string value(512, 'v');
            const std::array fields{ruvia::HttpHeaderView("x-pushed", std::string_view(value))};
            auto operation = context.push({.authority = authority, .path = target, .headers = fields});
            authority.assign("changed");
            target.assign("changed");
            value.assign("changed");
            if (co_await std::move(operation)) {
                ++state.accepted;
            }
        }
        co_return context.text("parent");
    }
    state.metadataOwned = state.metadataOwned && context.req().header("x-pushed") == std::string(512, 'v');
    if (context.req().path() == "/file") {
        ruvia::HttpResponse response({.resource = context.arena()});
        response.fileBody(state.filePath, state.body.size(), 0, state.body.size(), ruvia::HttpResponseFileIdentity::unchecked());
        co_return response;
    }
    co_return context.text("asset");
}
ruvia::Task<void> producePushStream(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<PushRouteState*>(raw);
    state.metadataOwned = state.metadataOwned && context.req().header("x-pushed") == std::string(512, 'v');
    co_await context.stream().write(std::string_view(state.body));
    const std::array trailers{ruvia::HttpHeaderView("x-push-end", "done")};
    co_await context.stream().end(trailers);
}

ruvia::Task<void> exerciseQuicPushRoutes(ruvia::EventLoopAttachment& attachment, QuicPair& pair,
    std::uint64_t parentId, std::span<const char> request, std::span<const char> peerControl,
    PushRouteState& state, ruvia::BlockingPool& blockingPool,
    ruvia::test::CountingMemoryResource& upstream, ruvia::testing::TestContext& ruvia_ctx) {
    using Owner = ruvia::detail::Http3ServerConnection;
    const auto& workerHandle = attachment.loop().handle();
    ruvia::WorkerMemory worker(upstream);
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    for (auto path : {"/push", "/asset", "/file"}) {
        implementation.registerRoute(ruvia::HttpKnownMethod::kGet, routing_test::path(path),
            ruvia::detail::RouteHandler(&state, &producePushRoutes), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    }
    implementation.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet, routing_test::path("/stream"),
        ruvia::detail::RouteStreamHandler(&state, &producePushStream), {}, {});
    implementation.finalize();
    ruvia::StopSource stopping;
    const auto stopToken = stopping.token();
    ruvia::detail::ContextServices services(workerHandle, stopToken);
    ruvia::detail::HttpServerOptions options;
    options.blockingPool = &blockingPool;
    ruvia::ConnectionScanner scanner(workerHandle, {.scanInterval = std::chrono::seconds(1)});
    Mailbox inbound(2, 2, 2, worker.resource());
    Mailbox outbound(2, 2, 2, worker.resource());
    unsigned activations{};
    Owner owner(implementation.routeTable(), worker, services, options, outbound,
        {.context = &activations, .activate = [](void* raw, std::uint64_t, std::uint64_t, std::uint64_t, const Owner::WorkerActivation&) noexcept { ++*static_cast<unsigned*>(raw); }, .slotGeneration = 1},
        {.epoch = kEpoch, .connectionGeneration = kGeneration, .maxTrackedStreams = 32, .connectionScanner = &scanner, .executor = attachment.loop().executor()});
    Output output(pair.server(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 4, .maxQueuedBlocks = 2, .maxDriveWorkItems = 4});
    std::vector<std::uint64_t> pushedIds;
    std::array<ReceivedWire, 3> pushed;
    ReceivedWire parent;
    std::exception_ptr failure;
    auto submitInput = [&](http3_stream_id id, std::span<const char> bytes, bool fin) {
        auto block = enqueueBlock(inbound, id, bytes);
        RUVIA_CHECK(owner.acceptData(block).status == Owner::EventStatus::kAccepted);
        block.release();
        if (fin) {
            RUVIA_CHECK(owner.acceptControl({.kind = http3_stream_control::kind::stream_fin,
                                                .id = id,
                                                .value = bytes.size()})
                            .status == Owner::EventStatus::kDispatched);
        }
    };
    try {
        submitInput(messageId(2), peerControl, false);
        submitInput(messageId(parentId), request, true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        std::vector<std::uint64_t> acceptedIds;
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto intent = owner.peekTransportIntent()) {
                if (intent->token.kind == Owner::TransportIntentKind::kOpenPushStream) {
                    const auto opened = pair.server().open_stream(true);
                    RUVIA_CHECK(opened.status == ruvia::quic_operation_status::accepted);
                    RUVIA_CHECK(output.registerPushStream(opened.stream_id, *intent->token.id.push_id).status == Output::Status::kAccepted);
                    pushedIds.push_back(opened.stream_id);
                    RUVIA_CHECK(owner.ackTransportIntent(intent->token, Owner::PushStreamOpenResult{.status = Owner::PushStreamOpenResult::Status::kOpened, .streamId = opened.stream_id}));
                } else {
                    RUVIA_CHECK(owner.ackTransportIntent(intent->token));
                }
            }
            (void)owner.publishOne({.data = true, .control = true, .local = true});
            http3_stream_control control;
            while (outbound.try_receive_control(control)) {
                const auto accepted = output.acceptControl(control);
                RUVIA_CHECK(accepted.status == Output::Status::kAccepted || accepted.status == Output::Status::kFinDeferred);
            }
            Mailbox::borrowed_block block;
            while (outbound.try_receive(block)) {
                RUVIA_CHECK(output.acceptData(block).status == Output::Status::kAccepted);
            }
            (void)output.drive();
            pair.pump();
            const auto streams = pair.client().accept_streams();
            for (std::size_t i = 0; i < streams.size; ++i) {
                acceptedIds.push_back(streams.streams[i].stream_id);
            }
            if (!parent.fin) {
                readAvailable(pair, parentId, parent);
            }
            for (std::size_t i = 0; i < pushedIds.size(); ++i) {
                if (!pushed[i].fin && std::ranges::find(acceptedIds, pushedIds[i]) != acceptedIds.end()) {
                    readAvailable(pair, pushedIds[i], pushed[i]);
                }
            }
            (void)owner.reactivateBlocked({.data = true, .control = true, .local = true});
            if (owner.activeTaskCount() == 0 && parent.fin && pushedIds.size() == 3 && pushed[0].fin && pushed[1].fin && pushed[2].fin) {
                break;
            }
            co_await ruvia::sleepFor(workerHandle, std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(parent.fin);
        RUVIA_CHECK_EQ(state.accepted, std::size_t{3});
        RUVIA_CHECK(state.metadataOwned);
        RUVIA_CHECK_EQ(owner.activeSessionStreamCount(), std::size_t{0});
        ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, worker.resource(), {.maxPushId = 2});
        RUVIA_CHECK(client.registerClientRequest(parentId, ruvia::HttpKnownMethod::kGet).scope == ruvia::Http3ConnectionErrorScope::kNone);
        struct Responses final {
            std::unordered_map<std::uint64_t, std::string> bodies;
            std::string trailer;
        } responses;
        auto capture = +[](void* raw, const ruvia::Http3ConnectionEvent& event) {
            auto& captured = *static_cast<Responses*>(raw);
            if (event.kind == ruvia::Http3ConnectionEventKind::kBody) {
                captured.bodies[event.streamId].append(event.body.data(), event.body.size());
            } else if (event.kind == ruvia::Http3ConnectionEventKind::kTrailerField) {
                captured.trailer = event.trailer.value;
            }
        };
        RUVIA_CHECK(client.feed(parentId, std::span(parent.bytes), true, false, capture, &responses).scope == ruvia::Http3ConnectionErrorScope::kNone);
        for (std::size_t i = 0; i < pushedIds.size(); ++i) {
            RUVIA_CHECK(pushed[i].fin);
            RUVIA_CHECK(client.feed(pushedIds[i], std::span(pushed[i].bytes), true, false, capture, &responses).scope == ruvia::Http3ConnectionErrorScope::kNone);
        }
        RUVIA_CHECK_EQ(responses.bodies[parentId], "parent");
        RUVIA_CHECK_EQ(responses.bodies[pushedIds[0]], "asset");
        RUVIA_CHECK_EQ(responses.bodies[pushedIds[1]], state.body);
        RUVIA_CHECK_EQ(responses.bodies[pushedIds[2]], state.body);
        RUVIA_CHECK_EQ(responses.trailer, "done");
    } catch (...) {
        failure = std::current_exception();
    }
    (void)owner.requestStop();
    while (const auto intent = owner.peekTransportIntent()) {
        if (intent->token.kind == Owner::TransportIntentKind::kOpenPushStream) {
            (void)owner.ackTransportIntent(intent->token, Owner::PushStreamOpenResult{.status = Owner::PushStreamOpenResult::Status::kStopped});
        } else {
            (void)owner.ackTransportIntent(intent->token);
        }
    }
    co_await owner.join();
    RUVIA_CHECK(output.stop().status != Output::Status::kUnsafeToRelease);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace
#endif

RUVIA_TEST(http3ServerPushBufferedStreamingAndFileRoutesUseRealQuicUnidirectionalStreams) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    QuicPair pair;
    pair.connect();
    auto requestHead = ruvia::encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test", .path = "/push"});
    RUVIA_CHECK(requestHead.has_value());
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> frame{};
    const auto prefix = ruvia::encodeHttp3FrameHeader(frame, 1, requestHead->fieldSection.size());
    std::vector<char> request(frame.begin(), frame.begin() + *prefix);
    request.insert(request.end(), requestHead->fieldSection.begin(), requestHead->fieldSection.end());
    const auto parentId = pair.openRequestStream(true, request);
    const auto control = pair.client().open_stream(true);
    RUVIA_CHECK_EQ(control.stream_id, std::uint64_t{2});
    constexpr std::array<char, 6> settingsAndMax{0, 4, 0, 0xd, 1, 2};
    pair.writeClientStream(control.stream_id, settingsAndMax);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::string receivedControl;
    while (receivedControl.size() < settingsAndMax.size() && std::chrono::steady_clock::now() < deadline) {
        pair.pump();
        if (pair.acceptedStream(control.stream_id)) {
            std::array<char, 64> buffer{};
            const auto read = pair.server().read_stream(control.stream_id, std::as_writable_bytes(std::span(buffer)));
            if (read.status == ruvia::quic_stream_read_status::data) {
                receivedControl.append(buffer.data(), read.size);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK_EQ(receivedControl, std::string(settingsAndMax.data(), settingsAndMax.size()));
    PushRouteState state;
    state.body.assign(65539, 's');
    state.filePath = std::filesystem::temp_directory_path() / ("ruvia-h3-push-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream file(state.filePath, std::ios::binary);
        file << state.body;
    }
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    ruvia::test::CountingMemoryResource upstream;
    ruvia::BlockingPool pool({.threadCount = 1, .queueCapacity = 8});
    auto root = attachment.loop().start(exerciseQuicPushRoutes(attachment, pair, parentId, request, settingsAndMax, state, pool, upstream, ruvia_ctx));
    attachment.run();
    root.get();
    pool.stop();
    pool.join();
    std::filesystem::remove(state.filePath);
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
#endif
}
