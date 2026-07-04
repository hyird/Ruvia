#include "StreamingInternal.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <system_error>

namespace ruvia {

Task<void> SseWriter::writeSSE(const SseMessage& message) {
    auto& frame = detail::StreamingAccess::scratch(writer_);
    if (!message.event.empty()) {
        frame.append("event: ");
        frame.append(message.event.data(), message.event.size());
        frame.push_back('\n');
    }
    if (!message.id.empty()) {
        frame.append("id: ");
        frame.append(message.id.data(), message.id.size());
        frame.push_back('\n');
    }
    if (message.retry.has_value()) {
        frame.append("retry: ");
        appendUnsigned(frame, *message.retry);
        frame.push_back('\n');
    }
    appendData(frame, message.data);
    frame.push_back('\n');
    co_await writer_.write(frame);
}

void SseWriter::appendData(std::pmr::string& frame, std::string_view data) {
    while (true) {
        const auto next = data.find('\n');
        auto line = next == std::string_view::npos ? data : data.substr(0, next);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        frame.append("data: ");
        frame.append(line.data(), line.size());
        frame.push_back('\n');
        if (next == std::string_view::npos) {
            return;
        }
        data.remove_prefix(next + 1);
    }
}

void SseWriter::appendUnsigned(std::pmr::string& frame, std::uint32_t value) {
    std::array<char, 10> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format SSE retry value");
    }
    frame.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

}  // namespace ruvia
