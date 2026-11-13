#include "ruvia/http/http1_chunked_body_decoder.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/url_encoding.h"

#include "http_websocket_permessage_deflate.h"
#include "http_zlib_stream.h"

namespace ruvia {

namespace {

struct http_chunk_size_line final {
    std::size_t size_{0};
    // Line bytes beyond the shortest hex spelling of size_: leading zeros and
    // chunk-ext. A decoder charges them to a per-message framing budget.
    std::size_t overhead_bytes_{0};
};

bool is_valid_http_chunk_extension(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    std::size_t cursor_value = 0;
    const auto skip_bws = [&value, &cursor_value]() noexcept {
        while (cursor_value < value.size() && is_http_ows_char(value[cursor_value])) {
            ++cursor_value;
        }
    };
    const auto parse_token = [&value, &cursor_value]() noexcept {
        const auto begin = cursor_value;
        while (
            cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        return cursor_value != begin;
    };

    while (cursor_value < value.size()) {
        skip_bws();
        if (cursor_value == value.size()) {
            // Reached end-of-line after consuming whitespace with no ";": the
            // chunk-ext grammar (RFC 9112 7.1) permits BWS only before ";"/"=",
            // never as trailing space before CRLF. Accepting "5 " is a
            // request-smuggling differential of the same class as the leading-OWS
            // case that parse_http_chunk_size_line already rejects.
            return false;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (!parse_token()) {
            return false;
        }
        const auto after_name = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // Bare ext-name at end-of-line: valid only with no trailing BWS
            // between the name and the CRLF (RFC 9112 7.1).
            return cursor_value == after_name;
        }
        if (value[cursor_value] == ';') {
            continue;
        }
        if (value[cursor_value] != '=') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            return false;
        }
        if (value[cursor_value] == '"') {
            const auto quoted_size = http_quoted_string_size(value.substr(cursor_value));
            if (quoted_size == 0) {
                return false;
            }
            cursor_value += quoted_size;
        } else if (!parse_token()) {
            return false;
        }
        const auto before_trailing_bws = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // End-of-line after a chunk-ext value is valid only if it lands
            // exactly on the value; whitespace between the value and CRLF is
            // rejected for the same reason as trailing space after the size.
            return cursor_value == before_trailing_bws;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
    }
    return true;
}

// Parses `chunk-size [ chunk-ext ]` (RFC 9112 7.1) without its CRLF; nullopt on
// any malformed or overflowing line.
std::optional<http_chunk_size_line> parse_http_chunk_size_line(std::string_view value) noexcept {
    // RFC 9112: chunk-size starts at the first byte of the line. Leading OWS
    // before the size is a known request-smuggling vector and is rejected,
    // matching picohttpparser/llhttp strict parsing.
    std::size_t cursor_value = 0;
    std::size_t leading_zero_digits = 0;
    std::size_t parsed_value = 0;
    constexpr auto max_before_shift = std::numeric_limits<std::size_t>::max() >> 4U;
    while (cursor_value < value.size()) {
        const int nibble = http_decode_hex_nibble(value[cursor_value]);
        if (nibble < 0) {
            break;
        }
        if (parsed_value > max_before_shift) {
            return std::nullopt;
        }
        if (parsed_value == 0 && nibble == 0) {
            ++leading_zero_digits;
        }
        parsed_value = (parsed_value << 4U) | static_cast<std::size_t>(nibble);
        ++cursor_value;
    }
    if (cursor_value == 0 || !is_valid_http_chunk_extension(value.substr(cursor_value))) {
        return std::nullopt;
    }

    // The shortest spelling of a size drops its leading zeros but keeps one
    // digit for a zero size.
    const auto significant_digits = std::max<std::size_t>(cursor_value - leading_zero_digits, 1);
    return http_chunk_size_line{.size_ = parsed_value, .overhead_bytes_ = value.size() - significant_digits};
}

// The HTTP/1 received trailer-block field parser. `block` holds the field
// lines between the last chunk and the empty line that ends the trailer
// section. Values are exposed with field-line OWS trimmed.
// `obs_fold` decides whether a line starting with OWS continues the previous
// field (RFC 9112 section 5.2). Rejected, such a line is malformed. Accepted,
// the field is validated and charged as its HTTP unfolding (each obs-fold and
// adjacent OWS is one SP), and the visitor receives the received value span
// with its folds in place. Every field is charged to the section budget;
// syntax errors and the budget fail the whole section. The visitor receives
// views borrowing `block`.
template <typename visitor_type>
[[nodiscard]] bool visit_http1_trailer_block(
    std::string_view block, http1_trailer_obs_fold obs_fold, visitor_type&& visitor) {
    auto budget = http_trailer_section_budget();
    const auto line_at = [block](std::size_t begin, std::size_t end) noexcept {
        return end == std::string_view::npos ? block.substr(begin) : block.substr(begin, end - begin);
    };
    std::size_t cursor_value = 0;
    while (cursor_value < block.size()) {
        auto line_end = block.find("\r\n", cursor_value);
        const auto line = line_at(cursor_value, line_end);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return false;
        }
        const auto name = line.substr(0, colon);
        // The unfolded value spans the first to the last non-empty piece, each
        // fold between them counting as one SP.
        auto piece = http_trim_ows(line.substr(colon + 1));
        auto value = piece;
        std::size_t value_bytes = 0;
        std::size_t pending_separators = 0;
        for (;;) {
            if (!is_valid_http_field_value_bytes(piece)) {
                return false;
            }
            if (!piece.empty()) {
                if (value_bytes == 0) {
                    value = piece;
                } else {
                    value_bytes += pending_separators;
                    value = std::string_view(
                        value.data(), static_cast<std::size_t>(piece.data() - value.data()) + piece.size());
                }
                value_bytes += piece.size();
                pending_separators = 0;
            }
            if (obs_fold == http1_trailer_obs_fold::reject || line_end == std::string_view::npos ||
                line_end + 2 >= block.size() || !is_http_ows_char(block[line_end + 2])) {
                break;
            }
            ++pending_separators;
            const auto piece_begin = line_end + 2;
            line_end = block.find("\r\n", piece_begin);
            piece = http_trim_ows(line_at(piece_begin, line_end));
        }
        if (!budget.add_field(name.size(), value_bytes)) {
            return false;
        }
        visitor(name, value);
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor_value = line_end + 2;
    }
    return true;
}

}  // namespace

http1_chunk_decode_result http1_chunk_decode_result::make_need_more(std::size_t consumed_bytes) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_need_more(consumed_bytes));
}

http1_chunk_decode_result http1_chunk_decode_result::make_body_chunk(
    std::size_t consumed_bytes, std::string_view bytes_value) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_body_chunk_view(consumed_bytes, bytes_value));
}

http1_chunk_decode_result http1_chunk_decode_result::make_complete(
    std::size_t consumed_bytes, std::span<const http_header> trailers) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_complete_view(consumed_bytes, trailers));
}

http1_chunk_decode_result http1_chunk_decode_result::make_failure(
    std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_failure(consumed_bytes, error));
}

http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder_config config)
    : body_limit_(config.body_limit_),
      obs_fold_(config.obs_fold_),
      trailer_message_(config.trailer_message_) {}

http1_chunked_body_decoder::~http1_chunked_body_decoder() = default;
http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept = default;
http1_chunked_body_decoder& http1_chunked_body_decoder::operator=(http1_chunked_body_decoder&&) noexcept = default;

http1_chunk_decode_result http1_chunked_body_decoder::decode(std::string_view available) {
    return decode(available, std::numeric_limits<std::size_t>::max());
}

http1_chunk_decode_result http1_chunked_body_decoder::decode(
    std::string_view available, std::size_t max_body_bytes) {
    if (max_body_bytes == 0) {
        throw std::invalid_argument("HTTP/1 chunk decode body quota must be greater than zero");
    }
    if (const auto* error = std::get_if<http1_chunk_decode_error>(&state_)) {
        return http1_chunk_decode_result::make_failure(0, *error);
    }
    // Published trailers live until this next call.
    trailers_.clear();

    std::size_t cursor = 0;
    for (;;) {
        switch (std::get<progress>(state_)) {
            case progress::size_line: {
                // Every LF must end a CRLF (RFC 9112 section 7.1), so a bare LF is
                // rejected as soon as it arrives instead of waiting for a CRLF.
                const auto line = available.substr(cursor, max_http_header_bytes);
                const auto line_feed = line.find('\n', size_line_search_offset_);
                if (line_feed == std::string_view::npos) {
                    const auto line_bytes = line.size();
                    if (line_bytes >= max_http_header_bytes) {
                        return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                    }
                    size_line_search_offset_ = line_bytes;
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                if (line_feed == 0 || line[line_feed - 1] != '\r') {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                size_line_search_offset_ = 0;
                const auto line_end = line_feed - 1;
                const auto size_line = parse_http_chunk_size_line(line.substr(0, line_end));
                if (!size_line) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                // RFC 9112 section 7.1.1: a recipient ought to limit the total
                // chunk-ext length of a message; leading zeros cost the same scan.
                if (max_http_header_bytes - framing_overhead_bytes_ < size_line->overhead_bytes_) {
                    return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                }
                framing_overhead_bytes_ += size_line->overhead_bytes_;
                cursor += line_end + 2;
                if (size_line->size_ == 0) {
                    state_ = progress::trailers;
                    trailer_search_offset_ = 0;
                } else {
                    if (body_limit_.addition_exceeds(decoded_bytes_, size_line->size_)) {
                        return fail(cursor, http1_chunk_decode_error::body_limit_exceeded);
                    }
                    decoded_bytes_ += size_line->size_;
                    remaining_ = size_line->size_;
                    state_ = progress::body;
                }
                break;
            }
            case progress::body: {
                const auto bytes_value = std::min({remaining_, available.size() - cursor, max_body_bytes});
                if (bytes_value == 0) {
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                const auto body = available.substr(cursor, bytes_value);
                remaining_ -= bytes_value;
                cursor += bytes_value;
                if (remaining_ == 0) {
                    state_ = progress::delimiter;
                    // A buffered delimiter is consumed with the final data bytes,
                    // so consumed_bytes covers the whole chunk.
                    if (available.size() - cursor >= 2 && !consume_delimiter(available, cursor)) {
                        return fail(cursor, http1_chunk_decode_error::invalid_framing);
                    }
                }
                return http1_chunk_decode_result::make_body_chunk(cursor, body);
            }
            case progress::delimiter:
                if (available.size() - cursor < 2) {
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                if (!consume_delimiter(available, cursor)) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                break;
            case progress::trailers: {
                const auto trailers = available.substr(cursor, max_http_header_bytes);
                if (trailers.starts_with("\r\n")) {
                    state_ = progress::complete;
                    return http1_chunk_decode_result::make_complete(cursor + 2);
                }
                // Scan only bytes not examined by an earlier call. Each LF must be
                // preceded by CR; an LF directly after a CRLF ends the section.
                auto trailer_end = std::string_view::npos;
                for (auto line_feed = trailers.find('\n', trailer_search_offset_);
                    line_feed != std::string_view::npos; line_feed = trailers.find('\n', line_feed + 1)) {
                    if (line_feed == 0 || trailers[line_feed - 1] != '\r') {
                        return fail(cursor, http1_chunk_decode_error::invalid_framing);
                    }
                    if (line_feed >= 3 && trailers[line_feed - 2] == '\n') {
                        trailer_end = line_feed - 3;
                        break;
                    }
                }
                if (trailer_end == std::string_view::npos) {
                    if (trailers.size() >= max_http_header_bytes) {
                        return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                    }
                    trailer_search_offset_ = trailers.size();
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                const auto trailer_bytes = trailer_end + 4;
                if (!publish_trailers(trailers.substr(0, trailer_end))) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                state_ = progress::complete;
                return http1_chunk_decode_result::make_complete(cursor + trailer_bytes, trailers_);
            }
            case progress::complete:
                return http1_chunk_decode_result::make_complete(cursor);
        }
    }
}

// Requires both delimiter bytes to be buffered at cursor.
bool http1_chunked_body_decoder::consume_delimiter(std::string_view available, std::size_t& cursor) noexcept {
    if (!available.substr(cursor).starts_with("\r\n")) {
        return false;
    }
    cursor += 2;
    state_ = progress::size_line;
    return true;
}

bool http1_chunked_body_decoder::publish_trailers(std::string_view trailers) {
    // Received fields that were not permitted as trailers are discarded, never
    // merged (RFC 9112 section 7.1.2), so framing validity depends only on
    // field syntax and the section budget.
    const auto discarded = [message = trailer_message_](std::string_view name) noexcept {
        return message == http1_trailer_message::request ? detail::is_forbidden_http_request_trailer_name(name)
                                                         : detail::is_forbidden_response_trailer_name(name);
    };
    const auto publish = [this, &discarded](std::string_view name, std::string_view value) {
        if (discarded(name)) {
            return;
        }
        // RFC 9112 section 5.2: each accepted obs-fold becomes SP before the
        // value is interpreted.
        if (const auto fold = detail::find_field_line_fold(value); fold != std::string_view::npos) {
            std::string unfolded;
            detail::copy_unfolded_field_lines(value, fold, detail::field_line_folding_policy::http, unfolded);
            trailers_.push_back(http_header::copy_with_lowercase_name(name, unfolded));
            return;
        }
        trailers_.push_back(http_header::copy_with_lowercase_name(name, value));
    };
    bool valid = false;
    try {
        valid = visit_http1_trailer_block(trailers, obs_fold_, publish);
    } catch (...) {
        trailers_.clear();
        throw;
    }
    if (!valid) {
        trailers_.clear();
    }
    return valid;
}

http1_chunk_decode_result http1_chunked_body_decoder::fail(
    std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    state_ = error;
    return http1_chunk_decode_result::make_failure(consumed_bytes, error);
}

}  // namespace ruvia

namespace ruvia {

namespace detail {

// One zlib stage is an implementation detail of the coding sequence, not a
// second public decoder. Its address must stay fixed for the pinned zlib stream.
class transfer_coding_decoder final {
public:
    transfer_coding_decoder(http_transfer_coding coding, protocol_byte_limit decoded_limit);
    transfer_coding_decoder(const transfer_coding_decoder&) = delete;
    transfer_coding_decoder& operator=(const transfer_coding_decoder&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;
    [[nodiscard]] bool received_input() const noexcept {
        return received_input_;
    }

private:
    struct inflate_step_result {
        std::size_t consumed_{0};
        std::size_t produced_{0};
        int status_{Z_OK};
    };
    struct active final {};
    struct gzip_member_boundary final {};
    struct completed final {};
    using state = std::variant<active, gzip_member_boundary, completed>;
    [[nodiscard]] inflate_step_result inflate_step(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result need_input(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result complete(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result fail(std::size_t consumed, http_transfer_coding_decode_error error) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result fail_decoder(std::size_t consumed) noexcept;
    zlib_inflate_stream stream_;
    state state_{active{}};
    protocol_byte_limit body_limit_;
    std::size_t decoded_bytes_{0};
    // An empty coded body carries no content and completes at framing EOF.
    bool received_input_{false};
    http_transfer_coding coding_;
};

namespace {

[[nodiscard]] int transfer_coding_window_bits(http_transfer_coding coding) {
    switch (coding) {
        case http_transfer_coding::gzip:
            return 15 + 16;
        case http_transfer_coding::deflate:
            return 15;
        default:
            throw std::invalid_argument("unsupported HTTP transfer coding");
    }
}

}  // namespace

transfer_coding_decoder::transfer_coding_decoder(
    http_transfer_coding coding, protocol_byte_limit decoded_limit)
    : stream_(transfer_coding_window_bits(coding)),
      body_limit_(decoded_limit),
      coding_(coding) {
    if (!stream_.initialized()) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    if (std::holds_alternative<completed>(state_)) {
        return input.empty() ? complete(0) : fail(0, http_transfer_coding_decode_error::invalid_content);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<gzip_member_boundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : need_input(consumed);
            }
            if (!stream_.reset()) {
                return fail_decoder(consumed);
            }
            state_.emplace<active>();
        }

        const auto window_bytes =
            std::min<std::size_t>(input.size() - consumed, (std::numeric_limits<uInt>::max)());
        const auto step = inflate_step(input.substr(consumed), output_buffer.subspan(produced));
        consumed += step.consumed_;
        received_input_ = received_input_ || step.consumed_ != 0;
        if (body_limit_.addition_exceeds(decoded_bytes_, step.produced_)) {
            return fail(consumed, http_transfer_coding_decode_error::decoded_size_exceeded);
        }
        decoded_bytes_ += step.produced_;
        produced += step.produced_;

        if (step.status_ == Z_STREAM_END) {
            if (coding_ != http_transfer_coding::gzip) {
                if (consumed != input.size()) {
                    return fail(consumed, http_transfer_coding_decode_error::invalid_content);
                }
                state_.emplace<completed>();
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : complete(consumed);
            }

            // RFC 1952 gzip data is a series of members. A member boundary is
            // not the end of the transfer coding: another member can arrive in
            // the same HTTP chunk or in a later one. Only framing EOF, reported
            // through finish_input(), commits this boundary as complete.
            state_.emplace<gzip_member_boundary>();
            if (produced == output_buffer.size()) {
                return output(consumed, std::string_view(output_buffer.data(), produced));
            }
            continue;
        }

        if (step.status_ != Z_OK && step.status_ != Z_BUF_ERROR) {
            if (step.status_ == Z_DATA_ERROR || step.status_ == Z_NEED_DICT) {
                return fail(consumed, http_transfer_coding_decode_error::invalid_content);
            }
            return fail_decoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(output_buffer.data(), produced));
        }
        if (consumed != input.size()) {
            // zlib sees at most one uInt window per step; a window that was
            // consumed without output (RFC 1952 FNAME/FCOMMENT bytes) is not
            // corrupt, the remaining input needs another step.
            if (step.consumed_ == window_bytes) {
                continue;
            }
            return fail(consumed, http_transfer_coding_decode_error::invalid_content);
        }
        return need_input(consumed);
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::finish_input() noexcept {
    if (std::holds_alternative<completed>(state_) ||
        std::holds_alternative<gzip_member_boundary>(state_) ||
        (std::holds_alternative<active>(state_) && !received_input_)) {
        state_.emplace<completed>();
        return complete(0);
    }
    return fail(0, http_transfer_coding_decode_error::invalid_content);
}

transfer_coding_decoder::inflate_step_result transfer_coding_decoder::inflate_step(
    std::string_view input, std::span<char> output) noexcept {
    const auto input_bytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    auto& stream = stream_.native();
    stream.next_in =
        input_bytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input_bytes);
    const auto output_bytes =
        std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output_bytes);

    const auto status = inflate(&stream, Z_NO_FLUSH);
    const auto consumed = input_bytes - stream.avail_in;
    const auto produced = output_bytes - stream.avail_out;
    stream.next_in = Z_NULL;
    stream.avail_in = 0;
    stream.next_out = Z_NULL;
    stream.avail_out = 0;
    return inflate_step_result{consumed, produced, status};
}

http_transfer_coding_decode_result transfer_coding_decoder::need_input(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::output(
    std::size_t consumed, std::string_view bytes_value) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_output_view(consumed, bytes_value));
}

http_transfer_coding_decode_result transfer_coding_decoder::complete(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail(
    std::size_t consumed, http_transfer_coding_decode_error error) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(consumed, error));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail_decoder(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
}

}  // namespace detail

namespace {

constexpr std::size_t transfer_coding_scratch_bytes = 4096;

struct transfer_coding_scratch_buffer final {
    // Stage output bounds valid bytes to [begin_, end_), so default construction
    // leaves the 4 KiB storage uninitialized instead of zero-filling it.
    std::array<char, transfer_coding_scratch_bytes> bytes_;
};

struct transfer_coding_stage final {
    transfer_coding_stage(http_transfer_coding coding, protocol_byte_limit decoded_limit)
        : decoder_(coding, decoded_limit) {}

    detail::transfer_coding_decoder decoder_;
    std::size_t begin_{0};
    std::size_t end_{0};
    bool complete_{false};
};

}  // namespace

struct http_transfer_coding_stack_decoder::impl_type final {
    impl_type(std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit)
        : stage_count_(codings.size()) {
        std::size_t index = 0;
        for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
            stages_[index++].emplace(*coding, decoded_limit);
        }
    }
    std::array<std::optional<transfer_coding_stage>, max_transfer_codings> stages_{};
    std::size_t stage_count_{0};
    // Only inter-stage edges own scratch. The final stage writes directly to
    // the caller's buffer, so a one-coding sequence uses no internal scratch.
    std::array<transfer_coding_scratch_buffer, max_transfer_codings - 1> buffers_;
    std::optional<http_transfer_coding_decode_error> failure_;
    bool decoder_failed_{false};
};

http_transfer_coding_stack_decoder::http_transfer_coding_stack_decoder(
    std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit) {
    if (codings.empty() || codings.size() > max_transfer_codings) {
        throw std::invalid_argument("invalid transfer-coding stack length");
    }
    impl_ = new impl_type(codings, decoded_limit);
}

http_transfer_coding_stack_decoder::~http_transfer_coding_stack_decoder() {
    delete impl_;
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    const auto failed = [this](std::size_t consumed) {
        if (impl_->decoder_failed_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
        }
        return http_transfer_coding_decode_result(
            http_transfer_coding_decode_failure(consumed, *impl_->failure_));
    };
    if (impl_->decoder_failed_ || impl_->failure_.has_value()) {
        return failed(0);
    }
    if (output_buffer.empty()) {
        impl_->decoder_failed_ = true;
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }

    std::size_t wire_consumed = 0;
    for (;;) {
        bool progressed = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            auto& stage = *impl_->stages_[index];
            if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
                continue;
            }
            std::string_view layer_input;
            if (index == 0) {
                layer_input = input.substr(wire_consumed);
            } else {
                auto& previous = *impl_->stages_[index - 1];
                layer_input = std::string_view(
                    impl_->buffers_[index - 1].bytes_.data() + previous.begin_,
                    previous.end_ - previous.begin_);
            }
            if (stage.complete_) {
                if (!layer_input.empty()) {
                    impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
                    return failed(wire_consumed);
                }
                continue;
            }
            const auto output = index + 1 == impl_->stage_count_
                                    ? output_buffer
                                    : std::span<char>(impl_->buffers_[index].bytes_);
            const auto decoded = stage.decoder_.decode(layer_input, output);
            const auto consumed = decoded.consumed_bytes();
            if (index == 0) {
                wire_consumed += consumed;
            } else {
                auto& previous = *impl_->stages_[index - 1];
                previous.begin_ += consumed;
                if (previous.begin_ == previous.end_) {
                    previous.begin_ = 0;
                    previous.end_ = 0;
                }
            }
            progressed = progressed || consumed != 0;
            if (const auto* bytes = decoded.output()) {
                progressed = true;
                if (index + 1 == impl_->stage_count_) {
                    return http_transfer_coding_decode_result(
                        http_transfer_coding_decode_output_view(wire_consumed, bytes->bytes()));
                }
                stage.begin_ = 0;
                stage.end_ = bytes->bytes().size();
                continue;
            }
            if (const auto* protocol_failure = decoded.failure()) {
                impl_->failure_ = protocol_failure->error();
                return failed(wire_consumed);
            }
            if (decoded.decoder_failure() != nullptr) {
                impl_->decoder_failed_ = true;
                return failed(wire_consumed);
            }
            if (decoded.complete() != nullptr) {
                stage.complete_ = true;
                progressed = true;
            }
        }

        bool all_complete = true;
        bool pending = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            const auto& stage = *impl_->stages_[index];
            all_complete = all_complete && stage.complete_;
            pending = pending || (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_);
        }
        if (all_complete && !pending) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(wire_consumed));
        }
        if (!progressed) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(wire_consumed));
        }
    }
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::finish_input() noexcept {
    if (impl_->decoder_failed_) {
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }
    if (impl_->failure_.has_value()) {
        return http_transfer_coding_decode_result(
            http_transfer_coding_decode_failure(0, *impl_->failure_));
    }
    const bool wire_received_input = impl_->stages_[0]->decoder_.received_input();
    for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
        auto& stage = *impl_->stages_[index];
        if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(0));
        }
        if (stage.complete_) {
            continue;
        }
        if (index > 0 && wire_received_input && !stage.decoder_.received_input()) {
            impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
            return http_transfer_coding_decode_result(
                http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        const auto finished = stage.decoder_.finish_input();
        if (const auto* protocol_failure = finished.failure()) {
            impl_->failure_ = protocol_failure->error();
            return http_transfer_coding_decode_result(
                http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        if (finished.decoder_failure() != nullptr) {
            impl_->decoder_failed_ = true;
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
        }
        stage.complete_ = true;
    }
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(0));
}

}  // namespace ruvia

namespace ruvia {

std::string_view http1_client_response_body_error_message(http1_client_response_body_error error) noexcept {
    switch (error) {
        case http1_client_response_body_error::invalid_framing:
            return "invalid HTTP/1 response body framing";
        case http1_client_response_body_error::invalid_transfer_coding:
            return "invalid HTTP/1 response transfer coding";
        case http1_client_response_body_error::incomplete_body:
            return "incomplete HTTP/1 response body";
    }
    return "invalid HTTP/1 response body";
}

http1_client_response_body_decoder::http1_client_response_body_decoder(
    http1_client_response_plan plan) {
    if (plan.informational() || plan.connect_tunnel() || plan.protocol_upgrade()) {
        throw std::invalid_argument("HTTP/1 response plan has no ordinary message body");
    }
    if (const auto* without = plan.without_content()) {
        framing_ = framing_type::no_body;
        persistence_ = without->persistence();
        return;
    }
    // A discarding (205) plan carries the same framings as an ordinary one,
    // but its content is never emitted, so it is never transfer-decoded.
    discard_content_ = plan.discards_content();
    const auto configure_coding = [this](const http_transfer_codings& codings) {
        if (!codings.empty() && !discard_content_) {
            transfer_.emplace(std::span<const http_transfer_coding>(
                                  codings.data(), codings.size()),
                protocol_byte_limit::unlimited());
        }
    };
    if (const auto* known = plan.known_length()) {
        framing_ = framing_type::fixed;
        remaining_ = known->content_length();
        persistence_ = known->persistence();
    } else if (const auto* chunked = plan.chunked()) {
        framing_ = framing_type::chunked;
        persistence_ = chunked->persistence();
        configure_coding(chunked->transfer_codings());
        chunked_.emplace(http1_chunked_body_decoder_config{.obs_fold_ = http1_trailer_obs_fold::accept,
            .trailer_message_ = http1_trailer_message::response});
    } else if (const auto* close = plan.close_delimited()) {
        framing_ = framing_type::close_delimited;
        configure_coding(close->transfer_codings());
    } else {
        throw std::invalid_argument("unsupported HTTP/1 response plan");
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decode(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    return step(available, scratch, eof_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_input(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    eof_ = true;
    return step(available, scratch, true);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (!discard_content_) {
        return step_content(available, scratch, eof);
    }
    // RFC 9110 section 15.3.6 forbids only generating 205 content: consume
    // what the framing delimits without emitting it, then report the next
    // non-output result over every byte consumed by this call.
    std::size_t discarded = 0;
    for (;;) {
        const auto result_value = step_content(available.substr(discarded), scratch, eof);
        if (result_value.output() == nullptr) {
            return with_consumed_prefix(result_value, discarded);
        }
        discarded += result_value.consumed_bytes();
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_content(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (trailers_reported_) {
        return finish_transfer(0, scratch);
    }
    // A full output step can leave inflate output buffered after consuming
    // all wire input. Drain it before asking framing or transport for more.
    if (transfer_ && transfer_phase_ == transfer_phase_type::drain_output) {
        const auto pending = drive_transfer({}, 0, scratch);
        if (pending.need_input() == nullptr) {
            return pending;
        }
    }
    switch (framing_) {
        case framing_type::fixed:
            return step_fixed(available, scratch, eof);
        case framing_type::close_delimited:
            return step_close_delimited(available, scratch, eof);
        case framing_type::chunked:
            return step_chunked(available, scratch, eof);
        case framing_type::no_body:
            break;
    }
    return complete(0);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_fixed(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (remaining_ == 0) {
        return complete(0);
    }
    if (available.empty()) {
        return need_input_or_incomplete(0, eof);
    }
    const auto output_length = std::min({remaining_, available.size(), scratch.size()});
    remaining_ -= output_length;
    return emit(output_length, available.substr(0, output_length));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_close_delimited(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (!transfer_) {
        if (available.empty()) {
            return eof ? complete(0) : need_input(0);
        }
        const auto output_length = std::min(available.size(), scratch.size());
        return emit(output_length, available.substr(0, output_length));
    }
    std::size_t consumed = 0;
    for (;;) {
        const auto rest = available.substr(consumed);
        if (transfer_phase_ == transfer_phase_type::ended) {
            // The connection close is the only valid framing end after the
            // transfer coding completed.
            if (!rest.empty()) {
                return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
            }
            return eof ? complete(consumed) : need_input(consumed);
        }
        if (rest.empty()) {
            return eof ? finish_transfer(consumed, scratch) : need_input(consumed);
        }
        const auto driven = drive_transfer(rest, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        consumed += input_consumed;
        if (driven.need_input() == nullptr) {
            return driven;
        }
        if (input_consumed == 0 && transfer_phase_ != transfer_phase_type::ended) {
            return need_input_or_incomplete(consumed, eof);
        }
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_chunked(
    std::string_view available, std::span<char> scratch, bool eof) {
    std::size_t consumed = 0;
    for (;;) {
        if (auto pending = drain_pending_chunk(available, consumed, scratch, eof)) {
            return *pending;
        }
        const auto rest = available.substr(consumed);
        const auto decoded = chunked_->decode(rest,
            transfer_ ? (std::numeric_limits<std::size_t>::max)() : scratch.size());
        const auto chunk_consumed = consumed + decoded.consumed_bytes();
        if (decoded.failure() != nullptr) {
            return fail(http1_client_response_body_error::invalid_framing, chunk_consumed);
        }
        if (decoded.need_more() != nullptr) {
            return need_input_or_incomplete(chunk_consumed, eof);
        }
        if (const auto* complete = decoded.complete()) {
            trailers_reported_ = true;
            return validated_trailers(chunk_consumed, complete->trailers());
        }
        const auto body = decoded.body_chunk()->bytes();
        if (!transfer_) {
            return emit(chunk_consumed, body);
        }
        // The chunk decoder already consumed the whole chunk, but the transfer
        // decoder may take only part of its data. Track the rest as wire bytes
        // so the caller keeps the unconsumed suffix without a borrowed view.
        consumed += static_cast<std::size_t>(body.data() - rest.data());
        pending_body_bytes_ = body.size();
        pending_delimiter_bytes_ = chunk_consumed - consumed - body.size();
    }
}

// Feeds pending chunk data to the transfer decoder and then skips the chunk
// delimiter. Returns nothing once both are consumed and framing can continue.
std::optional<http1_client_response_body_decoder::result_type>
http1_client_response_body_decoder::drain_pending_chunk(
    std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof) {
    if (pending_body_bytes_ != 0) {
        const auto input = available.substr(consumed, std::min(pending_body_bytes_, available.size() - consumed));
        const auto driven = drive_transfer(input, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        pending_body_bytes_ -= input_consumed;
        consumed += input_consumed;
        if (pending_body_bytes_ == 0) {
            consume_pending_delimiter(available, consumed);
        }
        if (driven.need_input() == nullptr) {
            return with_consumed(driven, consumed);
        }
        if (pending_body_bytes_ != 0 || consumed == available.size()) {
            return need_input_or_incomplete(consumed, eof);
        }
    } else {
        consume_pending_delimiter(available, consumed);
    }
    if (pending_delimiter_bytes_ != 0) {
        return need_input_or_incomplete(consumed, eof);
    }
    return std::nullopt;
}

// The chunk decoder already validated these delimiter bytes.
void http1_client_response_body_decoder::consume_pending_delimiter(
    std::string_view available, std::size_t& consumed) noexcept {
    const auto bytes = std::min(pending_delimiter_bytes_, available.size() - consumed);
    consumed += bytes;
    pending_delimiter_bytes_ -= bytes;
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::drive_transfer(
    std::string_view input, std::size_t wire_prefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wire_prefix + decoded.consumed_bytes();
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.need_input() != nullptr) {
        transfer_phase_ = transfer_phase_type::needs_framed_input;
        return need_input(consumed);
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return need_input(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
    }
    return decoder_failure(consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_transfer(
    std::size_t consumed, std::span<char> scratch) {
    if (!transfer_) {
        return complete(consumed);
    }
    // Buffered output drains before framing EOF is committed to the decoder.
    auto decoded = transfer_->decode({}, scratch);
    if (decoded.need_input() != nullptr) {
        decoded = transfer_->finish_input();
    }
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return complete(consumed);
    }
    if (decoded.decoder_failure() != nullptr) {
        return decoder_failure(consumed);
    }
    return fail(http1_client_response_body_error::incomplete_body, consumed);
}

http1_client_response_body_decoder::result_type
http1_client_response_body_decoder::replay_terminal() const noexcept {
    return std::visit([](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(0, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(0, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(0));
        }
    },
        *terminal_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input(
    std::size_t consumed) const noexcept {
    return result_type(need_input_type(consumed));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input_or_incomplete(
    std::size_t consumed, bool eof) {
    if (eof) {
        return fail(http1_client_response_body_error::incomplete_body, consumed);
    }
    return need_input(consumed);
}

// The single emission point.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit(
    std::size_t consumed, std::string_view bytes_value) {
    return result_type(output_view_type(consumed, bytes_value));
}

// Transfer output borrows scratch and must be drained before more framed input.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit_transfer_output(
    std::size_t consumed, std::string_view bytes_value) {
    transfer_phase_ = transfer_phase_type::drain_output;
    return emit(consumed, bytes_value);
}

// Re-reports a transfer result with the wire bytes consumed after its input,
// such as the chunk delimiter that follows the final chunk data.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed(
    const result_type& result, std::size_t consumed) noexcept {
    return std::visit([consumed](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else if constexpr (std::is_same_v<t_type, decoder_failure_type>) {
            return result_type(decoder_failure_type(consumed));
        } else {
            return result_type(payload_value);
        }
    },
        result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed_prefix(
    const result_type& result, std::size_t prefix) noexcept {
    return std::visit([prefix](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        const auto consumed = prefix + payload_value.consumed_bytes();
        if constexpr (std::is_same_v<t_type, need_input_type>) {
            return result_type(need_input_type(consumed));
        } else if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, validated_trailers_view_type>) {
            return result_type(validated_trailers_view_type(consumed, payload_value.fields()));
        } else if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(consumed, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(consumed));
        }
    },
        result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::validated_trailers(
    std::size_t consumed, std::span<const http_header> fields) {
    return result_type(validated_trailers_view_type(consumed, fields));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::complete(
    std::size_t consumed) {
    const auto completed = complete_type(consumed, persistence_);
    terminal_ = completed;
    return result_type(completed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::fail(
    http1_client_response_body_error error, std::size_t consumed) {
    const auto failure = protocol_failure_type(consumed, error);
    terminal_ = failure;
    return result_type(failure);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decoder_failure(
    std::size_t consumed) {
    const auto failure = decoder_failure_type(consumed);
    terminal_ = failure;
    return result_type(failure);
}

std::size_t http1_client_response_body_decoder::result_type::consumed_bytes() const noexcept {
    return std::visit([](const auto& payload_value) { return payload_value.consumed_bytes(); }, value_);
}

const http1_client_response_body_decoder::need_input_type*
http1_client_response_body_decoder::result_type::need_input() const& noexcept {
    return std::get_if<need_input_type>(&value_);
}

const http1_client_response_body_decoder::output_view_type*
http1_client_response_body_decoder::result_type::output() const& noexcept {
    return std::get_if<output_view_type>(&value_);
}

const http1_client_response_body_decoder::validated_trailers_view_type*
http1_client_response_body_decoder::result_type::trailers() const& noexcept {
    return std::get_if<validated_trailers_view_type>(&value_);
}

const http1_client_response_body_decoder::complete_type*
http1_client_response_body_decoder::result_type::complete() const& noexcept {
    return std::get_if<complete_type>(&value_);
}

const http1_client_response_body_decoder::protocol_failure_type*
http1_client_response_body_decoder::result_type::protocol_failure() const& noexcept {
    return std::get_if<protocol_failure_type>(&value_);
}

const http1_client_response_body_decoder::decoder_failure_type*
http1_client_response_body_decoder::result_type::decoder_failure() const& noexcept {
    return std::get_if<decoder_failure_type>(&value_);
}

}  // namespace ruvia
