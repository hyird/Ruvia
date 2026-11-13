#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http2_observation.h"

namespace ruvia {

[[nodiscard]] constexpr bool is_tls_grease_value(std::uint16_t value) noexcept {
    return (value & 0x0f0fU) == 0x0a0aU && (value >> 8U) == (value & 0xffU);
}

struct tls_client_hello_fingerprints final {
    explicit tls_client_hello_fingerprints(std::pmr::memory_resource* resource = nullptr)
        : ja3_(detail::http_pmr_resource_or_default(resource)),
          ja3_raw_(detail::http_pmr_resource_or_default(resource)),
          ja4_(detail::http_pmr_resource_or_default(resource)),
          peetprint_(detail::http_pmr_resource_or_default(resource)),
          peetprint_hash_(detail::http_pmr_resource_or_default(resource)) {}

    std::pmr::string ja3_;
    std::pmr::string ja3_raw_;
    std::pmr::string ja4_;
    std::pmr::string peetprint_;
    std::pmr::string peetprint_hash_;
};

// Parsed TLS ClientHello handshake fields with GREASE values filtered out.
class tls_client_hello_info final {
public:
    explicit tls_client_hello_info(std::pmr::memory_resource* resource = nullptr)
        : cipher_suites_(detail::http_pmr_resource_or_default(resource)),
          extensions_(detail::http_pmr_resource_or_default(resource)),
          supported_groups_(detail::http_pmr_resource_or_default(resource)),
          ec_point_formats_(detail::http_pmr_resource_or_default(resource)),
          alpn_(detail::http_pmr_resource_or_default(resource)),
          signature_algorithms_(detail::http_pmr_resource_or_default(resource)),
          supported_versions_(detail::http_pmr_resource_or_default(resource)),
          server_name_(detail::http_pmr_resource_or_default(resource)) {}

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
    [[nodiscard]] std::span<const std::uint16_t> elliptic_curves() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_groups_;
    }
    std::span<const std::uint16_t> elliptic_curves() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> ec_point_formats() const& noexcept RUVIA_LIFETIMEBOUND {
        return ec_point_formats_;
    }
    std::span<const std::uint8_t> ec_point_formats() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> point_formats() const& noexcept RUVIA_LIFETIMEBOUND {
        return ec_point_formats_;
    }
    std::span<const std::uint8_t> point_formats() const&& = delete;
    [[nodiscard]] std::span<const std::pmr::string> alpn() const& noexcept RUVIA_LIFETIMEBOUND {
        return alpn_;
    }
    std::span<const std::pmr::string> alpn() const&& = delete;
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
    [[nodiscard]] std::string_view sni() const& noexcept RUVIA_LIFETIMEBOUND {
        return server_name_;
    }
    std::string_view sni() const&& = delete;

    [[nodiscard]] std::pmr::string ja3_raw(std::pmr::memory_resource* resource = nullptr) const;
    [[nodiscard]] std::pmr::string ja3(std::pmr::memory_resource* resource = nullptr) const;
    [[nodiscard]] std::pmr::string ja4(
        bool quic = false, std::pmr::memory_resource* resource = nullptr) const;
    [[nodiscard]] std::pmr::string peetprint(std::pmr::memory_resource* resource = nullptr) const;
    [[nodiscard]] std::pmr::string peetprint_hash(
        std::pmr::memory_resource* resource = nullptr) const;
    [[nodiscard]] tls_client_hello_fingerprints fingerprints(
        bool quic = false, std::pmr::memory_resource* resource = nullptr) const;

    std::uint16_t legacy_version_{0};
    std::pmr::vector<std::uint16_t> cipher_suites_;
    std::pmr::vector<std::uint16_t> extensions_;
    std::pmr::vector<std::uint16_t> supported_groups_;
    std::pmr::vector<std::uint8_t> ec_point_formats_;
    std::pmr::vector<std::pmr::string> alpn_;
    std::pmr::vector<std::uint16_t> signature_algorithms_;
    std::pmr::vector<std::uint16_t> supported_versions_;
    std::pmr::string server_name_;
};

[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::span<const std::byte> message, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::string_view message, std::pmr::memory_resource* resource = nullptr);

[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::span<const std::byte> message, bool quic = false,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::string_view message, bool quic = false,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] inline tls_client_hello_fingerprints compute_tls_client_hello_fingerprints(
    const tls_client_hello_info& hello, bool quic = false,
    std::pmr::memory_resource* resource = nullptr) {
    return hello.fingerprints(quic, resource);
}

struct http2_akamai_fingerprint_options final {
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
    std::pmr::memory_resource* resource_{nullptr};
};

// Collects HTTP/2 frame and HPACK header observations up to the end of the
// client's first request header block and builds the standard Akamai HTTP/2
// fingerprint string: "SETTINGS|WINDOW_UPDATE|PRIORITY|PSEUDO_HEADER_ORDER".
class http2_akamai_fingerprint_collector final {
public:
    explicit http2_akamai_fingerprint_collector(std::pmr::memory_resource* resource = nullptr);
    explicit http2_akamai_fingerprint_collector(http2_akamai_fingerprint_options options);

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

    std::pmr::memory_resource* resource_;
    std::pmr::string settings_;
    std::pmr::string priorities_;
    std::pmr::string pseudo_headers_;
    std::pmr::string fingerprint_;
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

}  // namespace ruvia
