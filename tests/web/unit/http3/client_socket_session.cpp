#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/use_future.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/AsioTask.h"

#include "http3/Http3QuicClientSocketSession.h"
#include "http3/Http3QuicClientTlsContext.h"
#include "http3_quic_udp_pair.h"
#include "server/HttpServerOptions.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace ruvia::detail {
struct Http3QuicClientSocketSessionTestAccess final {
    static std::size_t rejected_early_stream_capacity(
        const Http3QuicClientSocketSession& session) noexcept {
        return session.rejected_early_streams_.capacity();
    }

    static void close_candidate_socket(Http3QuicClientSocketSession& session) noexcept {
        if (session.candidate_socket_) {
            asio::error_code ignored;
            session.candidate_socket_->close(ignored);
        }
    }

    struct scripted_sender final {
        std::size_t calls{};

        [[nodiscard]] std::size_t operator()(asio::ip::udp::socket&,
            asio::const_buffer, asio::error_code& error) noexcept {
            ++calls;
            error = asio::error::would_block;
            return 0;
        }
    };

    static Http3QuicClientSocketSession::PumpResult pump_with_sender(
        Http3QuicClientSocketSession& session, scripted_sender& sender) {
        return session.pump_with_send(sender);
    }

    static void queue_pending_packet(Http3QuicClientSocketSession& session) noexcept {
        session.packetBuffer_[0] = std::byte{0};
        session.pending_packet_size_ = 1;
        session.pending_candidate_ = false;
    }
};
}  // namespace ruvia::detail

namespace {

template <typename T>
bool drive_until_ready(asio::io_context& io, std::future<T>& future,
    std::chrono::steady_clock::time_point deadline) {
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        io.restart();
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const auto maximum_slice = std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(std::chrono::milliseconds(10));
        io.run_for(std::min(remaining, maximum_slice));
    }
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

class quic_test_identity final {
public:
    using tls_config = ruvia::detail::HttpServerListenerDefinition::Tls;

    quic_test_identity() {
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-quic-migration-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create QUIC test identity directory");
        }
        key_.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"));
        certificate_.reset(X509_new_ex(nullptr, nullptr));
        if (!key_ || !certificate_ || X509_set_version(certificate_.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate_.get()), 1) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(certificate_.get()), -60) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(certificate_.get()), 3600) == nullptr ||
            X509_set_pubkey(certificate_.get(), key_.get()) != 1) {
            throw std::runtime_error("failed to create QUIC test certificate");
        }
        const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        if (subject == nullptr ||
            X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1 ||
            X509_set_subject_name(certificate_.get(), subject.get()) != 1 ||
            X509_set_issuer_name(certificate_.get(), subject.get()) != 1 ||
            ruvia::test::sign_tls_certificate(certificate_.get(), key_.get()) <= 0) {
            throw std::runtime_error("failed to sign QUIC test certificate");
        }
        certificate_path_ = directory_ / "certificate.pem";
        private_key_path_ = directory_ / "private-key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
            BIO_new_file(certificate_path_.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
            BIO_new_file(private_key_path_.string().c_str(), "w"), BIO_free);
        if (!certificate_bio || !key_bio ||
            PEM_write_bio_X509(certificate_bio.get(), certificate_.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key_.get()) != 1) {
            throw std::runtime_error("failed to write QUIC test certificate");
        }
    }

    ~quic_test_identity() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] tls_config server_tls_config() const {
        tls_config config;
        config.identity.certificateChainFile = certificate_path_.string();
        config.identity.privateKeyFile = private_key_path_.string();
        return config;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificate_path_;
    std::filesystem::path private_key_path_;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
    std::unique_ptr<X509, decltype(&X509_free)> certificate_{nullptr, X509_free};
};

}  // namespace

RUVIA_TEST(http3QuicClientSocketSessionOwnsConcreteConnectedSocket) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls,
        {.qpackMaxTableCapacity = 4096, .qpackBlockedStreams = 16},
        ruvia::quic_version::v1, false, nullptr);
    RUVIA_CHECK_EQ(Http3QuicClientSocketSessionTestAccess::
                       rejected_early_stream_capacity(session),
        0U);
    RUVIA_CHECK(session.localEndpoint().port() != 0);
    RUVIA_CHECK(session.localEndpoint().address().is_loopback());
    RUVIA_CHECK(session.peerEndpoint() == peerSocket.local_endpoint());
    const auto first = session.pump();
    RUVIA_CHECK(first.status == Http3QuicClientSocketSession::PumpStatus::kActive ||
                first.status == Http3QuicClientSocketSession::PumpStatus::kWouldBlock);
    const std::array<char, 0> empty{};
    asio::error_code error;
    (void)peerSocket.send_to(asio::buffer(empty), session.localEndpoint(), 0, error);
    RUVIA_CHECK(!error);
    const auto ignored = session.pump();
    RUVIA_CHECK(ignored.status != Http3QuicClientSocketSession::PumpStatus::kFatal);
    RUVIA_CHECK(ignored.received != 0);
    auto unstarted = session.waitReadable();
    session.close();
    RUVIA_CHECK(session.pump().status == Http3QuicClientSocketSession::PumpStatus::kClosed);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionKeepsPollingDuringSendBackpressure) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peer.local_endpoint(), "localhost", tls);
    (void)session.pump();
    Http3QuicClientSocketSessionTestAccess::scripted_sender sender;
    Http3QuicClientSocketSessionTestAccess::queue_pending_packet(session);
    const std::array<char, 1> input{'x'};
    (void)peer.send_to(asio::buffer(input), session.localEndpoint());
    const auto tick = Http3QuicClientSocketSessionTestAccess::pump_with_sender(session, sender);
    RUVIA_CHECK_EQ(sender.calls, 1U);
    RUVIA_CHECK(tick.outputBackpressured);
    RUVIA_CHECK(tick.received > 0);
    RUVIA_CHECK(tick.eventTimeout.has_value());
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionMigratesWithTwoLiveUdpPaths) {
    using namespace ruvia::detail;
    using udp = asio::ip::udp;
    quic_test_identity identity;
    ruvia::testing::http3_quic_udp_pair peer(
        identity.server_tls_config(), ruvia::testing::http3_quic_udp_pair::server_only_t{});
    asio::io_context io;
    http3_quic_client_tls_context tls(ClientTransportConfigView{
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification});
    Http3QuicClientSocketSession session(io, peer.server_endpoint(), "localhost", tls);

    const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!session.transport().info().confirmed &&
           std::chrono::steady_clock::now() < handshake_deadline) {
        (void)session.pump();
        peer.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(session.transport().info().confirmed);

    const auto old_endpoint = session.localEndpoint();
    // A connected UDP endpoint may be rebound on Windows. If the OS allows
    // the candidate socket, QUIC rejects migrating to its existing path.
    bool same_path_rejected = false;
    try {
        const auto same_path = session.start_path_migration(old_endpoint);
        same_path_rejected = same_path.status == ruvia::quic_migration_status::rejected;
    } catch (const std::system_error& error) {
        same_path_rejected = error.code() == asio::error::address_in_use;
    }
    RUVIA_CHECK(same_path_rejected);
    RUVIA_CHECK(session.localEndpoint() == old_endpoint);
    RUVIA_CHECK(!session.active_path_migration());

    const auto reserve_endpoint = [&] {
        udp::socket reservation(io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        return reservation.local_endpoint();
    };
    const auto candidate_endpoint = reserve_endpoint();
    const auto migration = session.start_path_migration(candidate_endpoint);
    RUVIA_CHECK(migration.status == ruvia::quic_migration_status::started);
    const auto validation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> validated;
    while (std::chrono::steady_clock::now() < validation_deadline) {
        (void)session.pump();
        peer.pump();
        validated = session.path_migration(migration.id);
        if (!validated || validated->status != ruvia::quic_migration_status::started) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(validated.has_value());
    RUVIA_CHECK(validated->status == ruvia::quic_migration_status::validated);
    RUVIA_CHECK(session.localEndpoint() == candidate_endpoint);

    const auto return_migration = session.start_path_migration(old_endpoint);
    RUVIA_CHECK(return_migration.status == ruvia::quic_migration_status::started ||
                return_migration.status == ruvia::quic_migration_status::validated);
    const auto return_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> returned;
    while (std::chrono::steady_clock::now() < return_deadline) {
        (void)session.pump();
        peer.pump();
        returned = session.path_migration(return_migration.id);
        if (!returned || returned->status != ruvia::quic_migration_status::started) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(returned.has_value());
    RUVIA_CHECK(returned->status == ruvia::quic_migration_status::validated);
    RUVIA_CHECK(session.localEndpoint() == old_endpoint);

    const auto candidate_io_endpoint = reserve_endpoint();
    const auto candidate_io_migration = session.start_path_migration(candidate_io_endpoint);
    RUVIA_CHECK(candidate_io_migration.status == ruvia::quic_migration_status::started);
    RUVIA_CHECK(session.consumeWorkNotification());
    const auto candidate_activity = session.pump();
    auto candidate_wait = asio::co_spawn(io,
        ruvia::asAwaitable(session.waitForActivity(candidate_activity)), asio::use_future);
    io.restart();
    RUVIA_CHECK(io.run_one_for(std::chrono::seconds(1)) != 0);
    RUVIA_CHECK(candidate_wait.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    Http3QuicClientSocketSessionTestAccess::close_candidate_socket(session);
    const auto candidate_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    RUVIA_CHECK(drive_until_ready(io, candidate_wait, candidate_wait_deadline));
    RUVIA_CHECK(candidate_wait.get() ==
                Http3QuicClientSocketSession::WakeReason::kCandidateFailure);
    (void)session.pump();
    peer.pump();
    const auto candidate_io_failed = session.path_migration(candidate_io_migration.id);
    RUVIA_CHECK(candidate_io_failed.has_value());
    RUVIA_CHECK(candidate_io_failed->status == ruvia::quic_migration_status::failed);
    RUVIA_CHECK(session.localEndpoint() == old_endpoint);
    RUVIA_CHECK(session.transport().info().state == ruvia::quic_connection_state::ready);

    const auto fallback_endpoint = session.localEndpoint();
    const auto failing_endpoint = reserve_endpoint();
    const auto pending_validation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> failing;
    while (std::chrono::steady_clock::now() < pending_validation_deadline) {
        const auto attempt = session.start_path_migration(failing_endpoint);
        if (attempt.status == ruvia::quic_migration_status::started ||
            attempt.status == ruvia::quic_migration_status::validated) {
            failing = attempt;
            break;
        }
        (void)session.pump();
        peer.pump(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(failing.has_value());
    const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> failed;
    while (failing && std::chrono::steady_clock::now() < failure_deadline) {
        (void)session.pump();
        peer.pump(true);
        failed = session.path_migration(failing->id);
        if (!failed || failed->status != ruvia::quic_migration_status::started) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(failed.has_value());
    RUVIA_CHECK(failed->status == ruvia::quic_migration_status::failed);
    RUVIA_CHECK(session.localEndpoint() == fallback_endpoint);

    const auto cancel_endpoint = reserve_endpoint();
    const auto cancel = session.start_path_migration(cancel_endpoint);
    RUVIA_CHECK(cancel.status == ruvia::quic_migration_status::started);
    RUVIA_CHECK(session.consumeWorkNotification());
    const auto activity = session.pump();
    auto wait = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(activity)),
        asio::use_future);
    io.restart();
    RUVIA_CHECK(io.run_one_for(std::chrono::seconds(1)) != 0);
    RUVIA_CHECK(wait.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    RUVIA_CHECK(session.cancel_path_migration(cancel.id) == ruvia::quic_operation_status::accepted);
    session.close();
    const auto stop_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    RUVIA_CHECK(drive_until_ready(io, wait, stop_wait_deadline));
    RUVIA_CHECK(wait.get() == Http3QuicClientSocketSession::WakeReason::kStopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    RUVIA_CHECK(session.pump().status == Http3QuicClientSocketSession::PumpStatus::kClosed);
    const auto cancelled = session.path_migration(cancel.id);
    RUVIA_CHECK(cancelled.has_value());
    RUVIA_CHECK(cancelled->status == ruvia::quic_migration_status::aborted);
}

RUVIA_TEST(http3QuicClientSocketSessionWriteWaitCompletesWhenSocketIsReady) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitWritable()), asio::use_future);
    io.run();
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] { future.get(); }));
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitWakesOnReadableDatagram) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    const std::array<char, 1> datagram{'x'};
    (void)peerSocket.send_to(asio::buffer(datagram), session.localEndpoint());
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kReadable);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitRetriesFullInputViaQuicTimer) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    const std::array<char, 1> datagram{'x'};
    (void)peerSocket.send_to(asio::buffer(datagram), session.localEndpoint());
    Http3QuicClientSocketSession::PumpResult fullInput;
    fullInput.inputBackpressured = true;
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(fullInput)),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kQuicEvent);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitDrainsReadAndTimerAfterWritable) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    Http3QuicClientSocketSession::PumpResult blockedOutput;
    blockedOutput.outputBackpressured = true;
    blockedOutput.eventTimeout = std::chrono::milliseconds(25);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(blockedOutput)),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kWritable);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitPreservesAbsoluteDeadlineAcrossTicks) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    Http3QuicClientSocketSession::PumpResult pendingInput;
    pendingInput.inputBackpressured = true;
    pendingInput.eventTimeout = std::chrono::milliseconds(2);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(18);
    bool expired{};
    int quicEvents{};
    for (int i = 0; i < 32; ++i) {
        auto future = asio::co_spawn(io,
            ruvia::asAwaitable(session.waitForActivity(pendingInput, deadline)),
            asio::use_future);
        io.run();
        io.restart();
        const auto reason = future.get();
        if (reason == Http3QuicClientSocketSession::WakeReason::kDeadline) {
            expired = true;
            break;
        }
        RUVIA_CHECK(reason == Http3QuicClientSocketSession::WakeReason::kQuicEvent);
        ++quicEvents;
    }
    RUVIA_CHECK(expired && quicEvents > 0);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitDrainsReadWriteTimerOnStop) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    Http3QuicClientSocketSession::PumpResult blockedOutput;
    blockedOutput.outputBackpressured = true;
    blockedOutput.eventTimeout = std::chrono::seconds(1);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(blockedOutput)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session.requestStop();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kStopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitCloseJoinsAllPendingHandlers) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    Http3QuicClientSocketSession::PumpResult blockedOutput;
    blockedOutput.outputBackpressured = true;
    blockedOutput.eventTimeout = std::chrono::milliseconds(50);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(blockedOutput)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session.close();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kStopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitRejectsConcurrentCyclesWithoutLosingOwner) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    auto first = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity({})),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    auto duplicate = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity({})),
        asio::use_future);
    io.run_for(std::chrono::milliseconds(5));
    const bool duplicateReady =
        duplicate.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    RUVIA_CHECK(duplicateReady);
    session.requestStop();
    io.restart();
    io.run();
    RUVIA_CHECK(first.get() == Http3QuicClientSocketSession::WakeReason::kStopped);
    if (duplicateReady) {
        RUVIA_CHECK(duplicate.get() == Http3QuicClientSocketSession::WakeReason::kFatal);
    }
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionApplicationWakeIsLatchedBeforeArming) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    session.notifyWork();
    session.notifyWork();
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kApplication);
    RUVIA_CHECK(session.consumeWorkNotification());
    RUVIA_CHECK(!session.consumeWorkNotification());
    Http3QuicClientSocketSession::PumpResult pendingInput;
    pendingInput.inputBackpressured = true;
    pendingInput.eventTimeout = std::chrono::milliseconds(2);
    auto next = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(pendingInput)),
        asio::use_future);
    io.restart();
    io.run();
    RUVIA_CHECK(next.get() == Http3QuicClientSocketSession::WakeReason::kQuicEvent);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionApplicationWakeDrainsAllArmedHandlers) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    Http3QuicClientSocketSession::PumpResult pending;
    pending.eventTimeout = std::chrono::seconds(1);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(pending)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session.notifyWork();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kApplication);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    RUVIA_CHECK(session.consumeWorkNotification());
    RUVIA_CHECK(!session.consumeWorkNotification());
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionApplicationWakeCannotOverrideStop) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    Http3QuicClientSocketSession::PumpResult pending;
    pending.eventTimeout = std::chrono::seconds(1);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity(pending)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session.notifyWork();
    session.requestStop();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kStopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitColdDropAndCloseBeforeStart) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    {
        auto cold = session.waitForActivity({});
    }
    session.requestStop();
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kStopped);
    session.close();
}

RUVIA_TEST(http3QuicClientSocketSessionCloseWakesJoinedReadWait) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitReadable()), asio::use_future);
    RUVIA_CHECK(io.poll() != 0);
    RUVIA_CHECK(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    session.close();
    io.restart();
    io.run();
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { future.get(); }));
}
