// Streaming: streaming request bodies, typed multipart chunk phases, chunked
// response streaming and server-sent events.
// Run ruvia_example_streaming on port 8082. Try:
// curl -N http://127.0.0.1:8082/streaming/events
// curl --data-binary @file.bin http://127.0.0.1:8082/streaming/upload/raw
// curl -F file=@file.bin http://127.0.0.1:8082/streaming/upload/multipart
// A chunk borrows reusable reader storage; consume it before the next read.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <system_error>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

class streaming_controller final : public ruvia::controller<streaming_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/streaming")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST_STREAM("/upload/raw", upload_raw);
    RUVIA_POST_STREAM("/upload/multipart", upload_multipart);
    RUVIA_GET_STREAM("/chunks", chunks);
    RUVIA_GET_SSE("/events", events_value);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> upload_raw(ruvia::context& c) {
        std::size_t bytes_value = 0;
        auto& reader_value = c.req().get_body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append("uploaded bytes=");
        append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> upload_multipart(ruvia::context& c) {
        std::size_t parts = 0;
        std::size_t bytes_value = 0;
        auto reader_value = c.req().get_multipart_reader();
        while (auto part = co_await reader_value.read()) {
            if (part->phase() == ruvia::multipart_chunk_phase::first ||
                part->phase() == ruvia::multipart_chunk_phase::complete) {
                ++parts;
            }
            bytes_value += part->body().size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append("multipart parts=");
        append_unsigned(body, parts);
        body.append(" bytes=");
        append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<void> chunks(ruvia::context& c) {
        auto& stream = c.stream_text();
        co_await stream.write("part 1\n");
        co_await stream.writeln("part 2");
        if (co_await stream.sleep(std::chrono::milliseconds(20)) ==
            ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!stream.aborted()) {
            co_await stream.write("part 3\n");
        }
    }

    ruvia::task<void> events_value(ruvia::context& c) {
        auto events_value = c.stream_sse();
        co_await events_value.write({.data_ = "connected", .event_ = "open", .id_ = "1"});
        if (co_await events_value.sleep(std::chrono::milliseconds(20)) ==
            ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!events_value.aborted()) {
            co_await events_value.write({.data_ = "heartbeat",
                .event_ = "tick",
                .id_ = "2",
                .retry_ = std::chrono::milliseconds{3000}});
        }
    }

    static void append_unsigned(std::pmr::string& output, std::size_t value) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8082})
        .server({
            .worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .max_buffered_body_bytes_ = 16 * 1024 * 1024,
            .max_stream_body_bytes_ = std::nullopt,
        })
        .run();
}
