#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/error.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {
using namespace std::chrono_literals;
using udp_type = asio::ip::udp;

class quic_udp_blackhole final {
public:
    explicit quic_udp_blackhole(asio::io_context& io)
        : socket_(io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0)),
          endpoint_(socket_.local_endpoint()) {
        socket_.non_blocking(true);
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }

    [[nodiscard]] std::size_t datagrams() const noexcept {
        return datagrams_;
    }

    [[nodiscard]] std::size_t quic_long_headers() const noexcept {
        return quic_long_headers_;
    }

    [[nodiscard]] std::span<const udp_type::endpoint> sources() const noexcept {
        return sources_;
    }

    void receive_available() {
        std::array<std::byte, 65536> packet{};
        for (;;) {
            udp_type::endpoint source;
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
                ++quic_long_headers_;
            }
            if (std::find(sources_.begin(), sources_.end(), source) == sources_.end()) {
                sources_.push_back(source);
            }
        }
    }

private:
    udp_type::socket socket_;
    udp_type::endpoint endpoint_;
    std::vector<udp_type::endpoint> sources_;
    std::size_t datagrams_{};
    std::size_t quic_long_headers_{};
};

struct send_result final {
    bool finished_{};
    bool returned_response_{};
    std::optional<ruvia::http_client_error::code_type> error_{};
};

ruvia::task<void> send_and_capture(ruvia::http_client_handle handle,
    const ruvia::http_client_request_view& request, send_result& result_value) {
    try {
        auto response = co_await handle.send(request);
        result_value.returned_response_ = true;
        static_cast<void>(response);
    } catch (const ruvia::http_client_error& error) {
        result_value.error_ = error.code();
    }
    result_value.finished_ = true;
}

ruvia::task<bool> wait_for_datagrams(const ruvia::worker_handle& worker_value,
    quic_udp_blackhole& peer, std::size_t target, std::chrono::milliseconds timeout) {
    const auto deadline_value = std::chrono::steady_clock::now() + timeout;
    while (peer.datagrams() < target) {
        peer.receive_available();
        if (peer.datagrams() >= target) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline_value) {
            co_return false;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    peer.receive_available();
    co_return peer.datagrams() >= target;
}

ruvia::task<bool> wait_for_completion(const ruvia::worker_handle& worker_value,
    quic_udp_blackhole& peer, const send_result& result_value, std::chrono::milliseconds timeout) {
    const auto deadline_value = std::chrono::steady_clock::now() + timeout;
    while (!result_value.finished_) {
        peer.receive_available();
        if (std::chrono::steady_clock::now() >= deadline_value) {
            co_return false;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    peer.receive_available();
    co_return true;
}

ruvia::task<void> exercise_cross_thread_constructed_http3_client(
    ruvia::event_loop_attachment& attachment, quic_udp_blackhole& peer,
    ruvia::http_client& client, send_result& result_value) {
    try {
        const auto worker_value = attachment.loop().handle();
        ruvia::stop_source stop;
        const ruvia::http_client_request_view request{
            .method_ = "GET", .target_ = "/cross-thread-owner"};
        {
            ruvia::task_scope task(worker_value);
            task.spawn(send_and_capture(client.with_options(
                                            {.timeout_ = 5s, .stop_token_ = stop.token()}),
                request, result_value));
            const bool dispatched = co_await wait_for_datagrams(worker_value, peer, 1, 2s);
            if (dispatched) {
                stop.request_stop();
                (void)co_await wait_for_completion(worker_value, peer, result_value, 2s);
            }
            task.request_stop();
            co_await task.join();
        }
        co_await client.shutdown();
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_public_http3_dispatch(
    ruvia::event_loop_attachment& attachment, quic_udp_blackhole& peer,
    ruvia::testing::test_context& ruvia_ctx) {
    try {
        const auto worker_value = attachment.loop().handle();
        ruvia::http_client client(attachment.loop(), ruvia::http_client_config{
                                                         .scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 2,
                                                         .connect_timeout_ = 2s,
                                                         .request_timeout_ = 5s,
                                                         .acquire_timeout_ = 1s,
                                                         .max_response_bytes_ = 4096,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
                                                     });

        ruvia::stop_source first_stop;
        ruvia::stop_source second_stop;
        const auto first_handle = client.with_options(
            {.timeout_ = 5s, .stop_token_ = first_stop.token()});
        const auto second_handle = client.with_options(
            {.timeout_ = 5s, .stop_token_ = second_stop.token()});
        const ruvia::http_client_request_view first_request{.method_ = "GET", .target_ = "/slot-one"};
        const ruvia::http_client_request_view second_request{.method_ = "GET", .target_ = "/slot-two"};
        send_result first_result;
        send_result second_result;
        {
            ruvia::task_scope requests(worker_value);
            requests.spawn(send_and_capture(first_handle, first_request, first_result));
            requests.spawn(send_and_capture(second_handle, second_request, second_result));
            const auto source_deadline = std::chrono::steady_clock::now() + 2s;
            while (peer.sources().size() < 2 &&
                   std::chrono::steady_clock::now() < source_deadline) {
                peer.receive_available();
                if (peer.sources().size() < 2) {
                    (void)co_await ruvia::sleep_for(worker_value, 1ms);
                }
            }
            peer.receive_available();
            RUVIA_CHECK(peer.sources().size() >= 2);
            RUVIA_CHECK(peer.quic_long_headers() >= 2);

            first_stop.request_stop();
            second_stop.request_stop();
            const bool first_finished = co_await wait_for_completion(
                worker_value, peer, first_result, 2s);
            const bool second_finished = co_await wait_for_completion(
                worker_value, peer, second_result, 2s);
            RUVIA_CHECK(first_finished && second_finished);
            co_await requests.join();
        }
        RUVIA_CHECK(first_result.finished_ && second_result.finished_);
        RUVIA_CHECK(first_result.error_ == ruvia::http_client_error::code_type::cancelled);
        RUVIA_CHECK(second_result.error_ == ruvia::http_client_error::code_type::cancelled);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});

        peer.receive_available();
        const auto deadline_datagram_count = peer.datagrams() + 1;
        send_result deadline_result;
        ruvia::stop_source deadline_safety_stop;
        const ruvia::http_client_request_view deadline_request{
            .method_ = "GET", .target_ = "/deadline"};
        {
            ruvia::task_scope request(worker_value);
            request.spawn(send_and_capture(client.with_options(
                                               {.timeout_ = 150ms, .stop_token_ = deadline_safety_stop.token()}),
                deadline_request, deadline_result));
            const bool dispatched = co_await wait_for_datagrams(
                worker_value, peer, deadline_datagram_count, 1s);
            RUVIA_CHECK(dispatched);
            const bool finished = co_await wait_for_completion(
                worker_value, peer, deadline_result, 2s);
            RUVIA_CHECK(finished);
            if (!deadline_result.finished_) {
                deadline_safety_stop.request_stop();
                (void)co_await wait_for_completion(
                    worker_value, peer, deadline_result, 1s);
            }
            co_await request.join();
        }
        RUVIA_CHECK(deadline_result.error_ == ruvia::http_client_error::code_type::timeout);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});

        peer.receive_available();
        const auto rotated_datagram_count = peer.datagrams() + 1;
        ruvia::stop_source rotation_stop;
        send_result rotation_result;
        const ruvia::http_client_request_view rotation_request{
            .method_ = "GET", .target_ = "/rotated-connection"};
        {
            ruvia::task_scope request(worker_value);
            request.spawn(send_and_capture(client.with_options(
                                               {.timeout_ = 5s, .stop_token_ = rotation_stop.token()}),
                rotation_request, rotation_result));
            const bool rotated = co_await wait_for_datagrams(
                worker_value, peer, rotated_datagram_count, 2s);
            RUVIA_CHECK(rotated);
            rotation_stop.request_stop();
            const bool finished = co_await wait_for_completion(
                worker_value, peer, rotation_result, 2s);
            RUVIA_CHECK(finished);
            co_await request.join();
        }
        RUVIA_CHECK(rotation_result.error_ == ruvia::http_client_error::code_type::cancelled);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});

        co_await client.shutdown();
        peer.receive_available();
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

}  // namespace

RUVIA_TEST(http3_public_http_client_constructed_before_worker_launch_binds_quic_owner_on_worker) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    quic_udp_blackhole peer(io);
    ruvia::http_client client(attachment.loop(), ruvia::http_client_config{
                                                     .scheme_ = ruvia::http_scheme::https,
                                                     .host_ = "127.0.0.1",
                                                     .port_ = peer.port(),
                                                     .connection_count_ = 1,
                                                     .connect_timeout_ = 2s,
                                                     .request_timeout_ = 5s,
                                                     .acquire_timeout_ = 1s,
                                                     .max_response_bytes_ = 4096,
                                                     .protocol_ = ruvia::http_client_protocol::http3_only,
                                                     .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
                                                 });
    send_result result;
    auto root = attachment.loop().start(
        exercise_cross_thread_constructed_http3_client(attachment, peer, client, result));
    std::thread worker_value([&attachment] { attachment.run(); });
    worker_value.join();
    root.get();
    RUVIA_CHECK(result.finished_);
    RUVIA_CHECK(result.error_ == ruvia::http_client_error::code_type::cancelled);
    RUVIA_CHECK(peer.quic_long_headers() >= 1);
}

RUVIA_TEST(http3_public_http_client_handle_dispatches_udp_and_maps_pool_slots_to_connections) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    quic_udp_blackhole peer(io);
    auto root = attachment.loop().start(
        exercise_public_http3_dispatch(attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}
