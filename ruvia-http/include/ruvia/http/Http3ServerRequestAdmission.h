#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ruvia {

enum class Http3ServerRequestAdmissionAction : std::uint8_t {
    kAdmit,
    kAnnounceGoaway,
    kReject,
};

enum class Http3ServerRequestAdmissionRejection : std::uint8_t {
    kNone,
    kInvalidStreamId,
    kRequestLimitReached,
};

enum class Http3ServerRequestAdmissionError : std::uint8_t {
    kZeroRequestLimit,
    kRequestLimitOutOfRange,
    kInvalidStreamId,
    kOutputTooSmall,
};

struct Http3ServerRequestAdmissionConfig final {
    std::uint64_t maxRequestsPerConnection{0};
};

struct Http3ServerRequestAdmissionDecision final {
    // kAnnounceGoaway rejects the triggering request; emitGoaway is true only
    // for the first announcement. Lower-ID requests remain admissible.
    Http3ServerRequestAdmissionAction action{Http3ServerRequestAdmissionAction::kReject};
    Http3ServerRequestAdmissionRejection rejection{Http3ServerRequestAdmissionRejection::kNone};
    std::uint64_t goawayId{0};
    // True only for the transition that first requires the server to emit GOAWAY.
    bool emitGoaway{false};
};

// Plans server-side request admission using the fixed RFC 9114 GOAWAY boundary
// 4 * maxRequestsPerConnection. Call admit() once per newly opened peer request
// stream; QUIC guarantees stream IDs are unique for the connection.
class Http3ServerRequestAdmissionPlanner final {
public:
    [[nodiscard]] static std::expected<Http3ServerRequestAdmissionPlanner,
        Http3ServerRequestAdmissionError>
    create(Http3ServerRequestAdmissionConfig config) noexcept;

    Http3ServerRequestAdmissionPlanner(const Http3ServerRequestAdmissionPlanner&) = delete;
    Http3ServerRequestAdmissionPlanner& operator=(const Http3ServerRequestAdmissionPlanner&) = delete;
    Http3ServerRequestAdmissionPlanner(Http3ServerRequestAdmissionPlanner&&) noexcept = default;
    Http3ServerRequestAdmissionPlanner& operator=(Http3ServerRequestAdmissionPlanner&&) noexcept = default;

    [[nodiscard]] Http3ServerRequestAdmissionDecision admit(std::uint64_t streamId) noexcept;
    [[nodiscard]] Http3ServerRequestAdmissionDecision announceGoaway() noexcept;

    [[nodiscard]] std::uint64_t maxRequestsPerConnection() const noexcept {
        return maxRequestsPerConnection_;
    }
    [[nodiscard]] std::uint64_t goawayId() const noexcept {
        return goawayId_;
    }
    [[nodiscard]] bool goawayAnnounced() const noexcept {
        return goawayAnnounced_;
    }

private:
    explicit Http3ServerRequestAdmissionPlanner(std::uint64_t maxRequestsPerConnection) noexcept
        : maxRequestsPerConnection_(maxRequestsPerConnection),
          goawayId_(maxRequestsPerConnection * 4) {}

    std::uint64_t maxRequestsPerConnection_;
    std::uint64_t goawayId_;
    bool goawayAnnounced_{false};
};

// Encodes one complete server GOAWAY frame, including frame header and the
// varint request-stream boundary. The output is unchanged on failure.
[[nodiscard]] std::expected<std::size_t, Http3ServerRequestAdmissionError>
encodeHttp3ServerGoawayFrame(std::span<char> output, std::uint64_t goawayId) noexcept;

}  // namespace ruvia
