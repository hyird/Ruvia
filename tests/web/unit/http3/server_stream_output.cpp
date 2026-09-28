#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/web/detail/http3/Http3BufferedResponseWrite.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3ServerStreamOutput.h"

#include "test_harness.h"

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
namespace {

using namespace ruvia::detail;
using ConnectionId = Http3QuicServerTransport::ConnectionId;
using Output = Http3ServerStreamOutput;
using Mailbox = Http3StreamMailbox;
using BorrowedBlock = Http3StreamMailbox::BorrowedBlock;

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

Http3QuicDatagramAddress serverAddress() {
    Http3QuicDatagramAddress address;
    address.address[0] = 127;
    address.address[3] = 1;
    address.port = 4433;
    return address;
}

Http3QuicDatagramAddress clientAddress() {
    auto address = serverAddress();
    address.port = 15433;
    return address;
}

HttpServerListenerDefinition::Tls serverTlsConfig(const IdentityFiles& files) {
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificateFile().string();
    config.identity.privateKeyFile = files.privateKeyFile().string();
    return config;
}

class QuicPair final {
public:
    QuicPair()
        : files_(),
          serverTls_(serverTlsConfig(files_), std::pmr::get_default_resource()),
          clientTls_(ClientTransportConfigView{}),
          serverBridge_(serverAddress()),
          clientBridge_(clientAddress()),
          server_(serverTls_, serverBridge_) {
        SSL_CTX_set_verify(clientTls_.nativeHandle(), SSL_VERIFY_NONE, nullptr);
        client_ = std::make_unique<Http3QuicClientTransport>(
            clientTls_, clientBridge_, serverAddress(), "localhost");
    }

    ~QuicPair() {
        client_->close();
        if (connectionId_) {
            (void)server_.retireConnectionLocally(*connectionId_);
        }
    }

    QuicPair(const QuicPair&) = delete;
    QuicPair& operator=(const QuicPair&) = delete;

    void connect() {
        (void)client_->startConnect();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (connectionId_) {
                const auto info = server_.connectionInfo(*connectionId_);
                if (info && info->handshakeComplete && info->h3Negotiated &&
                    client_->connectionInfo() == Http3QuicClientTransport::State::kH3Ready) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("QUIC test handshake deadline");
    }

    [[nodiscard]] std::uint64_t openRequestStream(bool finishRequest = true) {
        requestStreamsRequested_ = true;
        const auto opened = client_->openLocalBidirectionalStream();
        if (opened.error != Http3QuicStreamSet::Error::kNone) {
            throw std::runtime_error("failed to open client request stream");
        }
        constexpr std::array<char, 1> trigger{'x'};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool written = false;
        while (!written && std::chrono::steady_clock::now() < deadline) {
            const auto result = client_->writeStream(opened.id, trigger);
            if (result.status == Http3QuicStreamSet::StreamWrite::Status::kAccepted &&
                result.bytes == trigger.size()) {
                written = true;
            } else if (result.status != Http3QuicStreamSet::StreamWrite::Status::kWouldBlock) {
                throw std::runtime_error("failed to write client request stream trigger");
            }
            if (!written) {
                pump();
            }
        }
        if (!written ||
            (finishRequest && client_->finishStream(opened.id) != Http3QuicStreamSet::Error::kNone)) {
            throw std::runtime_error("failed to finish client request stream");
        }
        while (!hasAcceptedStream(opened.id) && std::chrono::steady_clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!hasAcceptedStream(opened.id)) {
            throw std::runtime_error("server did not accept client bidi stream");
        }
        if (finishRequest) {
            const auto received = readServerToTerminal(opened.id);
            if (received.status != Http3QuicStreamSet::StreamRead::Status::kFin ||
                received.bytes != "x") {
                throw std::runtime_error("server did not drain the finished request stream");
            }
        }
        return opened.id;
    }

    struct ServerReceive final {
        std::string bytes;
        Http3QuicStreamSet::StreamRead::Status status{
            Http3QuicStreamSet::StreamRead::Status::kWouldBlock};
        std::optional<std::uint64_t> peerResetErrorCode{};
    };

    [[nodiscard]] ServerReceive readServerToTerminal(std::uint64_t streamId) {
        ServerReceive received;
        std::array<char, 256> buffer{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto read = server_.readStream(connectionId(), streamId, buffer);
            if (read.status == Http3QuicStreamSet::StreamRead::Status::kData) {
                received.bytes.append(buffer.data(), read.size);
            } else if (read.status == Http3QuicStreamSet::StreamRead::Status::kFin ||
                       read.status == Http3QuicStreamSet::StreamRead::Status::kReset) {
                received.status = read.status;
                received.peerResetErrorCode = read.peerResetErrorCode;
                return received;
            } else if (read.status == Http3QuicStreamSet::StreamRead::Status::kWouldBlock) {
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                throw std::runtime_error("server request stream read failed");
            }
        }
        throw std::runtime_error("server request stream terminal watchdog");
    }

    void writeClientStream(std::uint64_t streamId, std::span<const char> bytes) {
        std::size_t offset{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (offset != bytes.size() && std::chrono::steady_clock::now() < deadline) {
            const auto write = client_->writeStream(streamId, bytes.subspan(offset));
            if (write.status == Http3QuicStreamSet::StreamWrite::Status::kAccepted &&
                write.bytes != 0 && write.bytes <= bytes.size() - offset) {
                offset += write.bytes;
            } else if (write.status == Http3QuicStreamSet::StreamWrite::Status::kWouldBlock) {
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                throw std::runtime_error("client request tail write failed");
            }
        }
        if (offset != bytes.size()) {
            throw std::runtime_error("client request tail write watchdog");
        }
    }

    void finishClientStream(std::uint64_t streamId) {
        if (client_->finishStream(streamId) != Http3QuicStreamSet::Error::kNone) {
            throw std::runtime_error("client request tail FIN failed");
        }
    }

    [[nodiscard]] Http3QuicServerTransport& server() noexcept {
        return server_;
    }
    [[nodiscard]] Http3QuicClientTransport& client() noexcept {
        return *client_;
    }
    [[nodiscard]] ConnectionId connectionId() const {
        if (!connectionId_) {
            throw std::logic_error("test QUIC connection is not accepted");
        }
        return *connectionId_;
    }
    [[nodiscard]] const Http3QuicServerTransport::StreamInfo* acceptedStream(
        std::uint64_t streamId) const noexcept {
        for (std::size_t i = 0; i < acceptedCount_; ++i) {
            if (accepted_[i].id == streamId) {
                return &accepted_[i];
            }
        }
        return nullptr;
    }

    void pump() {
        relay(clientBridge_, serverBridge_, clientAddress());
        if (server_.handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
            throw std::runtime_error("QUIC test server event handling failed");
        }
        if (!connectionId_) {
            const auto accepted = server_.acceptConnections(1);
            if (accepted.size != 0) {
                connectionId_ = accepted.ids[0];
            }
        }
        if (connectionId_ && requestStreamsRequested_) {
            auto accepted = server_.acceptStreams(*connectionId_);
            if (accepted.error != Http3QuicStreamSet::Error::kNone &&
                accepted.error != Http3QuicStreamSet::Error::kHandshakePending &&
                accepted.error != Http3QuicStreamSet::Error::kStreamLimitRetry) {
                throw std::runtime_error("QUIC test server stream acceptance failed");
            }
            for (std::size_t i = 0; i < accepted.size; ++i) {
                if (acceptedCount_ >= accepted_.size()) {
                    throw std::runtime_error("QUIC test accepted-stream fixture is full");
                }
                accepted_[acceptedCount_++] = accepted.streams[i];
            }
        }
        relay(serverBridge_, clientBridge_, serverAddress());
        const auto afterRelay = client_->handleEvents();
        if (afterRelay == Http3QuicClientTransport::State::kTlsFailure ||
            afterRelay == Http3QuicClientTransport::State::kAlpnMismatch) {
            const auto info = connectionId_ ? server_.connectionInfo(*connectionId_) : std::nullopt;
            throw std::runtime_error("QUIC test client transport failed after relay: " +
                                     std::to_string(static_cast<unsigned>(afterRelay)) + " server=" +
                                     std::to_string(info.has_value()) + " hs=" +
                                     std::to_string(info && info->handshakeComplete));
        }
    }

private:
    static void relay(Http3QuicDatagramBridge& from, Http3QuicDatagramBridge& to,
        const Http3QuicDatagramAddress& source) {
        for (unsigned packet = 0; packet < 128; ++packet) {
            Http3QuicOutboundDatagram datagram;
            const auto result = from.takeOutbound(datagram);
            if (result == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
                return;
            }
            if (result == Http3QuicDatagramBridge::OutboundResult::kBusy) {
                return;
            }
            if (result != Http3QuicDatagramBridge::OutboundResult::kReady ||
                to.inject(datagram.bytes, source) != Http3QuicDatagramBridge::InjectResult::kAccepted) {
                throw std::runtime_error("QUIC test datagram relay failed");
            }
            from.completeOutbound();
        }
        throw std::runtime_error("QUIC test exceeded its datagram relay bound");
    }

    [[nodiscard]] bool hasAcceptedStream(std::uint64_t id) const noexcept {
        return acceptedStream(id) != nullptr;
    }

    IdentityFiles files_;
    Http3QuicTlsContext serverTls_;
    Http3QuicClientTlsContext clientTls_;
    Http3QuicDatagramBridge serverBridge_;
    Http3QuicDatagramBridge clientBridge_;
    Http3QuicServerTransport server_;
    std::unique_ptr<Http3QuicClientTransport> client_;
    std::optional<ConnectionId> connectionId_;
    std::array<Http3QuicServerTransport::StreamInfo, 32> accepted_{};
    std::size_t acceptedCount_{};
    bool requestStreamsRequested_{};
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

[[nodiscard]] Http3StreamMessageId messageId(std::uint64_t streamId,
    std::uint64_t epoch = kEpoch, std::uint64_t generation = kGeneration) noexcept {
    return {.epoch = epoch, .connectionGeneration = generation, .streamId = streamId};
}

[[nodiscard]] BorrowedBlock enqueueBlock(Mailbox& mailbox, Http3StreamMessageId id,
    std::span<const char> bytes) {
    const auto input = std::as_bytes(bytes);
    const auto sent = mailbox.trySend(id, input);
    if (sent != Mailbox::SendResult::kSent && sent != Mailbox::SendResult::kSentNotifyPeer) {
        throw std::runtime_error("failed to enqueue HTTP/3 test block");
    }
    BorrowedBlock block;
    if (!mailbox.tryReceive(block)) {
        throw std::runtime_error("failed to receive HTTP/3 test block");
    }
    return block;
}

[[nodiscard]] std::pmr::vector<char> encodeResponse(std::string_view body,
    std::pmr::memory_resource* resource) {
    ruvia::HttpResponse response;
    response.body(body);
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    auto created = Http3BufferedResponseWrite::create(response, plan, resource);
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
    if (!cursor.finReady() || !cursor.acknowledgeFin(true) || !cursor.finished()) {
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
};

void readAvailable(QuicPair& pair, std::uint64_t streamId, ReceivedWire& received) {
    std::array<char, 2048> buffer{};
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto read = pair.client().readStream(streamId, buffer);
        if (read.status == Http3QuicStreamSet::StreamRead::Status::kData) {
            received.bytes.append(buffer.data(), read.size);
        } else if (read.status == Http3QuicStreamSet::StreamRead::Status::kFin) {
            received.fin = true;
            return;
        } else if (read.status == Http3QuicStreamSet::StreamRead::Status::kWouldBlock) {
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
    (void)mailbox.drainReturns();
    std::array<char, Mailbox::kMaxBlockBytes> bytes{};
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 8, .maxQueuedBlocks = 8, .maxDriveWorkItems = 1});

    auto foreign = enqueueBlock(mailbox, messageId(firstId, kEpoch + 1), firstWire);
    RUVIA_CHECK(output.acceptData(foreign).status == Output::Status::kForeignEpoch);
    RUVIA_CHECK(foreign);
    foreign.release();
    (void)mailbox.drainReturns();
    RUVIA_CHECK_EQ(output.trackedStreamCount(), std::size_t{0});
    RUVIA_CHECK(pair.server().connectionInfo(pair.connectionId()).has_value());

    auto firstBlock = enqueueBlock(mailbox, messageId(firstId), firstWire);
    auto secondBlock = enqueueBlock(mailbox, messageId(secondId), secondWire);
    const Http3StreamControl firstFin{.kind = Http3StreamControl::Kind::kStreamFin,
        .id = messageId(firstId),
        .value = firstWire.size()};
    RUVIA_CHECK(output.acceptControl(firstFin).status == Output::Status::kFinDeferred);
    RUVIA_CHECK(output.acceptControl(firstFin).status == Output::Status::kDuplicateFin);
    RUVIA_CHECK(output.acceptData(firstBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!firstBlock);
    RUVIA_CHECK(output.acceptData(secondBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!secondBlock);
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
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
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{2});
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

    const auto conflict = output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
        .id = messageId(firstId),
        .value = firstWire.size() + 1});
    RUVIA_CHECK(conflict.status == Output::Status::kFinalSizeError);
    RUVIA_CHECK(output.connectionRetired());
    RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
    RUVIA_CHECK(mailbox.stop());
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
        {.writeTimeout = std::chrono::milliseconds(5)});
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kAccepted);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        (void)output.drive();
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
    RUVIA_CHECK(pair.server().connectionInfo(pair.connectionId()).has_value());
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kDuplicateFin);
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration);
    const auto responseWire = encodeResponse("response before request end", worker.resource());
    auto block = enqueueBlock(mailbox, messageId(streamId), responseWire);
    RUVIA_CHECK(output.acceptData(block).status == Output::Status::kAccepted);
    RUVIA_CHECK(!block);
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
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
    RUVIA_CHECK(pair.server().retireCompletedBidirectionalStream(pair.connectionId(), streamId) ==
                Http3QuicStreamSet::Error::kWouldBlock);

    constexpr std::array<char, 9> tail{'-', 't', 'a', 'i', 'l', '-', 'o', 'k', '!'};
    pair.writeClientStream(streamId, tail);
    pair.finishClientStream(streamId);
    const auto request = pair.readServerToTerminal(streamId);
    RUVIA_CHECK(request.status == Http3QuicStreamSet::StreamRead::Status::kFin);
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
    std::array<char, 1> readBuffer{};
    RUVIA_CHECK_EQ(pair.server().readStream(pair.connectionId(), streamId, readBuffer).status,
        Http3QuicStreamSet::StreamRead::Status::kNoStream);
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
                                         .id = messageId(streamId),
                                         .value = responseWire.size()})
                    .status == Output::Status::kDuplicateFin);
    const auto repeated = output.drive();
    RUVIA_CHECK_EQ(repeated.finishedStreams, std::size_t{0});
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    (void)mailbox.drainReturns();
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration);
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
                                         .id = messageId(streamId),
                                         .value = 0})
                    .status == Output::Status::kAccepted);
    (void)output.drive();
    auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->sendFinAccepted);
    RUVIA_CHECK(info && info->state == Output::StreamState::kFinPending);
    RUVIA_CHECK(pair.server().retireCompletedBidirectionalStream(pair.connectionId(), streamId) ==
                Http3QuicStreamSet::Error::kWouldBlock);

    const auto request = pair.readServerToTerminal(streamId);
    RUVIA_CHECK(request.status == Http3QuicStreamSet::StreamRead::Status::kFin);
    RUVIA_CHECK_EQ(request.bytes, std::string("x"));
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration);
    const auto fin = output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
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
    RUVIA_CHECK(pair.client().resetStream(streamId, static_cast<std::uint64_t>(resetCode)) ==
                Http3QuicStreamSet::Error::kNone);
    const auto request = pair.readServerToTerminal(streamId);
    RUVIA_CHECK(request.status == Http3QuicStreamSet::StreamRead::Status::kReset);
    RUVIA_CHECK(request.peerResetErrorCode == static_cast<std::uint64_t>(resetCode));
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
    std::array<char, 1> readBuffer{};
    RUVIA_CHECK_EQ(pair.server().readStream(pair.connectionId(), streamId, readBuffer).status,
        Http3QuicStreamSet::StreamRead::Status::kNoStream);
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration);
    RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
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
    std::array<char, 1> readBuffer{};
    RUVIA_CHECK_EQ(pair.server().readStream(pair.connectionId(), streamId, readBuffer).status,
        Http3QuicStreamSet::StreamRead::Status::kNoStream);
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration);
    constexpr auto resetCode = ruvia::Http3ConnectionErrorCode::kMessageError;

    const auto reset = output.acceptControl({.kind = Http3StreamControl::Kind::kStreamReset,
        .id = messageId(streamId),
        .streamResetErrorCode = resetCode});
    RUVIA_CHECK(reset.status == Output::Status::kReset);
    RUVIA_CHECK(reset.termination.send == Http3QuicServerTransport::Error::kNone);
    RUVIA_CHECK(reset.termination.close == Http3QuicServerTransport::Error::kNone);

    Http3QuicClientTransport::StreamRead peerReset;
    std::array<char, 16> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline &&
           peerReset.status != Http3QuicClientTransport::StreamRead::Status::kReset) {
        pair.pump();
        peerReset = pair.client().readStream(streamId, buffer);
        if (peerReset.status == Http3QuicClientTransport::StreamRead::Status::kWouldBlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    RUVIA_CHECK(peerReset.status == Http3QuicClientTransport::StreamRead::Status::kReset);
    RUVIA_CHECK(peerReset.peerResetErrorCode.has_value());
    RUVIA_CHECK(peerReset.peerResetErrorCode ==
                static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto info = output.streamInfo(streamId);
    RUVIA_CHECK(info && info->state == Output::StreamState::kReset);
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
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
            Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
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
            RUVIA_CHECK(cancelled.termination.close == Http3QuicStreamSet::Error::kNone);
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK(!output.streamInfo(firstId)->queuedBlocks);
            RUVIA_CHECK(backpressured);
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
            backpressured.release();
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});

            auto stopBlock = enqueueBlock(mailbox, messageId(secondId), bytes);
            RUVIA_CHECK(output.acceptData(stopBlock).status == Output::Status::kAccepted);
            RUVIA_CHECK(!stopBlock);
            RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
            RUVIA_CHECK(output.stopped());
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
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
            Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
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
                    (void)mailbox.drainReturns();
                }
            }
            RUVIA_CHECK(wouldBlock);
            RUVIA_CHECK(supply.lastAddress != nullptr && supply.lastSize != 0);
            RUVIA_CHECK_EQ(output.liveStreamCount(), std::size_t{1});
            RUVIA_CHECK_EQ(output.pendingStreamCount(), std::size_t{1});
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{0});

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
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{0});

            std::this_thread::sleep_for(writeTimeout + std::chrono::milliseconds(10));
            std::size_t timedOut{};
            for (std::size_t attempt = 0; attempt < 32 && timedOut == 0; ++attempt) {
                timedOut += output.drive().timedOutStreams;
            }
            RUVIA_CHECK_EQ(timedOut, std::size_t{1});
            const auto timedOutInfo = output.streamInfo(blockedId);
            RUVIA_CHECK(timedOutInfo && timedOutInfo->state == Output::StreamState::kCancelled);
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
            RUVIA_CHECK(!output.connectionRetired());
            RUVIA_CHECK(pair.server().connectionInfo(pair.connectionId()).has_value());

            const auto siblingWire = encodeResponse("sibling survives timeout", worker.resource());
            auto siblingBlock = enqueueBlock(
                mailbox, messageId(siblingId), siblingWire);
            RUVIA_CHECK(output.acceptData(siblingBlock).status == Output::Status::kAccepted);
            RUVIA_CHECK(!siblingBlock);
            RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
                                                 .id = messageId(siblingId),
                                                 .value = siblingWire.size()})
                            .status == Output::Status::kFinDeferred);

            const auto lateDataBytes = std::as_bytes(std::span<const char>("late", 4));
            const auto lateSent = mailbox.trySend(messageId(blockedId), lateDataBytes);
            RUVIA_CHECK(lateSent == Mailbox::SendResult::kSent ||
                        lateSent == Mailbox::SendResult::kSentNotifyPeer);
            BorrowedBlock lateBlock;
            RUVIA_CHECK(mailbox.tryReceive(lateBlock));
            RUVIA_CHECK(output.acceptData(lateBlock).status == Output::Status::kClosedStream);
            RUVIA_CHECK(lateBlock);
            lateBlock.release();
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
            RUVIA_CHECK(output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
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
                (void)mailbox.drainReturns();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto blockedInfo = output.streamInfo(blockedId);
            const auto siblingInfo = output.streamInfo(siblingId);
            RUVIA_CHECK_EQ(timedOut, std::size_t{1});
            RUVIA_CHECK(blockedInfo && blockedInfo->state == Output::StreamState::kCancelled);
            RUVIA_CHECK(blockedInfo &&
                        blockedInfo->termination.send == Http3QuicServerTransport::Error::kNone);
            RUVIA_CHECK(siblingInfo && siblingInfo->state == Output::StreamState::kFinished);
            RUVIA_CHECK(siblingReceived.fin);
            RUVIA_CHECK_EQ(siblingReceived.bytes.size(), siblingWire.size());
            RUVIA_CHECK(!output.connectionRetired());
            RUVIA_CHECK(pair.server().connectionInfo(pair.connectionId()).has_value());
            RUVIA_CHECK_EQ(output.trackedStreamCount(), std::size_t{2});
            RUVIA_CHECK_EQ(output.liveStreamCount(), std::size_t{0});
            RUVIA_CHECK_EQ(output.pendingStreamCount(), std::size_t{0});
            RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
            RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{0});
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
        {.maxTrackedStreams = 4, .maxQueuedBlocks = 2, .maxDriveWorkItems = 1});
    constexpr std::array<char, 4> bytes{'d', 'a', 't', 'a'};

    auto closedBlock = enqueueBlock(mailbox, messageId(closedId), bytes);
    RUVIA_CHECK(output.acceptData(closedBlock).status == Output::Status::kAccepted);
    RUVIA_CHECK(!closedBlock);
    RUVIA_CHECK(pair.server().closeStream(pair.connectionId(), closedId) ==
                Http3QuicStreamSet::Error::kNone);
    const auto cancelled = output.cancelStream(closedId);
    RUVIA_CHECK(cancelled.status == Output::Status::kCancelled);
    RUVIA_CHECK(cancelled.termination.close == Http3QuicStreamSet::Error::kNoStream);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
    RUVIA_CHECK(pair.server().connectionInfo(pair.connectionId()).has_value());

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
    RUVIA_CHECK(pair.server().connectionInfo(pair.connectionId()).has_value());
    RUVIA_CHECK(!output.connectionRetired());
    RUVIA_CHECK(output.cancelStream(siblingId).status == Output::Status::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
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
            (void)mailbox.drainReturns();
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
            (void)mailbox.drainReturns();
        }
        ++steps;
    }

    RUVIA_CHECK(firstWouldBlock);
    RUVIA_CHECK(secondWouldBlock);
    const auto firstInfo = output.streamInfo(firstId);
    const auto secondInfo = output.streamInfo(secondId);
    RUVIA_CHECK(firstInfo && firstInfo->lastWriteStatus == Output::StreamWrite::Status::kWouldBlock);
    RUVIA_CHECK(secondInfo && secondInfo->lastWriteStatus == Output::StreamWrite::Status::kWouldBlock);
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
                firstRetriedInfo->lastWriteStatus == Output::StreamWrite::Status::kWouldBlock &&
                firstRetriedInfo->state == Output::StreamState::kOpen);
    RUVIA_CHECK(secondRetriedInfo &&
                secondRetriedInfo->lastWriteStatus == Output::StreamWrite::Status::kWouldBlock &&
                secondRetriedInfo->state == Output::StreamState::kOpen);
    idle = output.drive();
    RUVIA_CHECK_EQ(idle.operations, std::size_t{0});
    RUVIA_CHECK(!idle.needsReschedule);

    (void)mailbox.drainReturns();
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{0});
    RUVIA_CHECK(output.cancelStream(firstId).status == Output::Status::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
    RUVIA_CHECK(output.cancelStream(secondId).status == Output::Status::kCancelled);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
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
    Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
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
            (void)mailbox.drainReturns();
        }
    }
    RUVIA_CHECK(sawWouldBlock);
    RUVIA_CHECK(supply.lastAddress != nullptr && supply.lastSize != 0);
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{1});
    (void)mailbox.drainReturns();
    RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{0});
    const auto blockedInfo = output.streamInfo(streamId);
    RUVIA_CHECK(blockedInfo && blockedInfo->lastWriteStatus == Output::StreamWrite::Status::kWouldBlock);

    const auto fin = output.acceptControl({.kind = Http3StreamControl::Kind::kStreamFin,
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
        (void)mailbox.drainReturns();
        readAvailable(pair, streamId, received);
        const auto info = output.streamInfo(streamId);
        complete = info && info->state == Output::StreamState::kFinished && received.fin;
    }
    RUVIA_CHECK(complete);
    RUVIA_CHECK_EQ(received.bytes.size(), static_cast<std::size_t>(supply.bytes));
    RUVIA_CHECK(matchesPattern(received.bytes, 0));
    RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
    RUVIA_CHECK(output.stop().status == Output::Status::kStopped);
    (void)mailbox.drainReturns();
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
        Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration,
            {.maxTrackedStreams = 1, .maxQueuedBlocks = 2, .maxDriveWorkItems = 2});
        constexpr std::array<char, 2> bytes{'o', 'k'};
        auto stale = enqueueBlock(mailbox, messageId(streamId, kEpoch, kGeneration + 1), bytes);
        RUVIA_CHECK(output.acceptData(stale).status == Output::Status::kStaleConnection);
        RUVIA_CHECK(stale);
        stale.release();
        (void)mailbox.drainReturns();

        auto accepted = enqueueBlock(mailbox, messageId(streamId), bytes);
        RUVIA_CHECK(output.acceptData(accepted).status == Output::Status::kAccepted);
        RUVIA_CHECK(!accepted);
        const auto exhausted = output.acceptControl({.kind = Http3StreamControl::Kind::kWritable,
            .id = messageId(streamId + 4)});
        RUVIA_CHECK(exhausted.status == Output::Status::kCapacityExhausted);
        RUVIA_CHECK(output.connectionRetired());
        RUVIA_CHECK_EQ(output.queuedBlockCount(), std::size_t{0});
        RUVIA_CHECK_EQ(mailbox.drainReturns(), std::uint32_t{1});
        RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
        RUVIA_CHECK(mailbox.stop());
    }

    {
        QuicPair pair;
        pair.connect();
        (void)pair.openRequestStream();
        ruvia::WorkerMemory worker;
        Output output(pair.server(), pair.connectionId(), worker, kEpoch, kGeneration);
        const auto invalid = output.acceptControl({.kind = Http3StreamControl::Kind::kWritable,
            .id = messageId(2)});
        RUVIA_CHECK(invalid.status == Output::Status::kInvalidStreamId);
        RUVIA_CHECK(output.connectionRetired());
        RUVIA_CHECK(output.stop().status == Output::Status::kConnectionClosed);
    }
#endif
}
