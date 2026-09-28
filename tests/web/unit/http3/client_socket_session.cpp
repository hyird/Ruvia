#include <array>
#include <chrono>
#include <future>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/use_future.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/AsioTask.h"
#include "ruvia/web/detail/http3/Http3QuicClientSocketSession.h"
#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"

#include "test_harness.h"

RUVIA_TEST(http3QuicClientSocketSessionOwnsConcreteConnectedSocket) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionWriteWaitCompletesWhenSocketIsReady) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitWritable()), asio::use_future);
    io.run();
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] { future.get(); }));
    session.close();
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitWakesOnReadableDatagram) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    const std::array<char, 1> datagram{'x'};
    (void)peerSocket.send_to(asio::buffer(datagram), session.localEndpoint());
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitForActivity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == Http3QuicClientSocketSession::WakeReason::kReadable);
    session.close();
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitRetriesFullInputViaQuicTimer) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitDrainsReadAndTimerAfterWritable) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitPreservesAbsoluteDeadlineAcrossTicks) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitDrainsReadWriteTimerOnStop) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitCloseJoinsAllPendingHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitRejectsConcurrentCyclesWithoutLosingOwner) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionApplicationWakeIsLatchedBeforeArming) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionApplicationWakeDrainsAllArmedHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionApplicationWakeCannotOverrideStop) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionActivityWaitColdDropAndCloseBeforeStart) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
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
#endif
}

RUVIA_TEST(http3QuicClientSocketSessionCloseWakesJoinedReadWait) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peerSocket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
    Http3QuicClientSocketSession session(io, peerSocket.local_endpoint(), "localhost", tls);
    auto future = asio::co_spawn(io, ruvia::asAwaitable(session.waitReadable()), asio::use_future);
    RUVIA_CHECK(io.poll() != 0);
    RUVIA_CHECK(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    session.close();
    io.restart();
    io.run();
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { future.get(); }));
#endif
}
