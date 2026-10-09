#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory_resource>
#include <span>
#include <system_error>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "http3/http3_udp_socket.h"
#include "test_harness.h"

namespace {
using socket_type = ruvia::detail::http3_udp_socket;
using udp_type = asio::ip::udp;
using namespace std::chrono_literals;

[[noreturn]] void fixture_lifetime_violation(const char* fixture_value) noexcept {
    std::fprintf(stderr,
        "[FATAL FIXTURE LIFETIME] %s did not drain every borrowed completion; terminating\n",
        fixture_value);
    std::fflush(stderr);
    std::terminate();
}

struct exchange final {
    socket_type* server_{};
    udp_type::endpoint reply_source_;
    std::array<std::byte, 8> reply_bytes_{};
    std::size_t reply_size_{};
    std::array<std::byte, 64> request_bytes_{};
    std::size_t request_size_{};
    bool received_into_owned_storage_{};
    udp_type::endpoint received_peer_;
    udp_type::endpoint local_destination_;
    std::array<std::byte, 64> reply_buffer_{};
    udp_type::endpoint reply_peer_;
    int receive_calls_{};
    int send_calls_{};
    int reply_calls_{};
    int request_send_calls_{};
    std::error_code receive_error_;
    std::error_code send_error_;
    std::error_code reply_error_;
    std::error_code request_error_;
    std::size_t sent_size_{};
    std::size_t request_sent_size_{};
    std::size_t reply_received_size_{};
    bool send_accepted_{};
    bool request_sent_{};
};

void on_send(void* object, std::error_code error, std::size_t size) noexcept {
    auto& exchange_value = *static_cast<exchange*>(object);
    ++exchange_value.send_calls_;
    exchange_value.send_error_ = error;
    exchange_value.sent_size_ = size;
}

void on_receive(void* object, std::error_code error, socket_type::receive_view view) noexcept {
    auto& exchange_value = *static_cast<exchange*>(object);
    ++exchange_value.receive_calls_;
    exchange_value.receive_error_ = error;
    if (error) {
        return;
    }
    exchange_value.received_peer_ = std::move(view.peer_);
    exchange_value.local_destination_ = std::move(view.local_destination_);
    exchange_value.request_size_ = std::min(view.bytes_.size(), exchange_value.request_bytes_.size());
    exchange_value.received_into_owned_storage_ = view.bytes_.data() == exchange_value.request_bytes_.data();
    exchange_value.send_accepted_ = exchange_value.server_->async_send(
        socket_type::send_view{exchange_value.reply_source_, exchange_value.received_peer_,
            std::span<const std::byte>(exchange_value.reply_bytes_.data(), exchange_value.reply_size_)},
        &exchange_value, on_send);
}

exchange run_exchange(asio::io_context& io, socket_type& server, udp_type::socket& client,
    const udp_type::endpoint& destination, const udp_type::endpoint& reply_source,
    std::span<const std::byte> request) {
    if (io.stopped()) {
        io.restart();
    }
    exchange exchange;
    exchange.server_ = &server;
    exchange.reply_source_ = reply_source;
    exchange.reply_bytes_ = {std::byte{'r'}, std::byte{'e'}, std::byte{'p'},
        std::byte{'l'}, std::byte{'y'}, std::byte{'!'}, std::byte{}, std::byte{}};
    exchange.reply_size_ = 6;

    bool receive_accepted = false;
    bool client_receive_accepted = false;
    bool client_send_accepted = false;
    const auto cancel_and_drain = [&]() noexcept {
        server.request_stop();
        asio::error_code ignored;
        client.cancel(ignored);
        if (io.stopped()) {
            io.restart();
        }
        io.run_for(2s);
        if (io.stopped()) {
            io.restart();
        }
        (void)io.poll();
        const int expected_receive_calls = receive_accepted ? 1 : 0;
        const int expected_send_calls = exchange.send_accepted_ ? 1 : 0;
        const int expected_reply_calls = client_receive_accepted ? 1 : 0;
        const int expected_client_send_calls = client_send_accepted ? 1 : 0;
        if (!server.done() || exchange.receive_calls_ != expected_receive_calls ||
            exchange.send_calls_ != expected_send_calls || exchange.reply_calls_ != expected_reply_calls ||
            exchange.request_send_calls_ != expected_client_send_calls) {
            fixture_lifetime_violation("runExchange cancellation/drain");
        }
    };

    try {
        client.async_receive_from(asio::buffer(exchange.reply_buffer_), exchange.reply_peer_,
            [&exchange](const asio::error_code& error, std::size_t size) noexcept {
                ++exchange.reply_calls_;
                exchange.reply_error_ = error;
                exchange.reply_received_size_ = size;
            });
        client_receive_accepted = true;
        receive_accepted = server.async_receive(exchange.request_bytes_, &exchange, on_receive);
        client.async_send_to(asio::buffer(request), destination,
            [&exchange, expected_size = request.size()](const asio::error_code& error,
                std::size_t size) noexcept {
                ++exchange.request_send_calls_;
                exchange.request_error_ = error;
                exchange.request_sent_size_ = size;
                exchange.request_sent_ = !error && size == expected_size;
            });
        client_send_accepted = true;
        io.run_for(2s);
    } catch (...) {
        cancel_and_drain();
        throw;
    }

    const bool complete_value = receive_accepted && exchange.receive_calls_ == 1 &&
                                exchange.send_calls_ == (exchange.send_accepted_ ? 1 : 0) &&
                                exchange.reply_calls_ == 1 && exchange.request_send_calls_ == 1;
    if (!complete_value) {
        cancel_and_drain();
    }
    if (!receive_accepted) {
        exchange.receive_error_ = std::make_error_code(std::errc::operation_not_permitted);
    }
    return exchange;
}

bool is_operation_canceled(std::error_code error) noexcept {
    return error == asio::error::operation_aborted ||
           error == std::errc::operation_canceled;
}

bool is_i_pv6_unavailable(std::error_code error) noexcept {
    return error == asio::error::address_family_not_supported ||
           error == std::errc::protocol_not_supported ||
           error == asio::error::operation_not_supported ||
           error == std::errc::address_not_available ||
           error == std::errc::network_unreachable;
}

struct stop_receive final {
    socket_type* socket_{};
    int calls_{};
    std::error_code first_error_;
    std::error_code second_error_;
    std::size_t first_size_{};
    std::size_t second_size_{};
    bool rearm_{};
    bool rearm_accepted_{};
    bool post_stop_rearm_accepted_{};
    bool done_inside_completion_{};
    std::array<std::byte, 64> bytes_{};
};

struct send_completion_state final {
    int calls_{};
    std::error_code error_;
    std::size_t size_{};
};

struct datagram_send_state final {
    int calls_{};
    asio::error_code error_;
    std::size_t size_{};
};

void on_cancellation_send(void* object, std::error_code error, std::size_t size) noexcept {
    auto& state_value = *static_cast<send_completion_state*>(object);
    ++state_value.calls_;
    state_value.error_ = error;
    state_value.size_ = size;
}

void on_stop_receive(void* object, std::error_code error,
    socket_type::receive_view view) noexcept {
    auto& state_value = *static_cast<stop_receive*>(object);
    ++state_value.calls_;
    if (state_value.calls_ == 1) {
        state_value.first_error_ = error;
        state_value.first_size_ = view.bytes_.size();
        if (state_value.rearm_) {
            state_value.rearm_accepted_ = state_value.socket_->async_receive(state_value.bytes_, &state_value, on_stop_receive);
            state_value.socket_->request_stop();
            state_value.done_inside_completion_ = state_value.socket_->done();
            state_value.post_stop_rearm_accepted_ = state_value.socket_->async_receive(state_value.bytes_, &state_value, on_stop_receive);
        }
    } else {
        state_value.second_error_ = error;
        state_value.second_size_ = view.bytes_.size();
    }
}

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{};
    std::size_t allocations_{};
    std::size_t returns_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* const result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        ++allocations_;
        return result_value;
    }

    void do_deallocate(void* memory, std::size_t bytes_value, std::size_t alignment) override {
        live_bytes_ -= bytes_value;
        ++returns_;
        std::pmr::new_delete_resource()->deallocate(memory, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct send_round final {
    std::pmr::vector<std::byte> payload_;
    std::pmr::vector<std::byte>* retained_sent_{};
    std::pmr::vector<std::byte>* retained_received_{};
    bool payload_alive_{true};
    bool send_borrow_alive_{};
    int send_calls_{};
    int receive_calls_{};
    std::error_code send_error_;
    std::error_code receive_error_;
    std::size_t sent_size_{};
    std::size_t received_size_{};
    std::array<std::byte, 64> received_{};
    udp_type::endpoint sender_;

    explicit send_round(std::pmr::memory_resource* resource)
        : payload_(resource) {}
};

void on_round_send(void* object, std::error_code error, std::size_t size) noexcept {
    auto& round = *static_cast<send_round*>(object);
    ++round.send_calls_;
    round.send_error_ = error;
    round.sent_size_ = size;
    round.send_borrow_alive_ = round.payload_alive_;
    if (round.payload_alive_ && !error && !round.payload_.empty()) {
        round.retained_sent_->insert(round.retained_sent_->end(),
            round.payload_.begin(), round.payload_.end());
    }
}

}  // namespace

RUVIA_TEST(http3_network_udp_socket_preserves_pktinfo_and_explicit_reply_source) {
    asio::io_context io;
    socket_type server(io, udp_type::endpoint(asio::ip::address_v4::any(), 0));
    server.prepare();
    RUVIA_CHECK(server.bound_port() != 0);

    const auto loopback1 = asio::ip::address_v4::loopback();
    const auto loopback2 = asio::ip::address_v4({127, 0, 0, 2});
    udp_type::socket client1(io, udp_type::endpoint(loopback1, 0));
    udp_type::socket client2(io, udp_type::endpoint(loopback2, 0));
#ifdef _WIN32
    const udp_type::endpoint source_value(loopback1, server.bound_port());
#else
    const udp_type::endpoint source_value(loopback2, server.bound_port());
#endif
    const std::array<std::byte, 4> first_request{
        std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    const std::array<std::byte, 5> second_request{
        std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}};

    auto first = run_exchange(io, server, client1, udp_type::endpoint(loopback1, server.bound_port()),
        source_value, first_request);
    RUVIA_CHECK(first.request_sent_);
    RUVIA_CHECK(first.request_send_calls_ == 1);
    RUVIA_CHECK(!first.request_error_);
    RUVIA_CHECK(first.request_sent_size_ == first_request.size());
    RUVIA_CHECK(first.receive_calls_ == 1);
    RUVIA_CHECK(!first.receive_error_);
    RUVIA_CHECK(first.received_peer_ == client1.local_endpoint());
    RUVIA_CHECK(first.local_destination_ == udp_type::endpoint(loopback1, server.bound_port()));
    RUVIA_CHECK(first.received_into_owned_storage_);
    RUVIA_CHECK(first.request_size_ == first_request.size());
    RUVIA_CHECK(std::equal(first_request.begin(), first_request.end(), first.request_bytes_.begin()));
    RUVIA_CHECK(first.send_accepted_);
    RUVIA_CHECK(first.send_calls_ == 1);
    RUVIA_CHECK(!first.send_error_);
    RUVIA_CHECK(first.sent_size_ == first.reply_size_);
    RUVIA_CHECK(first.reply_calls_ == 1);
    RUVIA_CHECK(!first.reply_error_);
    RUVIA_CHECK(first.reply_received_size_ == first.reply_size_);
    RUVIA_CHECK(first.reply_peer_ == source_value);
    RUVIA_CHECK(std::equal(first.reply_bytes_.begin(),
        first.reply_bytes_.begin() + static_cast<std::ptrdiff_t>(first.reply_size_),
        first.reply_buffer_.begin()));
    if (first.receive_calls_ != 1 || first.send_calls_ != 1 || first.reply_calls_ != 1 ||
        first.request_send_calls_ != 1 || first.receive_error_ || first.send_error_ || first.reply_error_ ||
        first.request_error_) {
        return;
    }

    // Late replies to a departed peer must leave the shared receive side usable.
    client1.close();
    exchange closed_peer;
    const bool late_send = server.async_send(
        socket_type::send_view{source_value, first.received_peer_, first_request}, &closed_peer, on_send);
    RUVIA_CHECK(late_send);
    io.restart();
    io.run_for(50ms);
    RUVIA_CHECK_EQ(closed_peer.send_calls_, 1);
    RUVIA_CHECK(!closed_peer.send_error_);
    if (!late_send || closed_peer.send_calls_ != 1 || closed_peer.send_error_) {
        server.request_stop();
        io.restart();
        io.run_for(2s);
        if (!server.done()) {
            fixture_lifetime_violation("closed peer send");
        }
        return;
    }

    auto second = run_exchange(io, server, client2, udp_type::endpoint(loopback2, server.bound_port()),
        source_value, second_request);
    RUVIA_CHECK(second.request_sent_);
    RUVIA_CHECK(second.request_send_calls_ == 1);
    RUVIA_CHECK(!second.request_error_);
    RUVIA_CHECK(second.request_sent_size_ == second_request.size());
    RUVIA_CHECK(second.receive_calls_ == 1);
    RUVIA_CHECK(!second.receive_error_);
    RUVIA_CHECK(second.received_peer_ == client2.local_endpoint());
    RUVIA_CHECK(second.local_destination_ == udp_type::endpoint(loopback2, server.bound_port()));
    RUVIA_CHECK(second.received_into_owned_storage_);
    RUVIA_CHECK(second.request_size_ == second_request.size());
    RUVIA_CHECK(std::equal(second_request.begin(), second_request.end(), second.request_bytes_.begin()));
    RUVIA_CHECK(second.send_accepted_);
    RUVIA_CHECK(second.send_calls_ == 1);
    RUVIA_CHECK(!second.send_error_);
    RUVIA_CHECK(second.sent_size_ == second.reply_size_);
    RUVIA_CHECK(second.reply_calls_ == 1);
    RUVIA_CHECK(!second.reply_error_);
    RUVIA_CHECK(second.reply_received_size_ == second.reply_size_);
    RUVIA_CHECK(second.reply_peer_ == source_value);
    RUVIA_CHECK(std::equal(second.reply_bytes_.begin(),
        second.reply_bytes_.begin() + static_cast<std::ptrdiff_t>(second.reply_size_),
        second.reply_buffer_.begin()));
    if (second.receive_calls_ != 1 || second.send_calls_ != 1 || second.reply_calls_ != 1 ||
        second.request_send_calls_ != 1 || second.receive_error_ || second.send_error_ || second.reply_error_ ||
        second.request_error_) {
        return;
    }

    server.request_stop();
    RUVIA_CHECK(server.done());

    try {
        socket_type ipv6_server(io, udp_type::endpoint(asio::ip::address_v6::any(), 0));
        try {
            ipv6_server.prepare();
        } catch (const std::system_error& error) {
            if (is_i_pv6_unavailable(error.code())) {
                std::fprintf(stderr,
                    "[SKIP] IPv6 network (::1) unavailable during prepare: %s\n",
                    error.code().message().c_str());
            } else {
                std::fprintf(stderr, "IPv6 network prepare failed: %s\n",
                    error.code().message().c_str());
                RUVIA_CHECK(false && "unexpected IPv6 network prepare failure");
            }
            ipv6_server.request_stop();
            RUVIA_CHECK(ipv6_server.done());
            return;
        }

        udp_type::socket ipv6_client(io);
        asio::error_code ipv6_error;
        ipv6_client.open(udp_type::v6(), ipv6_error);
        if (!ipv6_error) {
            ipv6_client.bind(udp_type::endpoint(asio::ip::address_v6::loopback(), 0), ipv6_error);
        }
        if (ipv6_error) {
            if (is_i_pv6_unavailable(ipv6_error)) {
                std::fprintf(stderr,
                    "[SKIP] IPv6 network (::1) unavailable for client bind: %s\n",
                    ipv6_error.message().c_str());
            } else {
                std::fprintf(stderr, "IPv6 network client setup failed: %s\n",
                    ipv6_error.message().c_str());
                RUVIA_CHECK(false && "unexpected IPv6 network client setup failure");
            }
            ipv6_server.request_stop();
            RUVIA_CHECK(ipv6_server.done());
            return;
        }

        const auto ipv6_loopback = asio::ip::address_v6::loopback();
        const udp_type::endpoint ipv6_source(ipv6_loopback, ipv6_server.bound_port());
        const std::array<std::byte, 3> ipv6_request{
            std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
        auto ipv6 = run_exchange(io, ipv6_server, ipv6_client,
            udp_type::endpoint(ipv6_loopback, ipv6_server.bound_port()), ipv6_source, ipv6_request);
        RUVIA_CHECK(ipv6.request_sent_);
        RUVIA_CHECK(ipv6.request_send_calls_ == 1);
        RUVIA_CHECK(!ipv6.request_error_);
        RUVIA_CHECK(ipv6.request_sent_size_ == ipv6_request.size());
        RUVIA_CHECK(ipv6.receive_calls_ == 1);
        RUVIA_CHECK(!ipv6.receive_error_);
        RUVIA_CHECK(ipv6.received_peer_ == ipv6_client.local_endpoint());
        RUVIA_CHECK(ipv6.local_destination_ ==
                    udp_type::endpoint(ipv6_loopback, ipv6_server.bound_port()));
        RUVIA_CHECK(ipv6.received_into_owned_storage_);
        RUVIA_CHECK(ipv6.request_size_ == ipv6_request.size());
        RUVIA_CHECK(std::equal(ipv6_request.begin(), ipv6_request.end(), ipv6.request_bytes_.begin()));
        RUVIA_CHECK(ipv6.send_accepted_);
        RUVIA_CHECK(ipv6.send_calls_ == 1);
        RUVIA_CHECK(!ipv6.send_error_);
        RUVIA_CHECK(ipv6.sent_size_ == ipv6.reply_size_);
        RUVIA_CHECK(ipv6.reply_calls_ == 1);
        RUVIA_CHECK(!ipv6.reply_error_);
        RUVIA_CHECK(ipv6.reply_received_size_ == ipv6.reply_size_);
        RUVIA_CHECK(ipv6.reply_peer_ == ipv6_source);
        RUVIA_CHECK(std::equal(ipv6.reply_bytes_.begin(),
            ipv6.reply_bytes_.begin() + static_cast<std::ptrdiff_t>(ipv6.reply_size_),
            ipv6.reply_buffer_.begin()));
        ipv6_server.request_stop();
        RUVIA_CHECK(ipv6_server.done());
    } catch (const std::exception& error) {
        RUVIA_CHECK(false && "unexpected IPv6 network setup/runtime exception");
        std::fprintf(stderr, "IPv6 network exception: %s\n", error.what());
    }
}

RUVIA_TEST(http3_network_udp_socket_stop_drains_receive_completions) {
    constexpr int rounds = 12;
    asio::io_context io;
    const auto loopback = asio::ip::address_v4::loopback();
    {
        socket_type cold(io, udp_type::endpoint(loopback, 0));
        stop_receive cold_state{.socket_ = &cold, .first_error_ = {}, .second_error_ = {}};
        RUVIA_CHECK(!cold.async_receive(cold_state.bytes_, &cold_state, on_stop_receive));
        RUVIA_CHECK_EQ(cold_state.calls_, 0);
        cold.prepare();
        cold.request_stop();
        RUVIA_CHECK(cold.done());
    }

    for (int round = 0; round < rounds; ++round) {
        if (io.stopped()) {
            io.restart();
        }
        socket_type socket(io, udp_type::endpoint(loopback, 0));
        socket.prepare();
        stop_receive state_value{.socket_ = &socket, .first_error_ = {}, .second_error_ = {}};
        const bool accepted = socket.async_receive(state_value.bytes_, &state_value, on_stop_receive);
        RUVIA_CHECK(accepted);
        const bool duplicate_accepted = socket.async_receive(state_value.bytes_, &state_value, on_stop_receive);
        RUVIA_CHECK(!duplicate_accepted);
#ifndef _WIN32
        RUVIA_CHECK_EQ(io.poll_one(), 1U);
        RUVIA_CHECK_EQ(state_value.calls_, 0);
#endif
        socket.request_stop();
        RUVIA_CHECK(!socket.done());
        io.run_for(2s);
        if (!socket.done()) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2s);
        }
        RUVIA_CHECK(state_value.calls_ == 1);
        RUVIA_CHECK(is_operation_canceled(state_value.first_error_));
        RUVIA_CHECK(socket.done());
        if (!socket.done()) {
            fixture_lifetime_violation("pending receive stop");
        }
        if (state_value.calls_ != 1) {
            return;
        }
        if (io.stopped()) {
            io.restart();
        }
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }

    for (int round = 0; round < rounds; ++round) {
        if (io.stopped()) {
            io.restart();
        }
        socket_type socket(io, udp_type::endpoint(loopback, 0));
        socket.prepare();
        udp_type::socket sender(io, udp_type::endpoint(loopback, 0));
        stop_receive state_value{.socket_ = &socket, .first_error_ = {}, .second_error_ = {}, .rearm_ = true};
        const bool accepted = socket.async_receive(state_value.bytes_, &state_value, on_stop_receive);
        RUVIA_CHECK(accepted);
        const std::array<std::byte, 2> packet{std::byte{0x41}, std::byte{0x42}};
        datagram_send_state send_state;
        bool send_accepted = false;
        const auto cancel_and_drain = [&]() noexcept {
            socket.request_stop();
            asio::error_code ignored;
            sender.cancel(ignored);
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2s);
            if (io.stopped()) {
                io.restart();
            }
            (void)io.poll();
            const int expected_receive_calls = (accepted ? 1 : 0) +
                                               (state_value.rearm_accepted_ ? 1 : 0);
            const int expected_send_calls = send_accepted ? 1 : 0;
            if (!socket.done() || state_value.calls_ != expected_receive_calls ||
                send_state.calls_ != expected_send_calls) {
                fixture_lifetime_violation("receive completion/stop race cleanup");
            }
        };
        try {
            sender.async_send_to(asio::buffer(packet),
                udp_type::endpoint(loopback, socket.bound_port()),
                [&send_state](const asio::error_code& error, std::size_t size) noexcept {
                    ++send_state.calls_;
                    send_state.error_ = error;
                    send_state.size_ = size;
                });
            send_accepted = true;
            io.run_for(2s);
        } catch (...) {
            cancel_and_drain();
            throw;
        }
        if (!socket.done() || state_value.calls_ != 2 || send_state.calls_ != 1) {
            cancel_and_drain();
        }
        RUVIA_CHECK(state_value.calls_ == 2);
        RUVIA_CHECK(!send_state.error_);
        RUVIA_CHECK(send_state.size_ == packet.size());
        RUVIA_CHECK(!state_value.first_error_);
        RUVIA_CHECK(state_value.first_size_ == packet.size());
        RUVIA_CHECK(state_value.rearm_accepted_);
        RUVIA_CHECK(!state_value.done_inside_completion_);
        RUVIA_CHECK(!state_value.post_stop_rearm_accepted_);
        RUVIA_CHECK(is_operation_canceled(state_value.second_error_));
        RUVIA_CHECK(state_value.second_size_ == 0);
        RUVIA_CHECK(socket.done());
        if (!socket.done()) {
            fixture_lifetime_violation("receive completion/stop race");
        }
        if (state_value.calls_ != 2) {
            return;
        }
        if (io.stopped()) {
            io.restart();
        }
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }

    for (int round = 0; round < rounds; ++round) {
        if (io.stopped()) {
            io.restart();
        }
        socket_type socket(io, udp_type::endpoint(asio::ip::address_v4::any(), 0));
        socket.prepare();
        udp_type::socket peer(io, udp_type::endpoint(loopback, 0));
        send_completion_state state_value;
        const std::array<std::byte, 3> payload_value{
            std::byte{0x51}, std::byte{0x52}, std::byte{0x53}};
        const bool accepted = socket.async_send(
            socket_type::send_view{udp_type::endpoint(loopback, socket.bound_port()),
                peer.local_endpoint(), payload_value},
            &state_value, on_cancellation_send);
        RUVIA_CHECK(accepted);
        socket.request_stop();
        RUVIA_CHECK(!socket.done());
        io.run_for(2s);
        if (!socket.done()) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2s);
        }
        RUVIA_CHECK(state_value.calls_ == 1);
        RUVIA_CHECK(!state_value.error_ || is_operation_canceled(state_value.error_));
        RUVIA_CHECK(state_value.error_ ? state_value.size_ == 0 : state_value.size_ == payload_value.size());
        RUVIA_CHECK(socket.done());
        if (!socket.done()) {
            fixture_lifetime_violation("pending send cancellation");
        }
        if (state_value.calls_ != 1) {
            return;
        }
        if (io.stopped()) {
            io.restart();
        }
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }
}

RUVIA_TEST(http3_network_udp_socket_borrows_send_bytes_until_completion) {
    constexpr std::size_t rounds = 10;
    constexpr std::size_t payload_size = 37;
    asio::io_context io;
    const auto loopback = asio::ip::address_v4::loopback();
    socket_type server(io, udp_type::endpoint(asio::ip::address_v4::any(), 0));
    server.prepare();
    udp_type::socket peer(io, udp_type::endpoint(loopback, 0));
    const udp_type::endpoint explicit_source(loopback, server.bound_port());

    counting_resource resource;
    {
        std::pmr::vector<std::byte> retained_sent(&resource);
        std::pmr::vector<std::byte> retained_received(&resource);
        retained_sent.reserve(rounds * payload_size);
        retained_received.reserve(rounds * payload_size);
        const std::size_t retained_allocation_bytes = resource.live_bytes_;

        send_completion_state invalid_source_state;
        socket_type::send_view invalid_source{
            udp_type::endpoint(asio::ip::address_v4::any(), server.bound_port()),
            peer.local_endpoint(), {}};
        RUVIA_CHECK(!server.async_send(invalid_source, &invalid_source_state, on_cancellation_send));
        RUVIA_CHECK_EQ(invalid_source_state.calls_, 0);
        send_completion_state invalid_port_state;
        socket_type::send_view invalid_port{
            udp_type::endpoint(loopback, static_cast<std::uint16_t>(server.bound_port() + 1)),
            peer.local_endpoint(), {}};
        RUVIA_CHECK(!server.async_send(invalid_port, &invalid_port_state, on_cancellation_send));
        RUVIA_CHECK_EQ(invalid_port_state.calls_, 0);

        for (std::size_t round_index = 0; round_index < rounds; ++round_index) {
            if (io.stopped()) {
                io.restart();
            }
            {
                send_round round(&resource);
                round.retained_sent_ = &retained_sent;
                round.retained_received_ = &retained_received;
                round.payload_.resize(payload_size);
                for (std::size_t byte = 0; byte < round.payload_.size(); ++byte) {
                    round.payload_[byte] = static_cast<std::byte>(
                        (round_index * 19 + byte * 7) & 0xff);
                }

                peer.async_receive_from(asio::buffer(round.received_), round.sender_,
                    [&round](const asio::error_code& error, std::size_t size) noexcept {
                        ++round.receive_calls_;
                        round.receive_error_ = error;
                        round.received_size_ = size;
                        if (!error && size != 0) {
                            round.retained_received_->insert(round.retained_received_->end(),
                                round.received_.begin(),
                                round.received_.begin() + static_cast<std::ptrdiff_t>(size));
                        }
                    });
                const bool accepted = server.async_send(
                    socket_type::send_view{explicit_source, peer.local_endpoint(), round.payload_},
                    &round, on_round_send);
                RUVIA_CHECK(accepted);
                RUVIA_CHECK(round.send_calls_ == 0);
                RUVIA_CHECK(round.receive_calls_ == 0);
                io.run_for(2s);
                RUVIA_CHECK(round.send_calls_ == 1);
                RUVIA_CHECK(!round.send_error_);
                RUVIA_CHECK(round.send_borrow_alive_);
                RUVIA_CHECK(round.sent_size_ == payload_size);
                RUVIA_CHECK(round.receive_calls_ == 1);
                RUVIA_CHECK(!round.receive_error_);
                RUVIA_CHECK(round.received_size_ == payload_size);
                RUVIA_CHECK(round.sender_ == explicit_source);
                RUVIA_CHECK(std::equal(round.payload_.begin(), round.payload_.end(),
                    round.received_.begin()));
                if (round.send_calls_ != 1 || round.receive_calls_ != 1 ||
                    round.send_error_ || round.receive_error_) {
                    server.request_stop();
                    asio::error_code ignored;
                    peer.cancel(ignored);
                    if (io.stopped()) {
                        io.restart();
                    }
                    io.run_for(2s);
                    if (io.stopped()) {
                        io.restart();
                    }
                    (void)io.poll();
                    const int expected_send_calls = accepted ? 1 : 0;
                    if (!server.done() || round.send_calls_ != expected_send_calls ||
                        round.receive_calls_ != 1) {
                        fixture_lifetime_violation("SendRound timeout cleanup");
                    }
                    return;
                }
                round.payload_alive_ = false;
            }
            RUVIA_CHECK(resource.live_bytes_ == retained_allocation_bytes);
        }

        if (io.stopped()) {
            io.restart();
        }
        {
            send_round zero_length(&resource);
            zero_length.retained_sent_ = &retained_sent;
            zero_length.retained_received_ = &retained_received;
            peer.async_receive_from(asio::buffer(zero_length.received_), zero_length.sender_,
                [&zero_length](const asio::error_code& error, std::size_t size) noexcept {
                    ++zero_length.receive_calls_;
                    zero_length.receive_error_ = error;
                    zero_length.received_size_ = size;
                });
            const bool accepted = server.async_send(
                socket_type::send_view{explicit_source, peer.local_endpoint(), zero_length.payload_},
                &zero_length, on_round_send);
            RUVIA_CHECK(accepted);
            io.run_for(2s);
            RUVIA_CHECK(zero_length.send_calls_ == 1);
            RUVIA_CHECK(!zero_length.send_error_);
            RUVIA_CHECK(zero_length.send_borrow_alive_);
            RUVIA_CHECK_EQ(zero_length.sent_size_, 0U);
            RUVIA_CHECK(zero_length.receive_calls_ == 1);
            RUVIA_CHECK(!zero_length.receive_error_);
            RUVIA_CHECK_EQ(zero_length.received_size_, 0U);
            RUVIA_CHECK(zero_length.sender_ == explicit_source);
            if (zero_length.send_calls_ != 1 || zero_length.receive_calls_ != 1) {
                server.request_stop();
                asio::error_code ignored;
                peer.cancel(ignored);
                if (io.stopped()) {
                    io.restart();
                }
                io.run_for(2s);
                if (io.stopped()) {
                    io.restart();
                }
                (void)io.poll();
                if (!server.done() || zero_length.send_calls_ != (accepted ? 1 : 0) ||
                    zero_length.receive_calls_ != 1) {
                    fixture_lifetime_violation("zero-length SendRound timeout cleanup");
                }
                return;
            }
            zero_length.payload_alive_ = false;
        }
        RUVIA_CHECK(resource.live_bytes_ == retained_allocation_bytes);
        RUVIA_CHECK(retained_sent.size() == rounds * payload_size);
        RUVIA_CHECK(retained_received.size() == rounds * payload_size);
        for (std::size_t round_index = 0; round_index < rounds; ++round_index) {
            for (std::size_t byte = 0; byte < payload_size; ++byte) {
                const auto expected = static_cast<std::byte>((round_index * 19 + byte * 7) & 0xff);
                const auto index = round_index * payload_size + byte;
                RUVIA_CHECK(retained_sent[index] == expected);
                RUVIA_CHECK(retained_received[index] == expected);
            }
        }
        RUVIA_CHECK(resource.live_bytes_ == retained_allocation_bytes);
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, 0U);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
    server.request_stop();
    RUVIA_CHECK(server.done());
}
