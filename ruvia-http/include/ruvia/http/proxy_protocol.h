#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/ip_address.h"

namespace ruvia {

// HAProxy PROXY protocol, versions 1 and 2
// (https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt).

enum class proxy_protocol_version : std::uint8_t {
    v1,
    v2,
};

// Spec §2.2 command nibble; a v1 header is always proxy.
enum class proxy_protocol_command : std::uint8_t {
    local,
    proxy,
};

// Spec §2.2 transport protocol nibble. v1 TCP4/TCP6 are stream and UNKNOWN is
// unspecified.
enum class proxy_protocol_transport : std::uint8_t {
    unspecified,
    stream,
    datagram,
};

// One v2 Type-Length-Value vector (spec §2.2). The value borrows the parser.
struct proxy_protocol_tlv final {
    std::uint8_t type_{0};
    std::span<const std::byte> value_{};
};

struct proxy_protocol_parser_options final {
    // The whole v2 header: the 16-byte fixed part, the address block and the
    // TLVs. It must leave room for an AF_INET6 address block (16 + 36 bytes).
    // A v1 line is always capped at its spec §2.1 maximum of 107 bytes
    // including CRLF.
    std::size_t max_v2_header_bytes_{4096};
};

enum class proxy_protocol_parser_state : std::uint8_t {
    active,
    // A complete header was parsed; header() is available.
    finished,
    // The input was not a valid PROXY protocol header, exceeded a limit, or
    // the parser could not buffer it.
    failed,
};

// A parsed header. Its TLV views borrow the parser that produced it.
class proxy_protocol_header final {
public:
    [[nodiscard]] constexpr proxy_protocol_version version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr proxy_protocol_command command() const noexcept {
        return command_;
    }
    [[nodiscard]] constexpr proxy_protocol_transport transport() const noexcept {
        return transport_;
    }
    // A PROXY command with AF_INET/AF_INET6 and a stream or datagram transport
    // (v1 TCP4/TCP6): the original client and the destination it reached.
    // std::nullopt for LOCAL, v1 UNKNOWN, AF_UNSPEC, AF_UNIX and an UNSPEC
    // transport: the receiver keeps the real connection endpoints
    // (spec §2.1, §2.2).
    [[nodiscard]] constexpr std::optional<ip_endpoint> source() const noexcept {
        return source_;
    }
    [[nodiscard]] constexpr std::optional<ip_endpoint> destination() const noexcept {
        return destination_;
    }
    // Every TLV after the address block of a v2 PROXY header with a known
    // address family, raw and in wire order. Empty for v1, for LOCAL (whose
    // block is opaque) and for AF_UNSPEC (whose address length is unknown).
    [[nodiscard]] constexpr std::span<const proxy_protocol_tlv> tlvs() const& noexcept RUVIA_LIFETIMEBOUND {
        return tlvs_;
    }
    std::span<const proxy_protocol_tlv> tlvs() const&& = delete;

private:
    friend class proxy_protocol_parser;

    constexpr proxy_protocol_header() noexcept = default;

    proxy_protocol_version version_{proxy_protocol_version::v1};
    proxy_protocol_command command_{proxy_protocol_command::proxy};
    proxy_protocol_transport transport_{proxy_protocol_transport::unspecified};
    std::optional<ip_endpoint> source_{};
    std::optional<ip_endpoint> destination_{};
    std::span<const proxy_protocol_tlv> tlvs_{};
};

struct proxy_protocol_parse_result final {
    // The header ended within this input.
    bool complete_{false};
    // Header bytes only: never a byte after the header's end, so the caller
    // hands input[consumed_bytes_..] to the next protocol. Without
    // complete_, every input byte was consumed and more are needed.
    std::size_t consumed_bytes_{0};
};

class proxy_protocol_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Sans-I/O PROXY protocol header parser for the start of one connection, fed
// in chunks split at any byte. The first bytes must begin the v1 "PROXY "
// signature or the 12-byte v2 signature; there is no passthrough of other
// input. Each byte is scanned once and only header bytes are buffered.
// Invalid or over-limit input fails closed: parse() throws
// proxy_protocol_error and the parser becomes failed. Only reset() leaves the
// finished or failed state; it keeps buffer capacity, bounded by the v2
// header limit.
class proxy_protocol_parser final {
public:
    // Throws std::invalid_argument when max_v2_header_bytes_ is below 52.
    explicit proxy_protocol_parser(proxy_protocol_parser_options options = {});
    proxy_protocol_parser(const proxy_protocol_parser&) = delete;
    proxy_protocol_parser& operator=(const proxy_protocol_parser&) = delete;
    // The moved-to parser takes over the buffers that header() views borrow.
    proxy_protocol_parser(proxy_protocol_parser&&) noexcept = default;
    proxy_protocol_parser& operator=(proxy_protocol_parser&&) = delete;
    ~proxy_protocol_parser() = default;

    // A parser that is not active throws std::logic_error.
    [[nodiscard]] proxy_protocol_parse_result parse(std::span<const std::byte> input) &;
    proxy_protocol_parse_result parse(std::span<const std::byte> input) && = delete;

    [[nodiscard]] proxy_protocol_parser_state state() const noexcept {
        return state_;
    }
    // Finished parsers only; otherwise throws std::logic_error. Its views stay
    // valid until reset() or destruction.
    [[nodiscard]] const proxy_protocol_header& header() const& RUVIA_LIFETIMEBOUND;
    const proxy_protocol_header& header() const&& = delete;

    void reset() noexcept;

private:
    enum class parse_phase : std::uint8_t {
        signature,
        v1_line,
        v2_fixed,
        v2_block,
    };

    [[nodiscard]] proxy_protocol_parse_result parse_bytes(std::span<const std::byte> input);
    [[nodiscard]] std::size_t consume_signature(std::span<const std::byte> input, std::size_t position);
    [[nodiscard]] std::size_t consume_v1_line(std::span<const std::byte> input, std::size_t position);
    [[nodiscard]] std::size_t consume_v2_fixed(std::span<const std::byte> input, std::size_t position);
    [[nodiscard]] std::size_t consume_v2_block(std::span<const std::byte> input, std::size_t position);
    void finish_v1();
    void finish_v2();

    // Every header byte received so far. A vector, not a string: moving it
    // keeps its storage, so the header's views survive a parser move.
    std::vector<std::byte> buffer_;
    std::vector<proxy_protocol_tlv> tlvs_;
    proxy_protocol_header header_{};
    std::size_t max_v2_header_bytes_;
    // The v2 header size (16 + length), known once the fixed part is complete.
    std::size_t v2_header_size_{0};
    std::uint8_t v2_family_{0};
    parse_phase phase_{parse_phase::signature};
    proxy_protocol_parser_state state_{proxy_protocol_parser_state::active};
};

}  // namespace ruvia

namespace ruvia {

enum class http_forwarded_header_kind : std::uint8_t {
    auto_detect,
    rfc7239_forwarded,
    comma_separated_list,
    single_ip,
};

struct http_forwarded_header_spec final {
    http_forwarded_header_spec() noexcept = default;

    http_forwarded_header_spec(std::string_view name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : name_(name),
          kind_(kind) {}

    http_forwarded_header_spec(const char* name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : http_forwarded_header_spec(std::string_view(name), kind) {}

    http_forwarded_header_spec(const std::string& name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : name_(name),
          kind_(kind) {}

    std::string name_;
    http_forwarded_header_kind kind_{http_forwarded_header_kind::auto_detect};
};

struct http_client_ip_config final {
    http_client_ip_config() noexcept = default;

    bool enabled_{true};
    // Trusted proxy IP literals or CIDR ranges (e.g. "127.0.0.1", "10.0.0.0/8", "::1").
    std::vector<std::string> trusted_proxies_;
    // Priority-ordered forwarding headers. When empty, defaults to
    // {"X-Forwarded-For", "Forwarded"}.
    std::vector<http_forwarded_header_spec> forwarding_headers_;
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto_{false};
};

struct http_client_ip_result final {
    ip_address address_{};
    bool from_forwarded_header_{false};
    std::optional<http_scheme> scheme_{};

    [[nodiscard]] constexpr ip_address address() const noexcept {
        return address_;
    }
    [[nodiscard]] constexpr bool from_forwarded_header() const noexcept {
        return from_forwarded_header_;
    }
    [[nodiscard]] constexpr std::optional<http_scheme> scheme() const noexcept {
        return scheme_;
    }

    friend constexpr bool operator==(const http_client_ip_result&, const http_client_ip_result&) noexcept = default;
};

// Stateless client IP extractor for deployments behind trusted reverse proxies.
class http_client_ip_extractor final {
public:
    http_client_ip_extractor() noexcept
        : enabled_(false) {}

    explicit http_client_ip_extractor(const http_client_ip_config& config);

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return enabled_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !enabled_ || trusted_cidrs_.empty();
    }
    [[nodiscard]] constexpr bool trust_x_forwarded_proto() const noexcept {
        return trust_x_forwarded_proto_;
    }
    [[nodiscard]] bool is_trusted_proxy(const ip_address& address) const noexcept;
    [[nodiscard]] bool is_trusted_proxy(std::string_view address_text) const noexcept;

    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, std::span<const http_header> headers) const noexcept;
    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept;
    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, const http_request& request) const noexcept;

    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, std::span<const http_header> headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, const http_request& request) const noexcept;

private:
    struct compiled_header final {
        std::string name_;
        http_forwarded_header_kind kind_{http_forwarded_header_kind::comma_separated_list};
    };

    std::vector<ip_cidr> trusted_cidrs_;
    std::vector<compiled_header> headers_;
    bool enabled_{true};
    bool trust_x_forwarded_proto_{false};
};

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    std::span<const http_header> headers) noexcept {
    return extractor.resolve(peer_ip, headers);
}

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    std::span<const http_header_view> headers) noexcept {
    return extractor.resolve(peer_ip, headers);
}

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    const http_request& request) noexcept {
    return extractor.resolve(peer_ip, request);
}

}  // namespace ruvia
