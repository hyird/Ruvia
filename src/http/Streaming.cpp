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
        const auto next = data.find_first_of("\r\n");
        const auto line = next == std::string_view::npos ? data : data.substr(0, next);
        frame.append("data: ");
        frame.append(line.data(), line.size());
        frame.push_back('\n');
        if (next == std::string_view::npos) {
            return;
        }
        // An EventSource line break is CR, LF, or CRLF, so a bare CR is a line
        // separator too. Split on any of them and consume a CRLF pair together, so
        // no raw CR survives into a "data:" line where the client would reinterpret
        // it as an extra line break (injecting fields or, via a blank line, events).
        auto advance = next + 1;
        if (data[next] == '\r' && next + 1 < data.size() && data[next + 1] == '\n') {
            advance = next + 2;
        }
        data.remove_prefix(advance);
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
