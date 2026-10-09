#include "ruvia/http/http3_server_request_admission.h"

#include <array>
#include <cstdint>
#include <span>
#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia {
namespace {

constexpr std::uint64_t max_server_goaway_id = http3_var_int_max & ~std::uint64_t{3};
constexpr std::uint64_t max_supported_requests = max_server_goaway_id / 4;

[[nodiscard]] bool is_request_stream_id(std::uint64_t stream_id) noexcept {
    return (http3_peer_streams::accept_bidirectional(http3_peer_role::server, stream_id).index() == 0);
}

}  // namespace

std::variant<http3_server_request_admission_planner, http3_server_request_admission_error>
http3_server_request_admission_planner::create(http3_server_request_admission_config config) noexcept {
    if (config.max_requests_per_connection_ == 0) {
        return http3_server_request_admission_error::zero_request_limit;
    }
    if (config.max_requests_per_connection_ > max_supported_requests) {
        return http3_server_request_admission_error::request_limit_out_of_range;
    }
    return http3_server_request_admission_planner(config.max_requests_per_connection_);
}

http3_server_request_admission_decision http3_server_request_admission_planner::admit(
    std::uint64_t stream_id) noexcept {
    if (!is_request_stream_id(stream_id)) {
        return {.action_ = http3_server_request_admission_action::reject,
            .rejection_ = http3_server_request_admission_rejection::invalid_stream_id,
            .goaway_id_ = goaway_id_};
    }
    if (stream_id >= goaway_id_) {
        const auto announcement = announce_goaway();
        return {.action_ = announcement.emit_goaway_
                               ? http3_server_request_admission_action::announce_goaway
                               : http3_server_request_admission_action::reject,
            .rejection_ = http3_server_request_admission_rejection::request_limit_reached,
            .goaway_id_ = goaway_id_,
            .emit_goaway_ = announcement.emit_goaway_};
    }
    return {.action_ = http3_server_request_admission_action::admit, .goaway_id_ = goaway_id_};
}

http3_server_request_admission_decision http3_server_request_admission_planner::announce_goaway() noexcept {
    const bool emit_goaway = !goaway_announced_;
    goaway_announced_ = true;
    return {.action_ = http3_server_request_admission_action::announce_goaway,
        .goaway_id_ = goaway_id_,
        .emit_goaway_ = emit_goaway};
}

std::variant<std::size_t, http3_server_request_admission_error> encode_http3_server_goaway_frame(
    std::span<char> output, std::uint64_t goaway_id) noexcept {
    if (!is_request_stream_id(goaway_id)) {
        return http3_server_request_admission_error::invalid_stream_id;
    }

    std::array<char, http3_var_int_max_bytes> payload_value{};
    const auto payload_size = encode_http3_var_int(payload_value, goaway_id);
    if ((payload_size.index() != 0)) {
        return http3_server_request_admission_error::invalid_stream_id;
    }
    const auto written = encode_http3_frame(output, static_cast<std::uint64_t>(http3_frame_type::goaway),
        std::span<const char>(payload_value).first(std::get<0>(payload_size)));
    if ((written.index() != 0)) {
        return http3_server_request_admission_error::output_too_small;
    }
    return std::get<0>(written);
}

}  // namespace ruvia
