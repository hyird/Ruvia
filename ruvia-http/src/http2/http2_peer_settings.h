#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

#include "http2/http2_frame_codec.h"
#include "http2/http2_role.h"

namespace ruvia::detail {

enum class http2_peer_setting_error : std::uint8_t {
    invalid_enable_push,
    invalid_initial_window,
    invalid_max_frame_size,
    invalid_enable_connect_protocol,
    invalid_enable_connect_protocol_transition,
    invalid_priority_setting
};

class http2_peer_setting_apply_result;

// The setting was valid and required no stream-window propagation. This also
// represents an unknown setting identifier, which RFC 9113 requires peers to
// ignore while continuing to process the SETTINGS frame.
class http2_peer_setting_applied final {
private:
    friend class http2_peer_setting_apply_result;

    constexpr http2_peer_setting_applied() noexcept = default;
};

// SETTINGS_INITIAL_WINDOW_SIZE is the only setting whose application must be
// propagated to every active stream. The signed difference remains observable
// only on this alternative; a reduction can legitimately make a window negative.
class http2_peer_initial_window_change final {
public:
    [[nodiscard]] constexpr std::int64_t delta() const noexcept {
        return delta_;
    }

private:
    friend class http2_peer_setting_apply_result;

    explicit constexpr http2_peer_initial_window_change(std::int64_t delta) noexcept
        : delta_(delta) {}

    std::int64_t delta_;
};

class http2_peer_setting_failure final {
public:
    [[nodiscard]] constexpr http2_peer_setting_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_peer_setting_apply_result;

    explicit constexpr http2_peer_setting_failure(http2_peer_setting_error error) noexcept
        : error_(error) {}

    http2_peer_setting_error error_;
};

// Applying one peer setting has exactly one outcome. Ordinary application owns
// no payload, an initial-window change owns its delta, and failure owns only the
// protocol reason. There is no status/changed/delta tuple with invalid mixtures.
class http2_peer_setting_apply_result final {
public:
    [[nodiscard]] constexpr const http2_peer_setting_applied* applied() const& noexcept {
        return std::get_if<http2_peer_setting_applied>(&value_);
    }
    [[nodiscard]] constexpr const http2_peer_setting_applied* applied() const&& = delete;

    [[nodiscard]] constexpr const http2_peer_initial_window_change* initial_window_change()
        const& noexcept {
        return std::get_if<http2_peer_initial_window_change>(&value_);
    }
    [[nodiscard]] constexpr const http2_peer_initial_window_change* initial_window_change() const&& =
        delete;

    [[nodiscard]] constexpr const http2_peer_setting_failure* failure() const& noexcept {
        return std::get_if<http2_peer_setting_failure>(&value_);
    }
    [[nodiscard]] constexpr const http2_peer_setting_failure* failure() const&& = delete;

private:
    friend class http2_peer_settings;

    using value_type = std::variant<http2_peer_setting_applied, http2_peer_initial_window_change,
        http2_peer_setting_failure>;

    template <typename alternative_type>
    explicit constexpr http2_peer_setting_apply_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr http2_peer_setting_apply_result make_applied() noexcept {
        return http2_peer_setting_apply_result(http2_peer_setting_applied());
    }

    [[nodiscard]] static constexpr http2_peer_setting_apply_result make_initial_window_change(
        std::int64_t delta) noexcept {
        return http2_peer_setting_apply_result(http2_peer_initial_window_change(delta));
    }

    [[nodiscard]] static constexpr http2_peer_setting_apply_result make_failure(
        http2_peer_setting_error error) noexcept {
        return http2_peer_setting_apply_result(http2_peer_setting_failure(error));
    }

    value_type value_;
};

struct http2_setting_entry final {
    http2_setting_id id_{http2_setting_id::header_table_size};
    std::uint32_t value_{0};
};

[[nodiscard]] inline bool http2_settings_payload_size_valid(std::string_view payload_value) noexcept {
    return payload_value.size() % 6 == 0;
}

[[nodiscard]] inline http2_setting_entry http2_read_setting_entry(
    std::string_view payload_value, std::size_t offset) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(payload_value.data() + offset);
    return http2_setting_entry{
        .id_ = static_cast<http2_setting_id>(http2_read16(data)), .value_ = http2_read32(data + 2)};
}

[[nodiscard]] inline http2_error_code http2_peer_setting_error_code(
    http2_peer_setting_error error) noexcept {
    return error == http2_peer_setting_error::invalid_initial_window ? http2_error_code::flow_control_error
                                                                     : http2_error_code::protocol_error;
}

[[nodiscard]] inline std::string_view http2_peer_setting_error_message(
    http2_peer_setting_error error) noexcept {
    switch (error) {
        case http2_peer_setting_error::invalid_priority_setting:
            return "invalid NO_RFC7540_PRIORITIES";
        case http2_peer_setting_error::invalid_enable_push:
            return "invalid ENABLE_PUSH";
        case http2_peer_setting_error::invalid_initial_window:
            return "invalid initial window";
        case http2_peer_setting_error::invalid_max_frame_size:
            return "invalid max frame size";
        case http2_peer_setting_error::invalid_enable_connect_protocol:
            return "invalid ENABLE_CONNECT_PROTOCOL";
        case http2_peer_setting_error::invalid_enable_connect_protocol_transition:
            return "invalid ENABLE_CONNECT_PROTOCOL transition";
    }
    return {};
}

class http2_peer_settings final {
public:
    // SETTINGS semantics are directional. In particular, RFC 9113 permits a client
    // to send ENABLE_PUSH=1 but requires a client endpoint to reject that value from
    // a server peer, so the state cannot be constructed without its local role.
    explicit http2_peer_settings(http2_role local_role) noexcept
        : local_role_(local_role) {}

    [[nodiscard]] std::uint32_t max_frame_size() const noexcept {
        return max_frame_size_;
    }

    [[nodiscard]] std::int32_t initial_window_size() const noexcept {
        return initial_window_size_;
    }

    [[nodiscard]] std::uint32_t max_concurrent_streams() const noexcept {
        return max_concurrent_streams_;
    }

    [[nodiscard]] bool enable_connect_protocol() const noexcept {
        return enable_connect_protocol_;
    }

    void complete_frame() noexcept {
        first_frame_ = false;
    }

    [[nodiscard]] bool enable_push() const noexcept {
        return enable_push_;
    }

    // A complete SETTINGS frame is validated against a detached candidate and
    // committed only after every entry has passed. The local role is immutable,
    // so only the peer-controlled values need to be copied into the live state.
    void replace_values_from(const http2_peer_settings& candidate_value) noexcept {
        first_frame_ = candidate_value.first_frame_;
        no_rfc7540_priorities_ = candidate_value.no_rfc7540_priorities_;
        enable_push_ = candidate_value.enable_push_;
        max_frame_size_ = candidate_value.max_frame_size_;
        initial_window_size_ = candidate_value.initial_window_size_;
        max_concurrent_streams_ = candidate_value.max_concurrent_streams_;
        enable_connect_protocol_ = candidate_value.enable_connect_protocol_;
        header_table_size_ = candidate_value.header_table_size_;
        max_header_list_size_ = candidate_value.max_header_list_size_;
    }

    [[nodiscard]] http2_peer_setting_apply_result apply(
        http2_setting_id id, std::uint32_t value) noexcept {
        switch (id) {
            case http2_setting_id::no_rfc7540_priorities:
                if (value > 1 || (!first_frame_ && (value != (no_rfc7540_priorities_ ? 1u : 0u)))) {
                    return http2_peer_setting_apply_result::make_failure(http2_peer_setting_error::invalid_priority_setting);
                }
                no_rfc7540_priorities_ = value != 0;
                return http2_peer_setting_apply_result::make_applied();
            case http2_setting_id::header_table_size:
                header_table_size_ = value;
                return http2_peer_setting_apply_result::make_applied();
            case http2_setting_id::enable_push:
                if (value > 1 || (local_role_ == http2_role::client && value == 1)) {
                    return http2_peer_setting_apply_result::make_failure(
                        http2_peer_setting_error::invalid_enable_push);
                }
                enable_push_ = value != 0;
                return http2_peer_setting_apply_result::make_applied();
            case http2_setting_id::max_concurrent_streams:
                max_concurrent_streams_ = value;
                return http2_peer_setting_apply_result::make_applied();
            case http2_setting_id::initial_window_size: {
                if (!std::in_range<std::int32_t>(value)) {
                    return http2_peer_setting_apply_result::make_failure(
                        http2_peer_setting_error::invalid_initial_window);
                }
                const auto delta = static_cast<std::int64_t>(value) - initial_window_size_;
                initial_window_size_ = static_cast<std::int32_t>(value);
                return http2_peer_setting_apply_result::make_initial_window_change(delta);
            }
            case http2_setting_id::max_frame_size:
                if (value < http2_default_max_frame_size || value > http2_max_frame_size_limit) {
                    return http2_peer_setting_apply_result::make_failure(
                        http2_peer_setting_error::invalid_max_frame_size);
                }
                max_frame_size_ = value;
                return http2_peer_setting_apply_result::make_applied();
            case http2_setting_id::max_header_list_size:
                max_header_list_size_ = value;
                return http2_peer_setting_apply_result::make_applied();
            case http2_setting_id::enable_connect_protocol:
                if (value != 0 && value != 1) {
                    return http2_peer_setting_apply_result::make_failure(
                        http2_peer_setting_error::invalid_enable_connect_protocol);
                }
                if (enable_connect_protocol_ && value == 0) {
                    return http2_peer_setting_apply_result::make_failure(
                        http2_peer_setting_error::invalid_enable_connect_protocol_transition);
                }
                enable_connect_protocol_ = value == 1;
                return http2_peer_setting_apply_result::make_applied();
        }
        return http2_peer_setting_apply_result::make_applied();
    }

private:
    const http2_role local_role_;
    std::uint32_t max_frame_size_{http2_default_max_frame_size};
    // header_table_size_ and max_header_list_size_ are parsed and stored to keep the
    // peer-settings model complete, but no encode path reads them, by design. The
    // encoder never indexes the HPACK dynamic table (static index + literal
    // without indexing only), so the peer's table size cannot be exceeded; and
    // SETTINGS_MAX_HEADER_LIST_SIZE is advisory (RFC 7540 6.5.2) while outbound
    // header blocks are already bounded by the local max_http_header_bytes cap.
    std::uint32_t header_table_size_{4096};
    std::uint32_t max_concurrent_streams_{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t max_header_list_size_{std::numeric_limits<std::uint32_t>::max()};
    std::int32_t initial_window_size_{http2_default_initial_window_size};
    bool first_frame_{true};
    bool no_rfc7540_priorities_{false};
    bool enable_connect_protocol_{false};
    bool enable_push_{local_role_ == http2_role::server};
};

}  // namespace ruvia::detail
