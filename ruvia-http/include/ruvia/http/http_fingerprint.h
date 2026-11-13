#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http3_connection.h"

namespace ruvia {

[[nodiscard]] constexpr bool is_tls_grease_value(std::uint16_t value) noexcept {
    return (value & 0x0f0fU) == 0x0a0aU && (value >> 8U) == (value & 0xffU);
}

struct tls_client_hello_fingerprints final {
    std::string ja3_;
    std::string ja3_raw_;
    std::string ja4_;
    std::string peetprint_;
    std::string peetprint_hash_;
};

// Parsed TLS ClientHello handshake fields with GREASE values filtered out.
class tls_client_hello_info final {
public:
    tls_client_hello_info() noexcept = default;
    [[nodiscard]] constexpr std::uint16_t legacy_version() const noexcept {
        return legacy_version_;
    }
    [[nodiscard]] std::uint16_t version() const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> cipher_suites() const& noexcept RUVIA_LIFETIMEBOUND {
        return cipher_suites_;
    }
    std::span<const std::uint16_t> cipher_suites() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> extensions() const& noexcept RUVIA_LIFETIMEBOUND {
        return extensions_;
    }
    std::span<const std::uint16_t> extensions() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_groups() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_groups_;
    }
    std::span<const std::uint16_t> supported_groups() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> ec_point_formats() const& noexcept RUVIA_LIFETIMEBOUND {
        return ec_point_formats_;
    }
    std::span<const std::uint8_t> ec_point_formats() const&& = delete;
    [[nodiscard]] std::span<const std::string> alpn() const& noexcept RUVIA_LIFETIMEBOUND {
        return alpn_;
    }
    std::span<const std::string> alpn() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> signature_algorithms() const& noexcept RUVIA_LIFETIMEBOUND {
        return signature_algorithms_;
    }
    std::span<const std::uint16_t> signature_algorithms() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_versions() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_versions_;
    }
    std::span<const std::uint16_t> supported_versions() const&& = delete;
    [[nodiscard]] std::string_view server_name() const& noexcept RUVIA_LIFETIMEBOUND {
        return server_name_;
    }
    std::string_view server_name() const&& = delete;

    [[nodiscard]] std::string ja3_raw() const;
    [[nodiscard]] std::string ja3() const;
    [[nodiscard]] std::string ja4(bool quic = false) const;
    [[nodiscard]] std::string peetprint() const;
    [[nodiscard]] std::string peetprint_hash() const;
    [[nodiscard]] tls_client_hello_fingerprints fingerprints(bool quic = false) const;

    std::uint16_t legacy_version_{0};
    std::vector<std::uint16_t> cipher_suites_;
    std::vector<std::uint16_t> extensions_;
    std::vector<std::uint16_t> supported_groups_;
    std::vector<std::uint8_t> ec_point_formats_;
    std::vector<std::string> alpn_;
    std::vector<std::uint16_t> signature_algorithms_;
    std::vector<std::uint16_t> supported_versions_;
    std::string server_name_;
};

[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::span<const std::byte> message);
[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::string_view message);

[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::span<const std::byte> message, bool quic = false);
[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::string_view message, bool quic = false);
[[nodiscard]] inline tls_client_hello_fingerprints compute_tls_client_hello_fingerprints(
    const tls_client_hello_info& hello, bool quic = false) {
    return hello.fingerprints(quic);
}

struct http2_akamai_fingerprint_options final {
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
};

// Collects HTTP/2 frame and HPACK header observations up to the end of the
// client's first request header block and builds the standard Akamai HTTP/2
// fingerprint string: "SETTINGS|WINDOW_UPDATE|PRIORITY|PSEUDO_HEADER_ORDER".
class http2_akamai_fingerprint_collector final {
public:
    explicit http2_akamai_fingerprint_collector(http2_akamai_fingerprint_options options = {});

    void observe_frame(const http2_frame_metadata& frame) noexcept;
    void observe_header(const http2_header_metadata& header) noexcept;
    void reset() noexcept;

    static void on_frame(void* context, const http2_frame_metadata& frame) noexcept {
        static_cast<http2_akamai_fingerprint_collector*>(context)->observe_frame(frame);
    }
    static void on_header(void* context, const http2_header_metadata& header) noexcept {
        static_cast<http2_akamai_fingerprint_collector*>(context)->observe_header(header);
    }

    [[nodiscard]] constexpr bool complete() const noexcept {
        return phase_ == phase::complete;
    }
    [[nodiscard]] constexpr bool failed() const noexcept {
        return phase_ == phase::failed;
    }
    [[nodiscard]] std::string_view fingerprint() const& noexcept RUVIA_LIFETIMEBOUND {
        return phase_ == phase::complete ? std::string_view(fingerprint_) : std::string_view{};
    }
    std::string_view fingerprint() const&& = delete;

private:
    enum class phase : std::uint8_t {
        collecting,
        complete,
        failed,
    };

    std::string settings_;
    std::string priorities_;
    std::string pseudo_headers_;
    std::string fingerprint_;
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
    std::size_t frames_seen_{0};
    std::size_t bytes_seen_{0};
    std::uint32_t window_update_{0};
    std::uint32_t first_request_stream_id_{0};
    bool has_window_update_{false};
    bool saw_regular_header_{false};
    phase phase_{phase::collecting};
};

struct http3_fingerprint_options final {
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
};

// Collects HTTP/3 frame and QPACK field observations up to the end of the
// client's first request header block and builds a deterministic HTTP/3
// fingerprint string: "CONTROL_FRAMES|FIRST_STREAM_ID|PSEUDO_HEADER_ORDER".
class http3_fingerprint_collector final {
public:
    explicit http3_fingerprint_collector(http3_fingerprint_options options = {});

    void observe_frame(const http3_frame_metadata& frame) noexcept;
    void observe_field(const http3_field_metadata& field) noexcept;
    void reset() noexcept;

    static void on_frame(void* context, const http3_frame_metadata& frame) noexcept {
        static_cast<http3_fingerprint_collector*>(context)->observe_frame(frame);
    }
    static void on_field(void* context, const http3_field_metadata& field) noexcept {
        static_cast<http3_fingerprint_collector*>(context)->observe_field(field);
    }

    [[nodiscard]] constexpr bool complete() const noexcept {
        return phase_ == phase::complete;
    }
    [[nodiscard]] constexpr bool failed() const noexcept {
        return phase_ == phase::failed;
    }
    [[nodiscard]] std::string_view fingerprint() const& noexcept RUVIA_LIFETIMEBOUND {
        return phase_ == phase::complete ? std::string_view(fingerprint_) : std::string_view{};
    }
    std::string_view fingerprint() const&& = delete;

private:
    enum class phase : std::uint8_t {
        collecting,
        complete,
        failed,
    };

    std::string control_frames_;
    std::string pseudo_headers_;
    std::string fingerprint_;
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
    std::size_t frames_seen_{0};
    std::size_t bytes_seen_{0};
    std::int64_t first_request_stream_id_{-1};
    bool saw_regular_header_{false};
    phase phase_{phase::collecting};
};
}  // namespace ruvia
