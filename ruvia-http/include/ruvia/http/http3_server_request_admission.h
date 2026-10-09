#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ruvia {

enum class http3_server_request_admission_action : std::uint8_t {
    admit,
    announce_goaway,
    reject,
};

enum class http3_server_request_admission_rejection : std::uint8_t {
    none,
    invalid_stream_id,
    request_limit_reached,
};

enum class http3_server_request_admission_error : std::uint8_t {
    zero_request_limit,
    request_limit_out_of_range,
    invalid_stream_id,
    output_too_small,
};

struct http3_server_request_admission_config final {
    std::uint64_t max_requests_per_connection_{0};
};

struct http3_server_request_admission_decision final {
    // announce_goaway rejects the triggering request; emit_goaway is true only
    // for the first announcement. Lower-ID requests remain admissible.
    http3_server_request_admission_action action_{http3_server_request_admission_action::reject};
    http3_server_request_admission_rejection rejection_{http3_server_request_admission_rejection::none};
    std::uint64_t goaway_id_{0};
    // True only for the transition that first requires the server to emit GOAWAY.
    bool emit_goaway_{false};
};

// Plans server-side request admission using the fixed RFC 9114 GOAWAY boundary
// 4 * max_requests_per_connection. Call admit() once per newly opened peer request
// stream; QUIC guarantees stream IDs are unique for the connection.
class http3_server_request_admission_planner final {
public:
    [[nodiscard]] static std::variant<http3_server_request_admission_planner,
        http3_server_request_admission_error>
    create(http3_server_request_admission_config config) noexcept;

    http3_server_request_admission_planner(const http3_server_request_admission_planner&) = delete;
    http3_server_request_admission_planner& operator=(const http3_server_request_admission_planner&) = delete;
    http3_server_request_admission_planner(http3_server_request_admission_planner&&) noexcept = default;
    http3_server_request_admission_planner& operator=(http3_server_request_admission_planner&&) noexcept = default;

    [[nodiscard]] http3_server_request_admission_decision admit(std::uint64_t stream_id) noexcept;
    [[nodiscard]] http3_server_request_admission_decision announce_goaway() noexcept;

    [[nodiscard]] std::uint64_t max_requests_per_connection() const noexcept {
        return max_requests_per_connection_;
    }
    [[nodiscard]] std::uint64_t goaway_id() const noexcept {
        return goaway_id_;
    }
    [[nodiscard]] bool goaway_announced() const noexcept {
        return goaway_announced_;
    }

private:
    explicit http3_server_request_admission_planner(std::uint64_t max_requests_per_connection) noexcept
        : max_requests_per_connection_(max_requests_per_connection),
          goaway_id_(max_requests_per_connection * 4) {}

    std::uint64_t max_requests_per_connection_;
    std::uint64_t goaway_id_;
    bool goaway_announced_{false};
};

// Encodes one complete server GOAWAY frame, including frame header and the
// varint request-stream boundary. The output is unchanged on failure.
[[nodiscard]] std::variant<std::size_t, http3_server_request_admission_error>
encode_http3_server_goaway_frame(std::span<char> output, std::uint64_t goaway_id) noexcept;

}  // namespace ruvia
