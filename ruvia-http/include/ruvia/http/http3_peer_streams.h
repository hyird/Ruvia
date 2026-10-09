#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {

enum class http3_peer_role : std::uint8_t {
    client,
    server,
};

enum class http3_stream_id_type : std::uint8_t {
    client_bidirectional,
    server_bidirectional,
    client_unidirectional,
    server_unidirectional,
};

// Classifies the RFC 9000 stream-ID initiator/direction bits and rejects
// values outside QUIC's 62-bit stream-ID range.
[[nodiscard]] inline constexpr std::optional<http3_stream_id_type> http3_stream_id_type(
    std::uint64_t stream_id) noexcept {
    if (stream_id > http3_var_int_max) {
        return std::nullopt;
    }
    switch (stream_id & 3U) {
        case 0:
            return http3_stream_id_type::client_bidirectional;
        case 1:
            return http3_stream_id_type::server_bidirectional;
        case 2:
            return http3_stream_id_type::client_unidirectional;
        case 3:
            return http3_stream_id_type::server_unidirectional;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr bool is_http3_request_stream_id(std::uint64_t stream_id) noexcept {
    return http3_stream_id_type(stream_id) == http3_stream_id_type::client_bidirectional;
}

[[nodiscard]] inline constexpr bool is_http3_unidirectional_stream_id(std::uint64_t stream_id) noexcept {
    const auto type = http3_stream_id_type(stream_id);
    return type == http3_stream_id_type::client_unidirectional ||
           type == http3_stream_id_type::server_unidirectional;
}

[[nodiscard]] inline constexpr bool is_http3_client_unidirectional_stream_id(
    std::uint64_t stream_id) noexcept {
    return http3_stream_id_type(stream_id) == http3_stream_id_type::client_unidirectional;
}

[[nodiscard]] inline constexpr bool is_http3_peer_unidirectional_stream_id(
    http3_peer_role local_role, std::uint64_t stream_id) noexcept {
    return local_role == http3_peer_role::client
               ? http3_stream_id_type(stream_id) == http3_stream_id_type::server_unidirectional
               : is_http3_client_unidirectional_stream_id(stream_id);
}

[[nodiscard]] inline constexpr bool is_http3_peer_bidirectional_stream_id(
    http3_peer_role local_role, std::uint64_t stream_id) noexcept {
    // HTTP/3 clients do not accept server-initiated bidirectional streams.
    return local_role == http3_peer_role::server && is_http3_request_stream_id(stream_id);
}

enum class http3_peer_stream_kind : std::uint8_t {
    unclassified,
    control,
    push,
    qpack_encoder,
    qpack_decoder,
    unknown,
};
enum class http3_peer_stream_error : std::uint8_t {
    stream_creation_error,
    closed_critical_stream,
    excessive_load,
};

struct http3_peer_stream_feed final {
    http3_peer_stream_kind kind_{http3_peer_stream_kind::unclassified};
    std::uint64_t stream_type_{0};
    // Number of input bytes consumed by the classifier. `remaining` borrows the
    // unconsumed bytes from this call; the caller owns their subsequent parsing.
    std::size_t consumed_{0};
    std::span<const char> remaining_{};
    bool fin_{false};
    bool reset_{false};
    bool closed_{false};
};

struct http3_peer_stream_limits final {
    std::size_t max_active_streams_{128};
};

// Connection-local classifier for streams initiated by the QUIC peer. It owns
// only partial type-varint state; payload bytes are always borrowed by the caller.
class http3_peer_streams final {
public:
    explicit http3_peer_streams(http3_peer_role local_role, std::pmr::memory_resource* resource,
        http3_peer_stream_limits limits = {});

    [[nodiscard]] std::variant<http3_peer_stream_feed, http3_peer_stream_error> feed(
        std::uint64_t stream_id, std::span<const char> bytes, bool fin = false,
        bool reset = false);

    // Validate a peer-initiated bidirectional stream. HTTP/3 servers accept
    // client request streams; clients reject server-initiated bidi streams.
    [[nodiscard]] static std::variant<std::monostate, http3_peer_stream_error> accept_bidirectional(
        http3_peer_role local_role, std::uint64_t stream_id) noexcept;

    // Called only after transport retirement; critical streams cannot retire
    // independently of the connection.
    [[nodiscard]] bool retire_non_critical_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] std::size_t active_stream_count() const noexcept;

private:
    struct state_type final {
        std::array<char, 8> type_bytes_{};
        std::size_t type_size_{0};
        http3_peer_stream_kind kind_{http3_peer_stream_kind::unclassified};
        std::uint64_t stream_type_{0};
    };

    [[nodiscard]] std::variant<std::monostate, http3_peer_stream_error> validate_peer_uni(
        std::uint64_t stream_id) const noexcept;
    [[nodiscard]] static bool is_critical(http3_peer_stream_kind kind) noexcept;

    http3_peer_role local_role_;
    http3_peer_stream_limits limits_;
    std::pmr::unordered_map<std::uint64_t, state_type> streams_;
    bool control_seen_{false};
    bool encoder_seen_{false};
    bool decoder_seen_{false};
};

}  // namespace ruvia
