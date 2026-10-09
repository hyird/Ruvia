#pragma once

#include <array>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/streaming.h"

#include "body/http_request_body_facade.h"
#include "http/streaming_access.h"
#include "server/http_response_stream_state.h"
#include "test_harness.h"
#include "websocket/websocket_access.h"

namespace streaming_test {

constexpr ruvia::sse_message literal_sse_message{.data_ = "data", .event_ = "event", .id_ = "id"};

class test_scoped_capability final {
public:
    test_scoped_capability(ruvia::operation_scope& scope, int& expired_count) noexcept
        : expired_count_(&expired_count),
          registration_(scope, this, &test_scoped_capability::expire) {}

    test_scoped_capability(const test_scoped_capability& other) noexcept
        : expired_count_(other.expired_count_),
          registration_(other.registration_, this) {}

    test_scoped_capability(test_scoped_capability&& other) noexcept
        : expired_count_(std::exchange(other.expired_count_, nullptr)),
          registration_(std::move(other.registration_), this) {}

    void use() const {
        registration_.require_active();
    }

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<test_scoped_capability*>(target);
        ++*capability.expired_count_;
    }

    int* expired_count_;
    ruvia::scoped_capability_registration registration_;
};

struct cold_frame_probe final {
    bool* destroyed_;
    bool armed_{true};

    explicit cold_frame_probe(bool& value) noexcept
        : destroyed_(&value) {}
    cold_frame_probe(cold_frame_probe&& other) noexcept
        : destroyed_(other.destroyed_),
          armed_(std::exchange(other.armed_, false)) {}
    ~cold_frame_probe() {
        if (armed_) {
            *destroyed_ = true;
        }
    }
};

inline ruvia::task<void> cold_frame_task(cold_frame_probe) {
    co_return;
}

struct capture_stream_sink final {
    std::vector<std::string> writes_;
    std::vector<std::string> trailers_;
};

inline ruvia::task<void> write_chunk(void* target, std::string_view chunk) {
    static_cast<capture_stream_sink*>(target)->writes_.emplace_back(chunk);
    co_return;
}

inline ruvia::task<void> end_stream(void* target, std::span<const ruvia::http_header_view> trailers) {
    auto& captured_value = static_cast<capture_stream_sink*>(target)->trailers_;
    for (const auto& trailer : trailers) {
        captured_value.emplace_back(std::string(trailer.name()) + "=" + std::string(trailer.value()));
    }
    co_return;
}

inline ruvia::task<ruvia::timer_sleep_result> sleep_stream(
    void*, std::chrono::milliseconds, const ruvia::stop_token&) {
    co_return ruvia::timer_sleep_result::elapsed;
}

inline void bind_context(void*, ruvia::context*, ruvia::task<ruvia::http_response> (*)(ruvia::context&)) noexcept {
}
inline void release_context(void*) noexcept {}

inline bool committed(void*) noexcept {
    return false;
}

inline bool aborted(void*) noexcept {
    return false;
}

inline ruvia::task<ruvia::http_response> unused_streaming_head(ruvia::context&) {
    co_return ruvia::http_response({.resource_ = std::pmr::get_default_resource()});
}

inline ruvia::response_stream_writer make_writer(capture_stream_sink& sink_value) noexcept {
    return ruvia::detail::streaming_access::make_response_stream_writer(*ruvia::detail::process_resource(), &sink_value, &write_chunk, &end_stream,
        &sleep_stream, &bind_context, &release_context, &committed, &aborted);
}

inline ruvia::task<void> write_lines(ruvia::response_stream_writer& writer) {
    co_await writer.writeln("first");
    co_await writer.writeln("second");
}

inline ruvia::task<void> write_stored_lines(ruvia::response_stream_writer& writer) {
    auto first = writer.writeln(std::string("stored-first"));
    co_await std::move(first);

    auto second = writer.writeln(std::string("stored-second"));
    co_await std::move(second);
}

inline ruvia::scoped_operation<void> make_expired_write(capture_stream_sink& sink_value) {
    auto writer = make_writer(sink_value);
    return writer.write(std::string("must-not-run"));
}

inline ruvia::task<void> await_expired_write(
    ruvia::scoped_operation<void>& operation, bool& rejected) {
    try {
        co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

struct capture_websocket final {
    std::vector<std::string> writes_;
};

inline ruvia::task<std::optional<ruvia::websocket_message>> read_socket(void*) {
    co_return std::nullopt;
}

inline ruvia::task<void> write_socket(
    void* target, ruvia::websocket_opcode, std::string_view payload_value, bool) {
    static_cast<capture_websocket*>(target)->writes_.emplace_back(payload_value);
    co_return;
}

inline ruvia::task<void> close_socket(void*, ruvia::websocket_close_options) {
    co_return;
}

inline ruvia::scoped_operation<void> make_expired_websocket_write(capture_websocket& capture_value) {
    auto socket =
        ruvia::detail::websocket_access::make(*ruvia::detail::process_resource(), &capture_value, &read_socket, &write_socket, &close_socket);
    return socket.text(std::string("expired-payload"));
}

inline ruvia::task<void> write_stored_temporary_websocket_payload(ruvia::websocket& socket) {
    auto operation = socket.text(std::string("owned-payload"));
    co_await std::move(operation);
}

struct immediate_body_source final {
    ruvia::task<std::optional<std::span<const std::byte>>> read() {
        co_return ruvia::as_bytes("must-not-read");
    }
};

inline ruvia::scoped_operation<std::optional<std::span<const std::byte>>> make_expired_body_read() {
    ruvia::detail::body_reader_binding<immediate_body_source> binding;
    return binding.facade().read();
}

inline ruvia::task<void> await_expired_body_read(
    ruvia::scoped_operation<std::optional<std::span<const std::byte>>>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::task<void> end_with_trailers(ruvia::response_stream_writer& writer) {
    const std::array<ruvia::http_header_view, 2> trailers{
        ruvia::http_header_view{"Digest", "sha-256=value"},
        ruvia::http_header_view{"Server-Timing", "db;dur=7"}};
    co_await writer.end(trailers);
}

inline ruvia::task<void> end_with_expired_trailer_sources(ruvia::response_stream_writer& writer) {
    auto operation = [&] {
        std::string name = "X-Owned-Trailer";
        std::string value = "temporary-value";
        const std::array<ruvia::http_header_view, 1> trailers{ruvia::http_header_view{name, value}};
        return writer.end(trailers);
    }();
    co_await std::move(operation);
}

struct suspended_body_source final {
    struct awaiter_type final {
        suspended_body_source& source_;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            source_.continuation_ = continuation;
            source_.read_suspended_ = true;
        }
        void await_resume() const noexcept {}
    };

    ruvia::task<std::optional<std::span<const std::byte>>> read() {
        co_await awaiter_type{*this};
        co_return std::nullopt;
    }

    void resume() {
        const auto suspended = std::exchange(continuation_, {});
        if (suspended) {
            suspended.resume();
        }
    }

    std::coroutine_handle<> continuation_{};
    bool read_suspended_{false};
};

struct suspended_stream_sink final {
    struct awaiter_type final {
        suspended_stream_sink& sink_;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            sink_.continuation_ = continuation;
            sink_.write_suspended_ = true;
        }
        void await_resume() const noexcept {}
    };

    void resume() {
        const auto suspended = std::exchange(continuation_, {});
        if (suspended) {
            suspended.resume();
        }
    }

    std::coroutine_handle<> continuation_{};
    std::vector<std::string> writes_;
    std::size_t ends_{0};
    bool write_suspended_{false};
    bool suspend_next_write_{true};
    bool fail_next_write_{false};
};

inline ruvia::task<void> write_suspended_stream(void* target, std::string_view chunk) {
    auto& sink_value = *static_cast<suspended_stream_sink*>(target);
    sink_value.writes_.emplace_back(chunk);
    if (std::exchange(sink_value.fail_next_write_, false)) {
        throw std::runtime_error("stream write failed");
    }
    if (std::exchange(sink_value.suspend_next_write_, false)) {
        co_await suspended_stream_sink::awaiter_type{sink_value};
    }
}

inline ruvia::task<void> end_suspended_stream(void* target, std::span<const ruvia::http_header_view>) {
    ++static_cast<suspended_stream_sink*>(target)->ends_;
    co_return;
}

inline ruvia::response_stream_writer make_suspended_writer(suspended_stream_sink& sink_value) noexcept {
    return ruvia::detail::streaming_access::make_response_stream_writer(*ruvia::detail::process_resource(), &sink_value, &write_suspended_stream,
        &end_suspended_stream, &sleep_stream, &bind_context, &release_context, &committed, &aborted);
}

inline ruvia::task<void> complete_body_read(ruvia::body_reader& reader_value, bool& completed) {
    (void)co_await reader_value.read();
    completed = true;
}

inline ruvia::task<void> reject_concurrent_body_read(ruvia::body_reader& reader_value, bool& rejected) {
    try {
        (void)co_await reader_value.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::task<void> complete_stream_write(
    ruvia::response_stream_writer& writer, std::string_view chunk, bool& completed) {
    co_await writer.write(chunk);
    completed = true;
}

inline ruvia::task<void> reject_concurrent_stream_write(
    ruvia::response_stream_writer& writer, bool& rejected) {
    try {
        co_await writer.write("overlap");
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::task<void> reject_concurrent_stream_end(
    ruvia::response_stream_writer& writer, bool& rejected) {
    try {
        co_await writer.end();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::task<void> observe_stream_write_failure(
    ruvia::response_stream_writer& writer, bool& failed) {
    try {
        co_await writer.write("failed");
    } catch (const std::runtime_error&) {
        failed = true;
    }
}

}  // namespace streaming_test

namespace streaming_test {

inline ruvia::task<void> write_one_sse(ruvia::sse_writer& sse, ruvia::sse_message message) {
    co_await sse.write(message);
}

}  // namespace streaming_test

using namespace streaming_test;  // NOLINT(google-build-using-namespace)
