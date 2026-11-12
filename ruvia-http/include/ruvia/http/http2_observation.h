#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_types.h"

namespace ruvia {

// A complete received frame, before its final payload fragment is processed.
// observation_sequence_ starts at zero after the preface and counts frame
// headers exposed by nghttp2, in wire order (including ACK, unknown extensions,
// PRIORITY and CONTINUATION). Frames discarded before nghttp2's begin-frame
// callback (for example DATA on an already reset stream) are not exposed or
// counted. Neither transport reads nor
// the preface are counted. Observation does not imply acceptance: protocol
// validation and mandatory rejection still run unchanged afterwards.
struct http2_frame_metadata final {
    std::uint64_t observation_sequence_{0};
    std::uint32_t payload_length_{0};
    std::uint32_t stream_id_{0};
    http2_frame_type type_{http2_frame_type::data};
    // Protocol flags exposed by nghttp2; undefined/reserved flag bits may have
    // been removed by that parser. Not a second interpretation of wire bytes.
    std::uint8_t flags_{0};
    // Raw SETTINGS entries in wire order, including unknown and duplicate IDs.
    // ACK SETTINGS have no entries. Oversize SETTINGS are still rejected by the
    // protocol core; settings_complete_ is false if the fixed observation budget
    // cannot represent the payload, or its size is not a multiple of six.
    std::array<http2_setting_entry, http2_received_settings::max_frame_entries> settings_{};
    std::size_t settings_count_{0};
    bool settings_complete_{true};
    std::optional<std::uint32_t> window_increment_{};
    // HEADERS with PRIORITY and standalone PRIORITY, never a scheduling policy.
    std::optional<http2_received_priority> priority_{};

    [[nodiscard]] std::span<const http2_setting_entry> settings() const& noexcept RUVIA_LIFETIMEBOUND {
        return {settings_.data(), settings_count_};
    }
    std::span<const http2_setting_entry> settings() const&& = delete;
};

enum class http2_header_observation_phase : std::uint8_t { begin,
    field,
    end };

// HPACK decoder facts, not reconstructed request fields. begin is delivered
// before HTTP field validation; field follows HPACK emission and precedes HTTP
// validation, including for a field that causes rejection. end means HPACK
// completed (not that HTTP semantics accepted the section). Rejected sections
// may have no end and nghttp2 may suppress their remaining field callbacks.
// HEADERS/PUSH_PROMISE starts a section; CONTINUATION does not start a new one.
// name_/value_ borrow decoder storage only for this callback. Empty on begin/end.
struct http2_header_metadata final {
    std::uint64_t first_frame_observation_sequence_{0};
    std::uint64_t frame_observation_sequence_{0};
    std::uint32_t stream_id_{0};
    std::size_t field_index_{0};
    http2_header_observation_phase phase_{http2_header_observation_phase::begin};
    std::string_view name_{};
    std::string_view value_{};
};

// Invoked synchronously inside feed(), never from next_event(). Observers must
// not mutate, re-enter, move or destroy this connection until feed() returns.
// The context is borrowed for the lifetime of the connection. nullptr disables
// its observer with no input retention, extra allocation or framing work.
using http2_frame_observer_type = void (*)(void*, const http2_frame_metadata&) noexcept;
using http2_header_observer_type = void (*)(void*, const http2_header_metadata&) noexcept;

}  // namespace ruvia
