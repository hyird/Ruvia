#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "borrowed_text.h"
#include "decode_step.h"
#include "ip_address.h"

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

// Why parse() rejected its input; each one fails the parser closed.
enum class proxy_protocol_parse_error : std::uint8_t {
    // The first bytes begin neither the v1 nor the v2 signature.
    invalid_signature,
    // Spec §2.1: a CR not followed by LF, or a bare LF.
    v1_invalid_line_ending,
    // Spec §2.1: no CRLF within 107 bytes.
    v1_line_too_long,
    // Not TCP4, TCP6 or UNKNOWN.
    v1_unsupported_protocol,
    // Not four single-space separated fields.
    v1_invalid_fields,
    // Not canonical text of the announced family.
    v1_invalid_address,
    // Not decimal 0..65535 without heading zeroes.
    v1_invalid_port,
    // Spec §2.2: the version nibble is not 2.
    v2_unsupported_version,
    v2_unassigned_command,
    v2_unassigned_address_family,
    v2_unassigned_transport,
    // The length exceeds proxy_protocol_parser_options::max_v2_header_bytes_.
    v2_header_too_large,
    // A PROXY command whose length does not cover its address block.
    v2_short_address_block,
    // A TLV header or value overruns the header.
    v2_tlv_overrun,
};

using proxy_protocol_parse_failure = decode_failure<proxy_protocol_parse_error>;

// One parse() outcome: need more input, a complete header, or a failure.
// consumed_bytes() covers header bytes only: never a byte after the header's
// end, so the caller hands input[consumed_bytes()..] to the next protocol.
// With decode_need_input every input byte was consumed. It is zero for a
// failure, which leaves no byte the caller may reuse.
using proxy_protocol_parse_result = decode_step<decode_need_input, decode_complete, proxy_protocol_parse_failure>;

// Sans-I/O PROXY protocol header parser for the start of one connection, fed
// in chunks split at any byte. The first bytes must begin the v1 "PROXY "
// signature or the 12-byte v2 signature; there is no passthrough of other
// input. Each byte is scanned once and only header bytes are buffered.
// Invalid or over-limit input fails closed: parse() returns the failure and
// the parser becomes failed, as it does when buffering the header throws.
// A finished parser holds a complete header, available through header().
// Only reset() leaves the finished or failed state; it retains small buffers.
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

    [[nodiscard]] decoder_state state() const noexcept {
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
    [[nodiscard]] bool v2_has_known_proxy_family() const noexcept;
    void clear_scratch() noexcept;
    // Marks the parser failed with `error`; the consume loop then stops.
    void reject(proxy_protocol_parse_error error) noexcept;

    // Wire bytes backing the TLV views. Moving the vector preserves storage.
    std::vector<std::byte> buffer_;
    std::vector<proxy_protocol_tlv> tlvs_;
    proxy_protocol_header header_{};
    std::size_t max_v2_header_bytes_;
    std::size_t v2_remaining_{0};
    std::uint8_t v2_family_{0};
    parse_phase phase_{parse_phase::signature};
    decoder_state state_{decoder_state::active};
    // Meaningful only while state_ is failed.
    proxy_protocol_parse_error failure_{proxy_protocol_parse_error::invalid_signature};
};
}  // namespace ruvia
