#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <future>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/async.h"
#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_server_protocol.h"

#include "server/inbound_buffer_resource.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "websocket/http_websocket_session.h"
#include "websocket/http_websocket_socket_transport.h"

namespace {

using asio::ip::tcp;
using ruvia::connection_scanner;
using ruvia::websocket_compression;
using ruvia::websocket_opcode;
using ruvia::websocket_transport_disposition;
using ruvia::detail::socket_websocket_connection_type;
using ruvia::detail::websocket_connection;
using ruvia::detail::websocket_socket_transport;

struct recording_transport_state final {
    bool aborted_{false};
    std::size_t writes_{0};
    std::error_code read_error_;
    std::string last_non_empty_bytes_;
    websocket_transport_disposition last_disposition_{websocket_transport_disposition::keep_open};
    bool suspend_next_read_{false};
    bool suspend_next_write_{false};
    std::function<void()> complete_read_;
    std::function<void()> complete_write_;
    void* before_write_completion_target_{nullptr};
    void (*before_write_completion_)(void*) noexcept {nullptr};
};

class recording_transport final {
public:
    recording_transport(asio::io_context& io, recording_transport_state& state_value) noexcept
        : io_(&io),
          state_(&state_value) {}

    [[nodiscard]] auto executor() const noexcept {
        return io_->get_executor();
    }

    [[nodiscard]] ruvia::task<ruvia::detail::http_stream_read_result> read_more(std::pmr::string&) {
        if (std::exchange(state_->suspend_next_read_, false)) {
            static_cast<void>(co_await ruvia::async_asio<void>([state = state_](auto completion) mutable {
                state->complete_read_ = [completion = std::move(completion)]() mutable {
                    completion(std::error_code{});
                };
            }));
        }
        if (state_->read_error_) {
            co_return ruvia::detail::http_stream_read_result::make_failure(state_->read_error_);
        }
        co_return ruvia::detail::http_stream_read_result::make_end();
    }

    [[nodiscard]] ruvia::task<std::error_code> write_bytes(
        std::string_view bytes_value, websocket_transport_disposition disposition) {
        ++state_->writes_;
        if (!bytes_value.empty()) {
            state_->last_non_empty_bytes_.assign(bytes_value);
        }
        state_->last_disposition_ = disposition;
        if (state_->before_write_completion_ != nullptr) {
            const auto callback_value = std::exchange(state_->before_write_completion_, nullptr);
            callback_value(state_->before_write_completion_target_);
        }
        if (std::exchange(state_->suspend_next_write_, false)) {
            static_cast<void>(
                co_await ruvia::async_asio<void>([state = state_](auto completion) mutable {
                    state->complete_write_ = [completion = std::move(completion)]() mutable {
                        completion(std::error_code{});
                    };
                }));
        }
        co_return std::error_code{};
    }

    void abort() noexcept {
        state_->aborted_ = true;
        if (state_->complete_read_ != nullptr) {
            auto completion = std::exchange(state_->complete_read_, std::function<void()>{});
            completion();
        }
        if (state_->complete_write_ != nullptr) {
            auto completion = std::exchange(state_->complete_write_, std::function<void()>{});
            completion();
        }
    }

private:
    asio::io_context* io_;
    recording_transport_state* state_;
};

std::string masked_frame(
    std::uint8_t opcode, std::string_view payload_value, bool fin = true, bool rsv1 = false) {
    std::string frame;
    frame.reserve(payload_value.size() + 6);
    frame.push_back(static_cast<char>((fin ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | opcode));
    frame.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload_value.size())));
    constexpr std::array<unsigned char, 4> mask{0x11, 0x22, 0x33, 0x44};
    frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        frame.push_back(
            static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ mask[i % mask.size()]));
    }
    return frame;
}

asio::awaitable<std::string> read_short_server_frame(tcp::socket& socket) {
    std::array<char, 2> head{};
    co_await asio::async_read(socket, asio::buffer(head), asio::use_awaitable);
    const auto size = static_cast<std::size_t>(static_cast<unsigned char>(head[1]) & 0x7FU);
    if (size >= 126) {
        co_return std::string{};
    }
    std::string frame(head.data(), head.size());
    frame.resize(2 + size);
    if (size != 0) {
        co_await asio::async_read(
            socket, asio::buffer(frame.data() + 2, size), asio::use_awaitable);
    }
    co_return frame;
}

template <typename... results_type>
void run_until_ready(asio::io_context& io, std::future<results_type>&... futures) {
    static_assert(sizeof...(results_type) > 0);
    while (((futures.wait_for(std::chrono::seconds(0)) != std::future_status::ready) || ...)) {
        io.run_one();
    }
}

}  // namespace

RUVIA_TEST(websocket_teardown_aborts_and_joins_suspended_reader) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    state.suspend_next_read_ = true;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    auto reader_value = asio::co_spawn(io, [&]() -> asio::awaitable<std::optional<ruvia::websocket_message>> { co_return co_await ruvia::as_awaitable(connection.read()); }, asio::use_future);
    auto teardown = asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await ruvia::as_awaitable(connection.detach_and_drain_writes()); }, asio::use_future);
    run_until_ready(io, reader_value, teardown);

    RUVIA_CHECK(state.aborted_);
    RUVIA_CHECK(!state.complete_read_);
    RUVIA_CHECK(!reader_value.get().has_value());
    teardown.get();
}

RUVIA_TEST(websocket_drain_waits_for_a_cold_read_reservation_to_be_released) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());
    bool drained = false;
    bool reservation_released = false;
    bool drained_before_reservation_release = false;
    std::optional<ruvia::task<std::optional<ruvia::websocket_message>>> cold;

    auto test = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        cold.emplace(connection.read());
        asio::steady_timer release(io);
        release.expires_after(std::chrono::milliseconds(1));
        release.async_wait([&](const std::error_code&) {
            cold.reset();
            reservation_released = true;
        });
        co_await ruvia::as_awaitable(connection.detach_and_drain_writes());
        drained_before_reservation_release = !reservation_released;
        drained = true; }, asio::use_future);
    run_until_ready(io, test);
    test.get();

    RUVIA_CHECK(drained);
    RUVIA_CHECK(!drained_before_reservation_release);
    RUVIA_CHECK(reservation_released);
}

RUVIA_TEST(websocket_transport_read_failure_preserves_error_and_aborts) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    state.read_error_ = std::make_error_code(std::errc::connection_reset);
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());
    std::error_code observed;

    auto reader_value = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await ruvia::as_awaitable(connection.read());
            } catch (const std::system_error& error) {
                observed = error.code();
            }
        },
        asio::use_future);
    run_until_ready(io, reader_value);
    reader_value.get();

    RUVIA_CHECK_EQ(observed, state.read_error_);
    RUVIA_CHECK(state.aborted_);
}

RUVIA_TEST(websocket_read_reservation_rejects_cold_overlap_and_releases) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    bool rejected = false;
    bool close_rejected = false;
    auto reserve = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        auto cold = connection.read();
        try {
            auto overlapping = connection.read();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        try {
            auto closing = connection.close();
            static_cast<void>(closing);
        } catch (const std::logic_error&) {
            close_rejected = true;
        }
        co_return; }, asio::use_future);
    run_until_ready(io, reserve);
    reserve.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(close_rejected);

    io.restart();
    auto following = asio::co_spawn(io, [&]() -> asio::awaitable<std::optional<ruvia::websocket_message>> { co_return co_await ruvia::as_awaitable(connection.read()); }, asio::use_future);
    run_until_ready(io, following);
    const auto message = following.get();
    RUVIA_CHECK(!message.has_value());
    RUVIA_CHECK(!state.aborted_);
}

RUVIA_TEST(websocket_session_finish_maps_chain_failure_to_1011) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    // The 1011 close code is all the peer learns; the listener is where the
    // reason survives an already-upgraded connection.
    struct failure_observation final {
        std::size_t calls_{0};
        std::string message_;

        void operator()(const ruvia::connection_failure_record& record) noexcept {
            ++calls_;
            try {
                std::rethrow_exception(record.exception());
            } catch (const std::exception& error) {
                message_.assign(error.what());
            } catch (...) {
                message_.assign("<unknown>");
            }
        }
    } observation;
    ruvia::detail::connection_failure_sink connection_failure;
    connection_failure.callback_ =
        ruvia::detail::callback_access::bind<void(const ruvia::connection_failure_record&) noexcept>(
            observation);

    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(ruvia::detail::finish_websocket_session(connection,
            std::make_exception_ptr(std::runtime_error("middleware post failed")),
            connection_failure, "127.0.0.1")),
        asio::use_future);
    run_until_ready(io, future);
    future.get();

    RUVIA_CHECK_EQ(observation.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(observation.message_, std::string("middleware post failed"));
    RUVIA_CHECK_EQ(state.writes_, std::size_t{2});
    RUVIA_CHECK(state.last_non_empty_bytes_.size() >= 4);
    if (state.last_non_empty_bytes_.size() >= 4) {
        const auto high = static_cast<unsigned char>(state.last_non_empty_bytes_[2]);
        const auto low = static_cast<unsigned char>(state.last_non_empty_bytes_[3]);
        RUVIA_CHECK_EQ(static_cast<std::uint16_t>((high << 8U) | low), std::uint16_t{1011});
    }
    RUVIA_CHECK(state.last_disposition_ == websocket_transport_disposition::end_transport);
    RUVIA_CHECK(!state.aborted_);
}

// Periodic liveness failure aborts the websocket transport itself. The scanner
// callback has no connection-close return channel; for an RFC 8441 adapter abort
// means RST_STREAM(CANCEL), so one silent tunnel cannot tear down unrelated streams.
RUVIA_TEST(websocket_liveness_aborts_transport_not_scanner_owner) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    ruvia::websocket_lifecycle_options lifecycle;
    lifecycle.heartbeat_ = {
        .ping_interval_ = std::chrono::milliseconds(1),
        .pong_timeout_ = std::chrono::milliseconds(1),
    };
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, lifecycle, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    asio::post(io, [&connection] {
        websocket_connection<recording_transport>::heartbeat_tick_thunk(&connection, 10);
    });
    (void)io.poll();
    RUVIA_CHECK_EQ(state.writes_, std::size_t{1});
    RUVIA_CHECK(state.last_disposition_ == websocket_transport_disposition::keep_open);

    // No Pong arrived and its deadline elapsed. The callback can only abort its
    // own transport; it cannot ask Core to close the scanner's owning socket.
    io.restart();
    const auto timeout_now = ruvia::detail::websocket_steady_now_ms() + 1;
    asio::post(io, [&connection, timeout_now] {
        websocket_connection<recording_transport>::heartbeat_tick_thunk(&connection, timeout_now);
    });
    (void)io.poll();
    RUVIA_CHECK(state.aborted_);
}

RUVIA_TEST(websocket_heartbeat_ignores_unmatched_pong_until_timeout) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    ruvia::websocket_lifecycle_options lifecycle;
    lifecycle.heartbeat_ = {
        .ping_interval_ = std::chrono::milliseconds(1),
        .pong_timeout_ = std::chrono::milliseconds(1),
    };
    const auto incoming = masked_frame(0xA, "wrong") + masked_frame(0x1, "ok");
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, lifecycle, ruvia::protocol_byte_limit::limited(1024), memory.resource(), incoming);

    asio::post(io, [&connection] {
        websocket_connection<recording_transport>::heartbeat_tick_thunk(&connection, 10);
    });
    (void)io.poll();
    RUVIA_CHECK_EQ(state.writes_, std::size_t{1});
    RUVIA_CHECK_EQ(state.last_non_empty_bytes_.size(), std::size_t{10});
    RUVIA_CHECK_EQ(static_cast<unsigned char>(state.last_non_empty_bytes_[0]), 0x89U);

    auto reader_value = asio::co_spawn(io, [&]() -> asio::awaitable<std::optional<ruvia::websocket_message>> { co_return co_await ruvia::as_awaitable(connection.read()); }, asio::use_future);
    io.restart();
    run_until_ready(io, reader_value);
    const auto message = reader_value.get();
    RUVIA_CHECK(message && message->payload() == "ok");

    io.restart();
    const auto timeout_now = ruvia::detail::websocket_steady_now_ms() + 2;
    asio::post(io, [&connection, timeout_now] {
        websocket_connection<recording_transport>::heartbeat_tick_thunk(&connection, timeout_now);
    });
    (void)io.poll();
    RUVIA_CHECK(state.aborted_);
}

RUVIA_TEST(websocket_heartbeat_matching_pong_satisfies_ping) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    ruvia::websocket_lifecycle_options lifecycle;
    lifecycle.heartbeat_ = {
        .ping_interval_ = std::chrono::milliseconds(1),
        .pong_timeout_ = std::chrono::milliseconds(1),
    };
    const auto ping_payload = ruvia::detail::websocket_heartbeat_payload(1);
    const auto incoming = masked_frame(0xA, {ping_payload.data(), ping_payload.size()}) +
                          masked_frame(0x1, "ok");
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, lifecycle, ruvia::protocol_byte_limit::limited(1024), memory.resource(), incoming);

    asio::post(io, [&connection] {
        websocket_connection<recording_transport>::heartbeat_tick_thunk(&connection, 10);
    });
    (void)io.poll();
    RUVIA_CHECK_EQ(state.writes_, std::size_t{1});
    RUVIA_CHECK_EQ(std::string_view(state.last_non_empty_bytes_).substr(2),
        std::string_view(ping_payload.data(), ping_payload.size()));

    auto reader_value = asio::co_spawn(io, [&]() -> asio::awaitable<std::optional<ruvia::websocket_message>> { co_return co_await ruvia::as_awaitable(connection.read()); }, asio::use_future);
    io.restart();
    run_until_ready(io, reader_value);
    const auto message = reader_value.get();
    RUVIA_CHECK(message && message->payload() == "ok");

    io.restart();
    const auto timeout_now = ruvia::detail::websocket_steady_now_ms() + 2;
    asio::post(io, [&connection, timeout_now] {
        websocket_connection<recording_transport>::heartbeat_tick_thunk(&connection, timeout_now);
    });
    (void)io.poll();
    RUVIA_CHECK(!state.aborted_);
}

RUVIA_TEST(websocket_close_timeout_starts_after_close_write_commits) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    ruvia::websocket_lifecycle_options lifecycle;
    lifecycle.close_handshake_timeout_ = std::chrono::milliseconds(1);
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, lifecycle, ruvia::protocol_byte_limit::limited(1024), memory.resource());
    state.before_write_completion_target_ = &connection;
    state.before_write_completion_ = [](void* target) noexcept {
        auto* runtime = static_cast<websocket_connection<recording_transport>*>(target);
        // Even an arbitrarily large scanner time cannot expire a peer-response
        // window before the local Close write has completed.
        websocket_connection<recording_transport>::heartbeat_tick_thunk(runtime, 10000);
    };

    auto future = asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await ruvia::as_awaitable(connection.close()); }, asio::use_future);
    run_until_ready(io, future);
    future.get();

    RUVIA_CHECK(!state.aborted_);
    RUVIA_CHECK(state.before_write_completion_ == nullptr);
}

RUVIA_TEST(websocket_runtime_maps_typed_outbound_rejections) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(4), memory.resource());
    bool message_rejected = false;
    bool invalid_text_rejected = false;
    bool close_rejected = false;
    const std::string invalid_text("\xc0\x80", 2);

    auto validation = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::as_awaitable(
                    connection.write(websocket_opcode::text, "12345"));
            } catch (const std::invalid_argument&) {
                message_rejected = true;
            }
            try {
                co_await ruvia::as_awaitable(
                    connection.write(websocket_opcode::text, invalid_text));
            } catch (const std::invalid_argument&) {
                invalid_text_rejected = true;
            }
            try {
                co_await ruvia::as_awaitable(connection.close({.code_ = 1005}));
            } catch (const std::invalid_argument&) {
                close_rejected = true;
            }
        },
        asio::use_future);

    run_until_ready(io, validation);
    validation.get();
    RUVIA_CHECK(message_rejected);
    RUVIA_CHECK(invalid_text_rejected);
    RUVIA_CHECK(close_rejected);
    RUVIA_CHECK_EQ(state.writes_, std::size_t{0});
    RUVIA_CHECK(!state.aborted_);
}

RUVIA_TEST(websocket_write_guard_rejects_overlap_and_releases_after_suspend) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    state.suspend_next_write_ = true;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    auto first = asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await ruvia::as_awaitable(connection.write(websocket_opcode::text, "first")); }, asio::use_future);
    io.poll();
    RUVIA_CHECK(state.complete_write_ != nullptr);

    io.restart();
    bool rejected = false;
    auto overlap_check = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            auto overlapping = connection.write(websocket_opcode::text, "overlap");
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        co_return; }, asio::use_future);
    io.poll();
    overlap_check.get();
    RUVIA_CHECK(rejected);

    auto complete_write = std::move(state.complete_write_);
    asio::post(io, std::move(complete_write));
    io.restart();
    run_until_ready(io, first);
    first.get();

    io.restart();
    auto following = asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await ruvia::as_awaitable(connection.write(websocket_opcode::text, "following")); }, asio::use_future);
    run_until_ready(io, following);
    following.get();
    RUVIA_CHECK_EQ(state.writes_, std::size_t{2});
}

RUVIA_TEST(websocket_close_guard_rejects_write_until_close_flush_commits) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    state.suspend_next_write_ = true;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    auto closing = asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await ruvia::as_awaitable(connection.close()); }, asio::use_future);
    io.poll();
    RUVIA_CHECK(state.complete_write_ != nullptr);

    io.restart();
    bool rejected = false;
    auto overlap_check = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            auto overlapping = connection.write(websocket_opcode::text, "late");
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        co_return; }, asio::use_future);
    io.poll();
    overlap_check.get();
    RUVIA_CHECK(rejected);

    auto complete_write = std::move(state.complete_write_);
    asio::post(io, std::move(complete_write));
    io.restart();
    run_until_ready(io, closing);
    closing.get();
    RUVIA_CHECK_EQ(state.writes_, std::size_t{2});
    RUVIA_CHECK(state.last_disposition_ == websocket_transport_disposition::end_transport);
}

RUVIA_TEST(websocket_teardown_aborts_and_joins_suspended_application_write) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    state.suspend_next_write_ = true;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource());

    auto writing = asio::co_spawn(io, [&]() -> asio::awaitable<void> { co_await ruvia::as_awaitable(connection.write(websocket_opcode::text, "in flight")); }, asio::use_future);
    io.poll();
    RUVIA_CHECK(state.complete_write_ != nullptr);

    io.restart();
    auto teardown = asio::co_spawn(
        io, ruvia::as_awaitable(connection.detach_and_drain_writes()), asio::use_future);
    run_until_ready(io, writing, teardown);

    writing.get();
    teardown.get();
    RUVIA_CHECK(state.aborted_);
    RUVIA_CHECK(state.complete_write_ == nullptr);
}

// HTTP/1 upgraded-byte-stream bridge: Ping is answered by the protocol core,
// fragmented Text is reassembled, the application echo is serialized by the
// same core, and the normal Close is emitted on the socket transport.
RUVIA_TEST(websocket_socket_bridge_ping_fragment_echo_and_close) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    bool server_saw_message = false;
    bool got_pong = false;
    bool got_echo = false;
    bool got_close = false;
    bool server_close_completed = false;

    auto server = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory memory;
            connection_scanner::entry_type scanner_entry;
            socket_websocket_connection_type<tcp::socket> connection(
                websocket_socket_transport<tcp::socket>(socket), worker_handle_value, scanner_entry, {},
                ruvia::protocol_byte_limit::limited(1024), memory.resource());
            const auto message = co_await ruvia::as_awaitable(connection.read());
            server_saw_message = message.has_value() && message->payload() == "hello";
            if (message) {
                co_await ruvia::as_awaitable(
                    connection.write(message->opcode(), message->payload()));
            }
            co_await ruvia::as_awaitable(connection.close());
            server_close_completed = true;
        },
        asio::use_future);

    auto client = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            std::string input = masked_frame(0x9, "p");
            input += masked_frame(0x1, "hel", false);
            input += masked_frame(0x0, "lo", true);
            co_await asio::async_write(socket, asio::buffer(input), asio::use_awaitable);

            const auto pong = co_await read_short_server_frame(socket);
            got_pong =
                pong.size() == 3 && static_cast<unsigned char>(pong[0]) == 0x8A && pong[2] == 'p';
            const auto echo = co_await read_short_server_frame(socket);
            got_echo = echo.size() == 7 && static_cast<unsigned char>(echo[0]) == 0x81 &&
                       echo.substr(2) == "hello";
            const auto close = co_await read_short_server_frame(socket);
            got_close = close.size() >= 4 && static_cast<unsigned char>(close[0]) == 0x88 &&
                        static_cast<unsigned char>(close[2]) == 0x03 &&
                        static_cast<unsigned char>(close[3]) == 0xE8;
            if (got_close) {
                const auto reply = masked_frame(0x8, std::string_view("\x03\xE8", 2));
                co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            }
        },
        asio::use_future);

    run_until_ready(io, server, client);
    server.get();
    client.get();
    RUVIA_CHECK(server_saw_message);
    RUVIA_CHECK(got_pong);
    RUVIA_CHECK(got_echo);
    RUVIA_CHECK(got_close);
    RUVIA_CHECK(server_close_completed);
}

RUVIA_TEST(websocket_write_honors_per_frame_compression_choice) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    recording_transport_state state;
    connection_scanner::entry_type scanner_entry;
    ruvia::worker_memory memory;
    websocket_connection<recording_transport> connection(recording_transport(io, state), worker_handle_value,
        scanner_entry, {}, ruvia::protocol_byte_limit::limited(1024), memory.resource(), {},
        (websocket_compression{.enabled_ = true}));
    const std::string payload_value(200, 'x');
    std::string uncompressed_frame;
    std::string compressed_frame;

    auto writing = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await ruvia::as_awaitable(
            connection.write(websocket_opcode::text, payload_value, false));
        uncompressed_frame = state.last_non_empty_bytes_;
        co_await ruvia::as_awaitable(connection.write(websocket_opcode::text, payload_value));
        compressed_frame = state.last_non_empty_bytes_; }, asio::use_future);
    run_until_ready(io, writing);
    writing.get();

    RUVIA_CHECK(!uncompressed_frame.empty());
    RUVIA_CHECK(!compressed_frame.empty());
    if (!uncompressed_frame.empty() && !compressed_frame.empty()) {
        RUVIA_CHECK((static_cast<unsigned char>(uncompressed_frame[0]) & 0x40U) == 0);
        RUVIA_CHECK((static_cast<unsigned char>(compressed_frame[0]) & 0x40U) != 0);
    }
}

// Negotiated permessage-deflate crosses the actual socket bridge in both
// directions: core decode on read, core encode on application write.
RUVIA_TEST(websocket_socket_bridge_permessage_deflate_round_trip) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    const std::string original(200, 'a');
    bool server_decoded = false;
    bool client_decoded = false;

    auto server = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory memory;
            connection_scanner::entry_type scanner_entry;
            socket_websocket_connection_type<tcp::socket> connection(
                websocket_socket_transport<tcp::socket>(socket), worker_handle_value, scanner_entry, {},
                ruvia::protocol_byte_limit::limited(1024), memory.resource(), {},
                (websocket_compression{.enabled_ = true}));
            const auto message = co_await ruvia::as_awaitable(connection.read());
            server_decoded = message.has_value() && message->payload() == original;
            if (message) {
                co_await ruvia::as_awaitable(
                    connection.write(websocket_opcode::text, message->payload()));
            }
            co_await ruvia::as_awaitable(connection.close());
        },
        asio::use_future);

    auto client_future = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            ruvia::websocket_connection client({
                .message_limit_ = ruvia::protocol_byte_limit::limited(1024),
                .compression_ = (websocket_compression{.enabled_ = true}),
                .role_ = ruvia::websocket_connection_role::client,
                .mask_key_generator_ = +[](void*, ruvia::websocket_mask_key_type& key) noexcept {
                    key = {'\x11', '\x22', '\x33', '\x44'};
                    return true;
                },
            });
            if (client.submit_frame(websocket_opcode::text, original) !=
                ruvia::websocket_frame_submit_status::accepted) {
                co_return;
            }
            const std::string request(client.output_plan().bytes());
            (void)client.consume_output(request.size());
            co_await asio::async_write(socket, asio::buffer(request), asio::use_awaitable);

            const auto response = co_await read_short_server_frame(socket);
            if (response.size() < 2 || (static_cast<unsigned char>(response[0]) & 0x40U) == 0) {
                co_return;
            }
            if (client.feed(response) == ruvia::websocket_feed_status::accepted) {
                const auto event = client.next_event();
                client_decoded = event && event->message() && event->message()->payload() == original;
            }
            const auto close = co_await read_short_server_frame(socket);
            if (close.size() >= 4 &&
                (static_cast<unsigned char>(close[0]) & 0x0fU) == 0x8U) {
                const auto reply = masked_frame(0x8, std::string_view(close).substr(2));
                co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            }
        },
        asio::use_future);

    run_until_ready(io, server, client_future);
    server.get();
    client_future.get();
    RUVIA_CHECK(server_decoded);
    RUVIA_CHECK(client_decoded);
}

// An unmasked client frame is rejected by the core and the HTTP/1 socket bridge
// flushes the generated 1002 Close instead of rebuilding it in the web layer.
RUVIA_TEST(websocket_socket_bridge_protocol_error_flushes_core_close) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    bool server_ended = false;
    std::uint16_t close_code = 0;

    auto server = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory memory;
            connection_scanner::entry_type scanner_entry;
            socket_websocket_connection_type<tcp::socket> connection(
                websocket_socket_transport<tcp::socket>(socket), worker_handle_value, scanner_entry, {},
                ruvia::protocol_byte_limit::limited(1024), memory.resource());
            const auto message = co_await ruvia::as_awaitable(connection.read());
            server_ended = !message.has_value();
        },
        asio::use_future);

    auto client = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            const std::string invalid{"\x81\x02hi", 4};
            co_await asio::async_write(socket, asio::buffer(invalid), asio::use_awaitable);
            const auto close = co_await read_short_server_frame(socket);
            if (close.size() >= 4 && static_cast<unsigned char>(close[0]) == 0x88) {
                close_code = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(static_cast<unsigned char>(close[2])) << 8) |
                    static_cast<unsigned char>(close[3]));
            }
        },
        asio::use_future);

    run_until_ready(io, server, client);
    server.get();
    client.get();
    RUVIA_CHECK(server_ended);
    RUVIA_CHECK_EQ(close_code, static_cast<std::uint16_t>(1002));
}

RUVIA_TEST(websocket_fragment_storage_shares_budget_and_releases_completed_messages) {
    ruvia::detail::inbound_buffer_resource worker_value(std::pmr::new_delete_resource(), 35000);
    std::optional<std::pmr::string> first_input(std::in_place, &worker_value);
    std::optional<std::pmr::string> second_input(std::in_place, &worker_value);
    std::optional<ruvia::websocket_server_protocol> first(std::in_place, *first_input);
    std::optional<ruvia::websocket_server_protocol> second(std::in_place, *second_input);
    const auto baseline = worker_value.used();
    const auto fragment = [](unsigned char opcode, std::size_t bytes_value) {
        std::string wire;
        wire.push_back(static_cast<char>(opcode));
        wire.push_back(static_cast<char>(0xfe));
        wire.push_back(static_cast<char>(bytes_value >> 8));
        wire.push_back(static_cast<char>(bytes_value));
        wire.append(4, '\0');
        wire.append(bytes_value, 'x');
        return wire;
    };
    *first_input = fragment(0x02, 8000);
    RUVIA_CHECK(!first->poll());
    RUVIA_CHECK(worker_value.used() >= baseline + 8000);
    *second_input = fragment(0x02, 8000);
    RUVIA_CHECK(!second->poll());
    RUVIA_CHECK(worker_value.used() >= baseline + 16000);
    bool rejected = false;
    try {
        *first_input = fragment(0x80, 8000);
        (void)first->poll();
    } catch (const ruvia::detail::inbound_buffer_limit_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    first.reset();
    first_input.reset();
    *second_input = std::string("\x80\x80\x00\x00\x00\x00", 6);
    const auto event = second->poll();
    RUVIA_CHECK(event && event->message());
    RUVIA_CHECK_EQ(event->message()->payload(), std::string(8000, 'x'));
    const auto retained = worker_value.used();
    RUVIA_CHECK(!second->poll());
    RUVIA_CHECK(worker_value.used() + 8000 <= retained);
    second.reset();
    second_input.reset();
    RUVIA_CHECK_EQ(worker_value.used(), std::size_t{0});
}
