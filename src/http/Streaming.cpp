#include "StreamingInternal.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <system_error>

namespace ruvia {

Task<void> SseWriter::writeSSE(const SseMessage& message) {
    // event and id are single-line SSE fields. A CR or LF in either would start a
    // new field -- or, with a blank line, a whole new event -- letting a
    // caller-supplied value inject arbitrary content into the stream. The data
    // field is safely split per line (see appendData); hold event/id to the same
    // bar rather than emitting them verbatim.
    if (message.event.find_first_of("\r\n") != std::string_view::npos ||
        message.id.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument("SSE event and id must not contain CR or LF");
    }

    auto& frame = detail::StreamingAccess::scratch(writer_);
    frame.clear();
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
