#include <array>
#include <exception>
#include <optional>
#include <string>
#include <variant>

#include <asio/post.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/http_capsule_stream.h"
#include "ruvia/web/http_udp_tunnel.h"

#include "http/http_tunnel_session.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct capsule_transport_state {
    std::string input_;
    std::string output_;
    std::size_t offset_{};
    std::size_t block_{};
    unsigned fins_{};
    bool aborted_{};
};
struct capsule_transport {
    capsule_transport_state& state_;
    asio::io_context& io_;
    ruvia::task<ruvia::detail::http_stream_read_result> read_more(std::pmr::string& bytes_value) {
        (void)co_await ruvia::async_asio<void>([&](auto done) {
            asio::post(io_, [done = std::move(done)]() mutable { done(std::error_code{}); });
        });
        if (state_.aborted_) {
            co_return ruvia::detail::http_stream_read_result::make_failure(std::make_error_code(std::errc::operation_canceled));
        }
        if (state_.offset_ == state_.input_.size()) {
            co_return ruvia::detail::http_stream_read_result::make_end();
        }
        const auto count = std::min(state_.block_, state_.input_.size() - state_.offset_);
        bytes_value.append(state_.input_.data() + state_.offset_, count);
        state_.offset_ += count;
        co_return ruvia::detail::http_stream_read_result::make_data();
    }
    ruvia::task<std::error_code> write_bytes(std::string_view bytes_value, ruvia::detail::http_stream_end end) {
        if (state_.aborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        state_.output_.append(bytes_value);
        state_.fins_ += end == ruvia::detail::http_stream_end::end;
        co_return std::error_code{};
    }
    void abort() noexcept {
        state_.aborted_ = true;
    }
};
std::string capsule_wire(std::uint64_t type, std::string_view payload_value) {
    std::array<char, 16> header;
    const auto encoded = ruvia::encode_http_capsule_header(header, type, payload_value.size());
    std::string bytes_value(header.data(), std::get<0>(encoded));
    bytes_value.append(payload_value);
    return bytes_value;
}
}  // namespace
RUVIA_TEST(http_capsule_stream_owns_cold_inputs_retained_results_and_split_or_coalesced_capsules) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource memory;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        try {
            for (std::size_t block : {std::size_t{1}, std::size_t{65536}}) {
                const std::string payload_value(20003, 'c');
                capsule_transport_state wire{.input_ = capsule_wire(0x123456789ULL, payload_value) + capsule_wire(0, "") + capsule_wire(7, "last"), .block_ = block};
                ruvia::detail::http_tunnel_session session_value(capsule_transport{wire, io}, worker_value, memory);
                std::optional<ruvia::http_capsule> retained;
                {
                    auto capsules = session_value.tunnel().capsules();
                    {
                        auto cold = capsules.write(10, "discarded");
                    }
                    std::string input(10003, 'w');
                    auto output = capsules.write(11, input);
                    input.assign("mutated");
                    bool busy{};
                    try {
                        (void)capsules.finish();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    auto first = capsules.read();
                    auto moved = std::move(capsules);
                    co_await std::move(output);
                    retained = co_await std::move(first);
                    RUVIA_CHECK(retained && retained->type() == 0x123456789ULL && retained->payload() == payload_value);
                    auto empty = co_await moved.read();
                    RUVIA_CHECK(empty && empty->type() == 0 && empty->payload().empty());
                    auto last = co_await moved.read();
                    RUVIA_CHECK(last && last->type() == 7 && last->payload() == "last");
                    RUVIA_CHECK(!(co_await moved.read()));
                    co_await moved.finish();
                    co_await moved.finish();
                    RUVIA_CHECK(wire.output_ == capsule_wire(11, std::string(10003, 'w')));
                    RUVIA_CHECK_EQ(wire.fins_, 1U);
                }
                RUVIA_CHECK(retained->payload() == payload_value);
                RUVIA_CHECK(!wire.aborted_);
                retained.reset();
                co_await session_value.join();
            }
            for (bool oversized : {false, true}) {
                auto bytes_value = capsule_wire(0, oversized ? "123456789" : "1234");
                if (!oversized) {
                    bytes_value.pop_back();
                }
                capsule_transport_state wire{.input_ = bytes_value, .block_ = 1};
                ruvia::detail::http_tunnel_session session_value(capsule_transport{wire, io}, worker_value, memory);
                auto capsules = session_value.tunnel().capsules({.max_capsule_length_ = 8});
                bool rejected{};
                try {
                    (void)co_await capsules.read();
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                RUVIA_CHECK(rejected && wire.aborted_);
                co_await session_value.join();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_udp_tunnel_ignores_unknown_capsules_and_contexts_preserves_empty_packets_and_owned_data) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource memory;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        try {
            const std::string payload_value(65527, 'u');
            capsule_transport_state wire{.input_ = capsule_wire(987, "unknown capsule") + capsule_wire(0, std::string(1, char(9)) + "unknown context") + capsule_wire(0, std::string(1, char(0)) + payload_value) + capsule_wire(0, std::string(1, char(0))), .block_ = 65536};
            for (unsigned i = 0; i != 20000; ++i) {
                wire.input_.insert(0, capsule_wire(63, ""));
            }
            ruvia::detail::http_tunnel_session session_value(capsule_transport{wire, io}, worker_value, memory);
            std::optional<ruvia::http_udp_datagram> retained;
            {
                ruvia::http_udp_tunnel udp(session_value.tunnel().capsules());
                retained = co_await udp.read();
                RUVIA_CHECK(retained && std::string_view(reinterpret_cast<const char*>(retained->payload().data()), retained->payload().size()) == payload_value);
                auto empty = co_await udp.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await udp.read()));
                std::string bytes_value = payload_value;
                auto write = udp.send(bytes_value);
                bytes_value.assign("mutated");
                auto moved = std::move(udp);
                co_await std::move(write);
                co_await moved.send("");
                bool rejected{};
                try {
                    (void)moved.send(std::string(65528, 'x'));
                } catch (const std::length_error&) {
                    rejected = true;
                }
                RUVIA_CHECK(rejected);
                co_await moved.finish();
                RUVIA_CHECK(wire.output_ == capsule_wire(0, std::string(1, char(0)) + payload_value) + capsule_wire(0, std::string(1, char(0))));
            }
            RUVIA_CHECK(retained->payload().size() == payload_value.size());
            retained.reset();
            co_await session_value.join();
            capsule_transport_state malformed{.input_ = capsule_wire(0, std::string(1, char(0x40))), .block_ = 4096};
            ruvia::detail::http_tunnel_session failed_session(capsule_transport{malformed, io}, worker_value, memory);
            ruvia::http_udp_tunnel udp(failed_session.tunnel().capsules());
            bool rejected{};
            try {
                (void)co_await udp.read();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected && malformed.aborted_);
            co_await failed_session.join();
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

namespace {
struct mixed_datagram_state final {
    std::array<std::string, 3> input_;
    std::string stream_output_;
    std::string native_output_;
    std::size_t next_{};
    bool aborted_{};
};
struct mixed_datagram_transport final {
    mixed_datagram_state& state_;
    asio::io_context& io_;
    std::pmr::memory_resource* resource_;
    ruvia::task<std::optional<ruvia::detail::http_datagram_input>> read_datagram_input() {
        (void)co_await ruvia::async_asio<void>([&](auto done) { asio::post(io_, [done = std::move(done)]() mutable { done(std::error_code{}); }); });
        if (state_.aborted_) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        if (state_.next_ == state_.input_.size()) {
            co_return std::nullopt;
        }
        const auto next_value = state_.next_++;
        co_return ruvia::detail::http_datagram_input{std::pmr::string(state_.input_[next_value], resource_), next_value == 1};
    }
    ruvia::http_datagram_session_config datagram_config() const {
        return {.http3_stream_id_ = 4, .local_h3_datagram_ = true, .peer_h3_datagram_ = true, .quic_datagram_ = true, .max_quic_payload_bytes_ = 16};
    }
    void send_datagram(std::span<const std::byte> bytes_value) {
        state_.native_output_.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
    }
    ruvia::task<ruvia::detail::http_stream_read_result> read_more(std::pmr::string&) {
        co_return ruvia::detail::http_stream_read_result::make_end();
    }
    ruvia::task<std::error_code> write_bytes(std::string_view bytes_value, ruvia::detail::http_stream_end) {
        state_.stream_output_.append(bytes_value);
        co_return std::error_code{};
    }
    void abort() noexcept {
        state_.aborted_ = true;
    }
};
}  // namespace
RUVIA_TEST(http_datagram_stream_preserves_partial_capsules_when_native_packets_arrive_and_selects_send_transport) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource memory;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            auto wire = capsule_wire(0, "abcde");
            mixed_datagram_state mixed{.input_ = {wire.substr(0, 4), std::string(1, '\1') + "xyz", wire.substr(4) + capsule_wire(17, "ignored") + capsule_wire(0, "")}};
            ruvia::detail::http_tunnel_session session_value(mixed_datagram_transport{mixed, io, &memory}, worker_value, memory);
            std::optional<ruvia::http_datagram> retained;
            {
                auto stream = session_value.tunnel().datagrams();
                retained = co_await stream.read();
                RUVIA_CHECK(retained && retained->transport() == ruvia::http_datagram_transport::quic);
                RUVIA_CHECK(retained && retained->payload().size() == 3 && retained->payload()[0] == std::byte{'x'});
                auto reliable = co_await stream.read();
                RUVIA_CHECK(reliable && reliable->transport() == ruvia::http_datagram_transport::capsule);
                RUVIA_CHECK(reliable && reliable->payload().size() == 5 && reliable->payload()[4] == std::byte{'e'});
                auto empty = co_await stream.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await stream.read()));
                std::string value = "hello";
                auto cold = stream.send(value);
                value.assign("changed");
                co_await std::move(cold);
                RUVIA_CHECK(mixed.native_output_ == std::string(1, '\1') + "hello");
                co_await stream.send(std::string(20, 'p'));
                RUVIA_CHECK(mixed.stream_output_ == capsule_wire(0, std::string(20, 'p')));
                co_await stream.finish();
                RUVIA_CHECK(ruvia::testing::throws_on([&] { static_cast<void>(stream.send("closed")); }));
            }
            RUVIA_CHECK(retained && retained->payload().size() == 3);
            retained.reset();
            co_await session_value.join();
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
    RUVIA_CHECK_EQ(memory.live_allocations(), 0U);
    if (failure) {
        std::rethrow_exception(failure);
    }
}
