#include <charconv>
#include <cstddef>
#include <system_error>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

class StreamingController final : public ruvia::Controller<StreamingController> {
public:
    RUVIA_CONTROLLER_GROUP("/streaming")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST_STREAM("/upload/raw", uploadRaw);
    RUVIA_POST_STREAM("/upload/multipart", uploadMultipart);
    RUVIA_GET_STREAM("/chunks", chunks);
    RUVIA_GET_SSE("/events", events);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> uploadRaw(ruvia::Context& c) {
        std::size_t bytes = 0;
        auto& reader = c.bodyReader();
        while (auto chunk = co_await reader.read()) {
            bytes += chunk->size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append("uploaded bytes=");
        appendUnsigned(body, bytes);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> uploadMultipart(ruvia::Context& c) {
        std::size_t parts = 0;
        std::size_t bytes = 0;
        auto reader = c.multipartReader();
        while (auto part = co_await reader.read()) {
            if (part->partBegin) {
                ++parts;
            }
            bytes += part->body.size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append("multipart parts=");
        appendUnsigned(body, parts);
        body.append(" bytes=");
        appendUnsigned(body, bytes);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<void> chunks(ruvia::Context& c) {
        auto& stream = c.streamText();
        co_await stream.write("part 1\n");
        co_await stream.write("part 2\n");
        co_await stream.write("part 3\n");
    }

    ruvia::Task<void> events(ruvia::Context& c) {
        auto events = c.streamSSE();
        co_await events.writeSSE({.data = "connected", .event = "open", .id = "1"});
        co_await events.writeSSE({.data = "heartbeat", .event = "tick", .id = "2", .retry = 3000});
    }

    static void appendUnsigned(std::pmr::string& output, std::size_t value) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8082)
        .setThreadNum(2)
        .setMaxBufferedBodyBytes(16 * 1024 * 1024)
        .setMaxStreamBodyBytes(0)
        .run();
}
