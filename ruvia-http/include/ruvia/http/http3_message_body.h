#pragma once

#include <cstdint>
#include <optional>

namespace ruvia {

// Result of feeding the size of one HTTP/3 DATA payload and its stream FIN.
enum class http3_message_body_result : std::uint8_t {
    accepted,
    complete,
    payload_not_allowed,
    content_length_exceeded,
    content_length_mismatch,
    length_overflow,
    already_complete,
    already_failed,
};

// Shared request/response DATA and FIN accounting state. content_length is the
// expected representation length (when present), not a framing length.
class http3_message_body final {
public:
    enum class state_type : std::uint8_t {
        receiving,
        complete,
        failed,
    };

    http3_message_body(std::optional<std::uint64_t> content_length, bool payload_allowed) noexcept;

    // data_length is only accounted; payload bytes remain owned by the caller.
    // Any protocol error is terminal. FIN succeeds only when Content-Length,
    // if present, exactly matches the total DATA payload length.
    [[nodiscard]] http3_message_body_result feed(std::uint64_t data_length, bool fin) noexcept;

    [[nodiscard]] state_type state() const noexcept;
    [[nodiscard]] std::uint64_t received_length() const noexcept;

private:
    std::optional<std::uint64_t> content_length_{};
    std::uint64_t received_length_{0};
    bool payload_allowed_{true};
    state_type state_{state_type::receiving};
};

}  // namespace ruvia
