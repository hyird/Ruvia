#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http1_chunked_body.h"
#include "http1_connection_plan.h"
#include "http_header.h"
#include "http_protocol.h"
#include "http_transfer_coding.h"

namespace ruvia {

// Why an HTTP/1 message body could not be decoded, for request and response
// bodies alike.
enum class http1_body_decode_error : std::uint8_t {
    // Malformed chunk framing or trailer section.
    invalid_framing,
    // A chunk-size line, the chunk-ext budget or the trailer section exceeded
    // max_http_header_bytes.
    framing_limit_exceeded,
    // Chunk payload or transfer-decoded content exceeded
    // http1_body_decoder_options::body_limit_.
    body_limit_exceeded,
    // Corrupt coded bytes, bytes after a coding ended, or a coding left
    // unfinished by complete chunked framing.
    invalid_transfer_coding,
    // The input ended before the framing did.
    incomplete_body,
};

// The status a server answers a request whose body failed to decode.
[[nodiscard]] http_protocol_error make_http_protocol_error(http1_body_decode_error error) noexcept;

// The diagnostic a client reports for a response whose body failed to decode.
[[nodiscard]] std::string_view http1_body_decode_error_message(http1_body_decode_error error) noexcept;

// The trailer section that ended a chunked body, possibly empty. The fields
// are validated and owned by the decoder, allocated only for a section that
// keeps a field: names are lowercase, an accepted obs-fold is already replaced
// with SP (RFC 9112 section 5.2), and fields RFC 9110 section 6.5.1 forbids in
// trailers of that message are discarded (RFC 9112 section 7.1.2). They stay
// valid until the next decode(), finish_input() or reset() call, or until the
// decoder is destroyed.
class http1_body_trailers final {
public:
    [[nodiscard]] constexpr std::span<const http_header> fields() const noexcept {
        return fields_;
    }

private:
    friend class http1_body_decoder;
    explicit constexpr http1_body_trailers(std::span<const http_header> fields) noexcept :
        fields_(fields) {}
    std::span<const http_header> fields_;
};

using http1_body_decode_failure = decode_failure<http1_body_decode_error>;

// One decode() or finish_input() outcome: more input needed, content bytes,
// the trailer section of a chunked body (reported once, before the end), the
// end of the body, or a failure.
using http1_body_decode_result = decode_step<decode_need_input, decode_output, http1_body_trailers, decode_complete, http1_body_decode_failure>;

// What differs between the two roles that receive an HTTP/1 message body.
struct http1_body_decoder_options final {
    // Bounds the chunk payload bytes of a chunked body and, independently, the
    // output of each transfer-decoding layer. A known length is bounded by its
    // caller before the decoder is constructed.
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_trailer_obs_fold obs_fold_{http1_trailer_obs_fold::reject};
    http1_trailer_message trailer_message_{http1_trailer_message::request};
};

// Incremental sans-I/O decoder for the body one HTTP/1 framing delimits, for
// requests and responses alike: it removes the framing and then the transfer
// codings the framing names. A server constructs it from
// http1_request_body_plan::framing(), a client from
// http1_client_response_plan::framing() with obs_fold_ = accept and
// trailer_message_ = response. Known-length content is forwarded without a
// transfer-decoding stage.
// A finished or failed decoder consumes no further input: decode() and
// finish_input() repeat the terminal result, never touching bytes of a
// following message, until reset().
// A transfer codec exception propagates unchanged and leaves the body decoder
// failed with invalid_transfer_coding, even if content is later discarded.
class http1_body_decoder final {
public:
    // Throws std::invalid_argument for transfer codings that are not transfer
    // codings, and http_content_codec_error when a codec cannot start.
    explicit http1_body_decoder(const http1_message_framing& framing, http1_body_decoder_options config = {});
    ~http1_body_decoder() = default;
    http1_body_decoder(const http1_body_decoder&) = delete;
    http1_body_decoder& operator=(const http1_body_decoder&) = delete;
    http1_body_decoder(http1_body_decoder&&) = delete;
    http1_body_decoder& operator=(http1_body_decoder&&) = delete;

    // Output borrows `available` when the framing names no transfer coding and
    // `scratch` otherwise; consume it before changing either storage or making
    // the next call. `scratch` must be non-empty only while actively decoding
    // transfer-coded content. One step publishes at most max_output_bytes,
    // which must be positive. Erase exactly consumed_bytes() from `available`.
    // After finish_input(), decode() continues with the same EOF state: it can
    // drain buffered input/output, but missing body bytes are an
    // incomplete_body failure rather than a request for more input.
    // If allocating trailers throws, retry with the same input. No wire bytes
    // are reported as consumed until a step returns a result.
    [[nodiscard]] http1_body_decode_result decode(std::string_view available RUVIA_LIFETIMEBOUND, std::span<char> scratch RUVIA_LIFETIMEBOUND, std::size_t max_output_bytes = (std::numeric_limits<std::size_t>::max)()) & RUVIA_LIFETIMEBOUND;
    http1_body_decode_result decode(std::string_view, std::span<char>, std::size_t = 0) && = delete;
    // EOF is monotonic. Supply any remaining buffered bytes; later calls may
    // drain decoder output but can never request more network input.
    [[nodiscard]] http1_body_decode_result finish_input(std::string_view available RUVIA_LIFETIMEBOUND, std::span<char> scratch RUVIA_LIFETIMEBOUND, std::size_t max_output_bytes = (std::numeric_limits<std::size_t>::max)()) & RUVIA_LIFETIMEBOUND;
    http1_body_decode_result finish_input(std::string_view, std::span<char>, std::size_t = 0) && = delete;
    template <detail::http_temporary_owning_char_string input_type>
    http1_body_decode_result decode(input_type&&, std::span<char>, std::size_t = 0) & = delete;
    template <detail::http_temporary_owning_char_string input_type>
    http1_body_decode_result finish_input(input_type&&, std::span<char>, std::size_t = 0) & = delete;

    // From now on the content the framing delimits is consumed without
    // transfer decoding and never published: a step reports only need-input,
    // trailers, the end or a framing failure. For content a recipient must not
    // use (a 205 response, RFC 9110 section 15.3.6) or no longer wants (an
    // unread request body) while the connection stays reusable.
    void discard_content() noexcept;

    [[nodiscard]] decoder_state state() const noexcept;
    // Extracts owned trailers after decode() or finish_input() reported
    // http1_body_trailers, avoiding a copy into http_request_trailers.
    [[nodiscard]] std::vector<http_header> take_trailers() & noexcept;
    std::vector<http_header> take_trailers() && = delete;
    // Starts another body with the constructed framing and configuration.
    // Allocation failure while recreating discarded codecs preserves the
    // previous body's state and progress.
    // Throws http_content_codec_error when a codec cannot restart; the decoder
    // is then unusable.
    void reset();

private:
    enum class transfer_phase_type : std::uint8_t {
        needs_framed_input,
        drain_output,
        ended,
    };

    enum class chunked_phase_type : std::uint8_t {
        reading,
        trailers_published,
        finished,
    };

    void start();
    [[nodiscard]] http1_body_decode_result run(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof);
    [[nodiscard]] http1_body_decode_result step(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof);
    [[nodiscard]] http1_body_decode_result step_content(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof);
    [[nodiscard]] http1_body_decode_result step_known_length(std::string_view available, std::size_t max_output_bytes, bool eof);
    [[nodiscard]] http1_body_decode_result step_close_delimited(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof);
    [[nodiscard]] http1_body_decode_result step_chunked(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof);
    [[nodiscard]] std::optional<http1_body_decode_result> drain_pending_chunk(std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof);
    void consume_pending_delimiter(std::string_view available, std::size_t& consumed) noexcept;
    [[nodiscard]] http1_body_decode_result need_input_or_incomplete(std::size_t consumed, bool eof) noexcept;
    [[nodiscard]] http1_body_decode_result emit_transfer_output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] http1_body_decode_result complete(std::size_t consumed) noexcept;
    [[nodiscard]] http1_body_decode_result fail(http1_body_decode_error error, std::size_t consumed) noexcept;
    [[nodiscard]] http1_body_decode_result drive_transfer(std::string_view input, std::size_t wire_prefix, std::span<char> scratch);
    [[nodiscard]] http1_body_decode_result finish_transfer(std::size_t consumed, std::span<char> scratch, http1_body_decode_error unfinished);

    http1_message_framing framing_;
    http1_body_decoder_options config_;
    std::optional<http_transfer_coding_stack_decoder> transfer_;
    std::optional<http1_chunked_body_decoder> chunked_;
    decoder_state state_{decoder_state::active};
    // Meaningful only while state_ is failed.
    http1_body_decode_error error_{http1_body_decode_error::invalid_framing};
    std::uint64_t remaining_{0};
    // The chunk parser advances through a whole body view. These counters let
    // the caller retain its unconsumed wire suffix without storing a borrowed view.
    std::size_t pending_body_bytes_{0};
    std::size_t pending_delimiter_bytes_{0};
    // Wire bytes consumed by framing or transfer decoding before an unreported
    // trailer allocation failure. Retry at this offset without replaying them.
    std::size_t chunk_retry_prefix_{0};
    bool discard_content_{false};
    bool eof_{false};
    chunked_phase_type chunked_phase_{chunked_phase_type::reading};
    transfer_phase_type transfer_phase_{transfer_phase_type::needs_framed_input};
};

}  // namespace ruvia
