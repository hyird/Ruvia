#include "ruvia/http/sse.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/http/detail/util/pmr_resource.h"

namespace ruvia {
namespace {

void append_sse_data(std::pmr::string& frame, std::string_view data) {
    while (true) {
        const auto next_value = data.find_first_of("\r\n");
        const auto line = next_value == std::string_view::npos ? data : data.substr(0, next_value);
        frame.append("data: ");
        frame.append(line.data(), line.size());
        frame.push_back('\n');
        if (next_value == std::string_view::npos) {
            return;
        }
        auto advance = next_value + 1;
        if (data[next_value] == '\r' && next_value + 1 < data.size() && data[next_value + 1] == '\n') {
            advance = next_value + 2;
        }
        data.remove_prefix(advance);
    }
}

void append_unsigned(std::pmr::string& frame, std::uint64_t value) {
    std::array<char, 20> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format SSE retry value");
    }
    frame.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

}  // namespace

std::pmr::string format_sse_message(const sse_message& message, sse_format_options options) {
    if (message.event_.view().find_first_of("\r\n") != std::string_view::npos ||
        (message.id_.has_value() &&
            message.id_->view().find_first_of("\r\n") != std::string_view::npos)) {
        throw std::invalid_argument("SSE event and id must not contain CR or LF");
    }
    if (message.id_.has_value() && (message.id_->view().find('\0') != std::string_view::npos)) {
        throw std::invalid_argument("SSE id must not contain a NUL character");
    }
    if (message.retry_.has_value() && message.retry_->count() < 0) {
        throw std::invalid_argument("SSE retry delay must not be negative");
    }

    std::pmr::string frame(detail::http_pmr_resource_or_default(options.resource_));
    if (!message.event_.empty()) {
        frame.append("event: ");
        frame.append(message.event_.data(), message.event_.size());
        frame.push_back('\n');
    }
    if (message.id_.has_value()) {
        frame.append("id:");
        if (!message.id_->empty()) {
            frame.push_back(' ');
            frame.append(message.id_->data(), message.id_->size());
        }
        frame.push_back('\n');
    }
    if (message.retry_.has_value()) {
        frame.append("retry: ");
        append_unsigned(frame, static_cast<std::uint64_t>(message.retry_->count()));
        frame.push_back('\n');
    }
    if (message.data_.has_value()) {
        append_sse_data(frame, message.data_->view());
    }
    frame.push_back('\n');
    return frame;
}

}  // namespace ruvia
