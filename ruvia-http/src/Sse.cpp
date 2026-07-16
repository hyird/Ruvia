#include "ruvia/http/Sse.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <system_error>

namespace ruvia::detail {
namespace {

void appendSseData(std::pmr::string& frame, std::string_view data) {
    while (true) {
        const auto next = data.find_first_of("\r\n");
        const auto line = next == std::string_view::npos ? data : data.substr(0, next);
        frame.append("data: ");
        frame.append(line.data(), line.size());
        frame.push_back('\n');
        if (next == std::string_view::npos) {
            return;
        }
        auto advance = next + 1;
        if (data[next] == '\r' && next + 1 < data.size() && data[next + 1] == '\n') {
            advance = next + 2;
        }
        data.remove_prefix(advance);
    }
}

void appendUnsigned(std::pmr::string& frame, std::uint32_t value) {
    std::array<char, 10> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format SSE retry value");
    }
    frame.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

}  // namespace

void formatSseMessage(std::pmr::string& frame, const SseMessage& message) {
    if (message.event.find_first_of("\r\n") != std::string_view::npos ||
        (message.id.has_value() &&
         message.id->find_first_of("\r\n") != std::string_view::npos)) {
        throw std::invalid_argument("SSE event and id must not contain CR or LF");
    }
    if (message.id.has_value() && message.id->find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SSE id must not contain a NUL character");
    }

    frame.clear();
    if (!message.event.empty()) {
        frame.append("event: ");
        frame.append(message.event.data(), message.event.size());
        frame.push_back('\n');
    }
    if (message.id.has_value()) {
        frame.append("id:");
        if (!message.id->empty()) {
            frame.push_back(' ');
            frame.append(message.id->data(), message.id->size());
        }
        frame.push_back('\n');
    }
    if (message.retry.has_value()) {
        frame.append("retry: ");
        appendUnsigned(frame, *message.retry);
        frame.push_back('\n');
    }
    if (message.data.has_value()) {
        appendSseData(frame, *message.data);
    }
    frame.push_back('\n');
}

}  // namespace ruvia::detail
