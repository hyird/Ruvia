#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_server_request_parser.h"

namespace ruvia::testing {

// One-shot whole-message view of a buffered request over the public server
// contract: parse_head frames the head, and the body plan drives either a
// Content-Length slice or http1_chunked_body_decoder over the rest.
struct http1_request_message final {
    http1_server_request_parse_state state_;
    // Wire bytes of the whole message once it is complete (head, chunk lines
    // and trailers included).
    std::optional<std::size_t> message_bytes_{};
    // Decoded body of a complete message: the Content-Length bytes or the
    // concatenated chunk payloads.
    std::string body_{};
    std::optional<http1_chunk_decode_error> chunk_error_{};

    [[nodiscard]] bool complete() const noexcept {
        return message_bytes_.has_value();
    }
    // Head accepted, body still incomplete.
    [[nodiscard]] bool need_body() const noexcept {
        return state_.head_ready() != nullptr && !message_bytes_.has_value() && !chunk_error_.has_value();
    }
};

[[nodiscard]] inline http1_request_message parse_http1_request_message(
    std::string_view buffer, std::pmr::memory_resource* resource = nullptr) {
    http1_request_message message;
    message.state_.parse_head(buffer, 0, resource);
    const auto* head = message.state_.head_ready();
    if (head == nullptr) {
        return message;
    }
    const auto header_bytes = head->header_bytes();
    if (const auto* known_length = message.state_.body_plan_.known_length()) {
        if (buffer.size() - header_bytes < known_length->content_length()) {
            return message;
        }
        message.body_.assign(buffer.substr(header_bytes, known_length->content_length()));
        message.message_bytes_ = header_bytes + known_length->content_length();
        return message;
    }
    if (message.state_.body_plan_.chunked() == nullptr) {
        message.message_bytes_ = header_bytes;
        return message;
    }
    http1_chunked_body_decoder decoder;
    auto consumed = header_bytes;
    for (;;) {
        const auto result_value = decoder.decode(buffer.substr(consumed));
        if (const auto* chunk = result_value.body_chunk()) {
            message.body_.append(chunk->bytes());
            consumed += chunk->consumed_bytes();
            continue;
        }
        if (const auto* done = result_value.complete()) {
            message.message_bytes_ = consumed + done->consumed_bytes();
        } else if (const auto* failure = result_value.failure()) {
            message.chunk_error_ = failure->error();
        }
        return message;
    }
}

}  // namespace ruvia::testing
