#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_protocol.h"

namespace ruvia {

// RFC 9112 section 7 transfer-parameter: `token BWS "=" BWS ( token / quoted-string )`.
// `part` is one OWS-trimmed segment as produced by http_all_parameters.
[[nodiscard]] inline bool is_valid_http_transfer_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(http_trim_ows(part.substr(0, equals))) && is_http_token_or_quoted_string(http_trim_ows(part.substr(equals + 1)));
}

[[nodiscard]] inline bool http_parse_transfer_coding_syntax(borrowed_text item RUVIA_LIFETIMEBOUND, std::string_view& coding, bool& has_parameters) noexcept {
    const auto text = item.view();
    const auto first_semicolon = http_find_unquoted_delimiter(text, 0, ';');
    coding = http_trim_ows(text.substr(0, first_semicolon));
    if (!is_valid_http_header_name(coding)) {
        return false;
    }

    has_parameters = first_semicolon < text.size();
    return http_all_parameters(text, is_valid_http_transfer_parameter);
}

enum class http_transfer_encoding_parse_status : std::uint8_t { ok,
    malformed,
    unsupported };

class http_non_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_content_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_content_codings& transfer_codings() const&& = delete;

private:
    friend class http_transfer_encoding_value;

    explicit http_non_chunked_transfer_encoding(http_content_codings transfer_codings) :
        transfer_codings_(std::move(transfer_codings)) {}

    http_content_codings transfer_codings_;
};

class http_final_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_content_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_content_codings& transfer_codings() const&& = delete;

private:
    friend class http_transfer_encoding_value;

    explicit http_final_chunked_transfer_encoding(http_content_codings transfer_codings) :
        transfer_codings_(std::move(transfer_codings)) {}

    http_content_codings transfer_codings_;
};

class http_transfer_encoding_value final {
public:
    [[nodiscard]] const http_non_chunked_transfer_encoding* non_chunked() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_non_chunked_transfer_encoding>(&value_);
    }
    const http_non_chunked_transfer_encoding* non_chunked() const&& = delete;

    [[nodiscard]] const http_final_chunked_transfer_encoding* final_chunked() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_final_chunked_transfer_encoding>(&value_);
    }
    const http_final_chunked_transfer_encoding* final_chunked() const&& = delete;

private:
    friend class http_transfer_encoding_state;

    using value_type = std::variant<http_non_chunked_transfer_encoding, http_final_chunked_transfer_encoding>;

    [[nodiscard]] static http_transfer_encoding_value make_non_chunked(http_content_codings transfer_codings) {
        return http_transfer_encoding_value(http_non_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    [[nodiscard]] static http_transfer_encoding_value make_final_chunked(http_content_codings transfer_codings) {
        return http_transfer_encoding_value(http_final_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    explicit http_transfer_encoding_value(http_non_chunked_transfer_encoding value) :
        value_(std::move(value)) {}

    explicit http_transfer_encoding_value(http_final_chunked_transfer_encoding value) :
        value_(std::move(value)) {}

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_transfer_encoding_value>);

// Field lines form one ordered recipient-side list. RFC 9110 section 5.6.1.2
// requires ignoring empty members, including on repeated field lines. Unknown
// codings are remembered while later items are still checked for malformed syntax.
class http_transfer_encoding_state final {
public:
    [[nodiscard]] http_transfer_encoding_parse_status parse_field(std::string_view field_value) {
        http_content_codings next_codings;
        bool final_chunked = false;
        if (value_) {
            if (const auto* chunked = value_->final_chunked()) {
                next_codings = chunked->transfer_codings();
                final_chunked = true;
            } else {
                next_codings = value_->non_chunked()->transfer_codings();
            }
        }
        bool unsupported = unsupported_;
        bool malformed = false;
        http_visit_comma_separated_quoted_field_items(field_value, [&next_codings, &final_chunked, &unsupported, &malformed](std::string_view item) {
            if (item.empty()) {
                return true;
            }
            std::string_view coding;
            bool has_parameters = false;
            if (final_chunked || !http_parse_transfer_coding_syntax(item, coding, has_parameters)) {
                malformed = true;
                return false;
            }
            if (http_ascii_equals_ignore_case(coding, "chunked")) {
                if (has_parameters) {
                    malformed = true;
                    return false;
                }
                final_chunked = true;
                return true;
            }
            const auto transfer_coding = parse_http_content_coding_token(coding);
            if (transfer_coding.has_value() && is_http_transfer_coding(*transfer_coding)) {
                if (has_parameters) {
                    malformed = true;
                    return false;
                }
                if (next_codings.size() == max_content_codings) {
                    unsupported = true;
                    return true;
                }
                next_codings.push_back(*transfer_coding);
                return true;
            }
            unsupported = true;
            return true;
        });
        if (malformed) {
            return http_transfer_encoding_parse_status::malformed;
        }
        auto next_value = final_chunked ? http_transfer_encoding_value::make_final_chunked(std::move(next_codings)) : http_transfer_encoding_value::make_non_chunked(std::move(next_codings));
        value_.emplace(std::move(next_value));
        unsupported_ = unsupported;
        return unsupported_ ? http_transfer_encoding_parse_status::unsupported : http_transfer_encoding_parse_status::ok;
    }

    [[nodiscard]] const std::optional<http_transfer_encoding_value>& value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    const std::optional<http_transfer_encoding_value>& value() const&& = delete;
    [[nodiscard]] bool unsupported() const noexcept {
        return unsupported_;
    }

private:
    std::optional<http_transfer_encoding_value> value_;
    bool unsupported_{false};
};

class http_codec_memory;

using http_transfer_coding_decode_failure = decode_failure<http_content_decode_error>;

// One decode() or finish_input() outcome: more input needed, decoded bytes in
// the caller's output storage, the end of every coding, or a data failure.
using http_transfer_coding_decode_result = decode_step<decode_need_input, decode_output, decode_complete, http_transfer_coding_decode_failure>;

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse; every
// coding must satisfy is_http_transfer_coding(), else std::invalid_argument.
// Input is never retained and must not overlap the caller's scratch storage.
// Output views borrow that storage.
// Consume each view before reusing it. Keep the decoder at a stable address
// until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// A layer that received no coded bytes before framing EOF completes as empty.
// Data errors are typed failures because incremental drivers must retain the
// exact wire consumption even when a layer fails. A finished or failed
// decoder repeats its terminal result without consuming input (the caller
// decides what bytes after a finished coding mean); only reset() starts
// another coded body. A codec fault throws and leaves the stack failed; any
// subsequent decode() or finish_input() reports invalid_content until reset().
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(std::span<const http_content_coding> codings, protocol_byte_limit decoded_limit, http_codec_memory* memory = nullptr);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output RUVIA_LIFETIMEBOUND) &;
    http_transfer_coding_decode_result decode(std::string_view, std::span<char>) && = delete;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() & noexcept;
    http_transfer_coding_decode_result finish_input() && = delete;

    [[nodiscard]] decoder_state state() const noexcept;
    // Starts a new coded body with the constructed codings and limit, keeping
    // the codec storage. Throws http_content_codec_error when a codec cannot
    // restart; the decoder is then unusable.
    void reset();

private:
    struct impl_type;
    impl_type* impl_{nullptr};
};

}  // namespace ruvia
