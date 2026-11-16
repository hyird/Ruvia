#include "http1_message_body.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "http1_chunked_body.h"
#include "http1_connection_plan.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_protocol.h"
#include "http_request_body_failure.h"
#include "http_transfer_coding.h"

namespace ruvia {

namespace {

[[nodiscard]] http1_body_decode_error chunk_framing_error(http1_chunk_decode_error error) noexcept {
    switch (error) {
        case http1_chunk_decode_error::invalid_framing:
            return http1_body_decode_error::invalid_framing;
        case http1_chunk_decode_error::body_limit_exceeded:
            return http1_body_decode_error::body_limit_exceeded;
        case http1_chunk_decode_error::framing_limit_exceeded:
            return http1_body_decode_error::framing_limit_exceeded;
    }
    return http1_body_decode_error::invalid_framing;
}

}  // namespace

http_protocol_error make_http_protocol_error(http1_body_decode_error error) noexcept {
    switch (error) {
        case http1_body_decode_error::invalid_framing:
            return make_http_protocol_error(http1_chunk_decode_error::invalid_framing);
        case http1_body_decode_error::framing_limit_exceeded:
            return make_http_protocol_error(http1_chunk_decode_error::framing_limit_exceeded);
        case http1_body_decode_error::body_limit_exceeded:
            return make_http_protocol_error(http_request_body_failure::too_large);
        case http1_body_decode_error::invalid_transfer_coding:
            return make_http_protocol_error(http_content_decode_error::invalid_content, http_coding_field::transfer_encoding);
        case http1_body_decode_error::incomplete_body:
            return make_http_protocol_error(http_request_body_failure::incomplete);
    }
    return make_http_protocol_error(http1_chunk_decode_error::invalid_framing);
}

std::string_view http1_response_body_error_message(http1_body_decode_error error) noexcept {
    switch (error) {
        case http1_body_decode_error::invalid_framing:
        case http1_body_decode_error::framing_limit_exceeded:
            return "invalid HTTP/1 response body framing";
        case http1_body_decode_error::body_limit_exceeded:
            return "HTTP/1 response body is too large";
        case http1_body_decode_error::invalid_transfer_coding:
            return "invalid HTTP/1 response transfer coding";
        case http1_body_decode_error::incomplete_body:
            return "incomplete HTTP/1 response body";
    }
    return "invalid HTTP/1 response body";
}

http1_body_decoder::http1_body_decoder(const http1_message_framing& framing, http1_body_decoder_options config) :
    framing_(framing),
    config_(config) {
    start();
}

void http1_body_decoder::start() {
    state_ = decoder_state::active;
    remaining_ = framing_.content_length();
    pending_body_bytes_ = 0;
    pending_delimiter_bytes_ = 0;
    discard_content_ = false;
    eof_ = false;
    trailers_reported_ = false;
    transfer_phase_ = transfer_phase_type::needs_framed_input;
    const auto& codings = framing_.transfer_codings();
    if (!codings.empty()) {
        if (transfer_) {
            transfer_->reset();
        } else {
            transfer_.emplace(std::span<const http_content_coding>(codings.data(), codings.size()), config_.body_limit_);
        }
    }
    if (framing_.kind() == http1_framing_kind::chunked) {
        if (chunked_) {
            chunked_->reset();
        } else {
            chunked_.emplace(http1_chunked_body_decoder_options{.body_limit_ = config_.body_limit_, .obs_fold_ = config_.obs_fold_, .trailer_message_ = config_.trailer_message_});
        }
    }
}

void http1_body_decoder::reset() {
    start();
}

decoder_state http1_body_decoder::state() const noexcept {
    return state_;
}

void http1_body_decoder::discard_content() noexcept {
    discard_content_ = true;
    transfer_.reset();
    transfer_phase_ = transfer_phase_type::needs_framed_input;
}

http1_body_decode_result http1_body_decoder::decode(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes) & {
    return run(available, scratch, max_output_bytes, false);
}

http1_body_decode_result http1_body_decoder::finish_input(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes) & {
    return run(available, scratch, max_output_bytes, true);
}

http1_body_decode_result http1_body_decoder::run(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof) {
    if (max_output_bytes == 0) {
        throw std::invalid_argument("HTTP/1 body decoder output quota must be greater than zero");
    }
    if (transfer_ && scratch.empty()) {
        throw std::invalid_argument("HTTP/1 body decoder scratch must be non-empty for transfer-coded content");
    }
    if (state_ == decoder_state::failed) {
        return http1_body_decode_result(0, http1_body_decode_failure(error_));
    }
    if (state_ == decoder_state::finished) {
        return http1_body_decode_result(0, decode_complete{});
    }
    eof_ = eof_ || eof;
    return step(available, scratch.first(std::min(scratch.size(), max_output_bytes)), max_output_bytes, eof_);
}

http1_body_decode_result http1_body_decoder::step(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof) {
    if (!discard_content_) {
        return step_content(available, scratch, max_output_bytes, eof);
    }
    // Consume what the framing delimits without publishing it, then report the
    // next non-output result over every byte consumed by this call.
    std::size_t discarded = 0;
    for (;;) {
        const auto result_value = step_content(available.substr(discarded), scratch, max_output_bytes, eof);
        if (!result_value.holds<decode_output>()) {
            return result_value.with_consumed_bytes(discarded + result_value.consumed_bytes());
        }
        discarded += result_value.consumed_bytes();
    }
}

http1_body_decode_result http1_body_decoder::step_content(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof) {
    if (trailers_reported_) {
        // The chunked framing is complete, so a coding it leaves unfinished is
        // a coding error, not missing input.
        return finish_transfer(0, scratch, http1_body_decode_error::invalid_transfer_coding);
    }
    // A full output step can leave inflate output buffered after consuming
    // all wire input. Drain it before asking framing or transport for more.
    if (transfer_ && transfer_phase_ == transfer_phase_type::drain_output) {
        const auto pending = drive_transfer({}, 0, scratch);
        if (!pending.holds<decode_need_input>()) {
            return pending;
        }
    }
    switch (framing_.kind()) {
        case http1_framing_kind::known_length:
            return step_known_length(available, max_output_bytes, eof);
        case http1_framing_kind::close_delimited:
            return step_close_delimited(available, scratch, max_output_bytes, eof);
        case http1_framing_kind::chunked:
            return step_chunked(available, scratch, max_output_bytes, eof);
        case http1_framing_kind::no_content:
            break;
    }
    return complete(0);
}

http1_body_decode_result http1_body_decoder::step_known_length(std::string_view available, std::size_t max_output_bytes, bool eof) {
    if (remaining_ == 0) {
        return complete(0);
    }
    if (available.empty()) {
        return need_input_or_incomplete(0, eof);
    }
    const auto output_length = std::min(available.size(), max_output_bytes);
    const auto taken = remaining_ < output_length ? static_cast<std::size_t>(remaining_) : output_length;
    remaining_ -= taken;
    return http1_body_decode_result(taken, decode_output(available.substr(0, taken)));
}

http1_body_decode_result http1_body_decoder::step_close_delimited(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof) {
    if (!transfer_) {
        if (available.empty()) {
            return eof ? complete(0) : http1_body_decode_result(0, decode_need_input{});
        }
        const auto output_length = std::min(available.size(), max_output_bytes);
        return http1_body_decode_result(output_length, decode_output(available.substr(0, output_length)));
    }
    std::size_t consumed = 0;
    for (;;) {
        const auto rest = available.substr(consumed);
        if (transfer_phase_ == transfer_phase_type::ended) {
            // The connection close is the only valid framing end after the
            // transfer coding completed.
            if (!rest.empty()) {
                return fail(http1_body_decode_error::invalid_transfer_coding, consumed);
            }
            return eof ? complete(consumed) : http1_body_decode_result(consumed, decode_need_input{});
        }
        if (rest.empty()) {
            return eof ? finish_transfer(consumed, scratch, http1_body_decode_error::incomplete_body) : http1_body_decode_result(consumed, decode_need_input{});
        }
        const auto driven = drive_transfer(rest, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        consumed += input_consumed;
        if (!driven.holds<decode_need_input>()) {
            return driven;
        }
        if (input_consumed == 0 && transfer_phase_ != transfer_phase_type::ended) {
            return need_input_or_incomplete(consumed, eof);
        }
    }
}

http1_body_decode_result http1_body_decoder::step_chunked(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes, bool eof) {
    std::size_t consumed = 0;
    for (;;) {
        if (auto pending = drain_pending_chunk(available, consumed, scratch, eof)) {
            return *pending;
        }
        const auto rest = available.substr(consumed);
        const auto decoded = chunked_->decode(rest, transfer_ ? (std::numeric_limits<std::size_t>::max)() : max_output_bytes);
        const auto chunk_consumed = consumed + decoded.consumed_bytes();
        if (const auto* failure = decoded.get_if<http1_chunk_decode_failure>()) {
            return fail(chunk_framing_error(failure->error()), chunk_consumed);
        }
        if (decoded.holds<decode_need_input>()) {
            return need_input_or_incomplete(chunk_consumed, eof);
        }
        if (const auto* chunked_end = decoded.get_if<http1_chunk_decode_complete>()) {
            trailers_reported_ = true;
            return http1_body_decode_result(chunk_consumed, http1_body_trailers(chunked_end->trailers()));
        }
        const auto body = decoded.get_if<decode_output>()->bytes();
        if (!transfer_) {
            return http1_body_decode_result(chunk_consumed, decode_output(body));
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
std::optional<http1_body_decode_result> http1_body_decoder::drain_pending_chunk(std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof) {
    if (pending_body_bytes_ != 0) {
        const auto input = available.substr(consumed, std::min(pending_body_bytes_, available.size() - consumed));
        if (!transfer_) {
            // Content was discarded inside this chunk: its remaining data is
            // skipped like the delimiter the chunk decoder already validated.
            pending_body_bytes_ -= input.size();
            consumed += input.size();
            if (pending_body_bytes_ != 0) {
                return need_input_or_incomplete(consumed, eof);
            }
            consume_pending_delimiter(available, consumed);
        } else if (transfer_phase_ == transfer_phase_type::ended && !input.empty()) {
            // Chunk data after the transfer coding completed belongs to no
            // coding; the finished stack would only repeat its end.
            return fail(http1_body_decode_error::invalid_transfer_coding, consumed);
        } else {
            const auto driven = drive_transfer(input, consumed, scratch);
            const auto input_consumed = driven.consumed_bytes() - consumed;
            pending_body_bytes_ -= input_consumed;
            consumed += input_consumed;
            if (pending_body_bytes_ == 0) {
                consume_pending_delimiter(available, consumed);
            }
            if (!driven.holds<decode_need_input>()) {
                // Re-report the transfer result with the wire bytes consumed
                // after its input, such as the chunk delimiter that follows
                // the final chunk data.
                return driven.with_consumed_bytes(consumed);
            }
            if (pending_body_bytes_ != 0 || consumed == available.size()) {
                return need_input_or_incomplete(consumed, eof);
            }
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
void http1_body_decoder::consume_pending_delimiter(std::string_view available, std::size_t& consumed) noexcept {
    const auto bytes = std::min(pending_delimiter_bytes_, available.size() - consumed);
    consumed += bytes;
    pending_delimiter_bytes_ -= bytes;
}

http1_body_decode_result http1_body_decoder::drive_transfer(std::string_view input, std::size_t wire_prefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wire_prefix + decoded.consumed_bytes();
    if (const auto* output = decoded.get_if<decode_output>()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.holds<decode_need_input>()) {
        transfer_phase_ = transfer_phase_type::needs_framed_input;
        return http1_body_decode_result(consumed, decode_need_input{});
    }
    if (decoded.holds<decode_complete>()) {
        transfer_phase_ = transfer_phase_type::ended;
        return http1_body_decode_result(consumed, decode_need_input{});
    }
    // The only remaining alternative is a data failure.
    const auto error = decoded.get_if<http_transfer_coding_decode_failure>()->error();
    return fail(error == http_content_decode_error::decoded_size_exceeded ? http1_body_decode_error::body_limit_exceeded : http1_body_decode_error::invalid_transfer_coding, consumed);
}

// Commits the framing end to the transfer decoder. `unfinished` is the error
// of a coding that the framing end leaves incomplete.
http1_body_decode_result http1_body_decoder::finish_transfer(std::size_t consumed, std::span<char> scratch, http1_body_decode_error unfinished) {
    if (!transfer_) {
        return complete(consumed);
    }
    // Buffered output drains before framing EOF is committed to the decoder.
    const auto drained = transfer_->decode({}, scratch);
    const auto decoded = drained.holds<decode_need_input>() ? transfer_->finish_input() : drained;
    if (const auto* output = decoded.get_if<decode_output>()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.holds<decode_complete>()) {
        transfer_phase_ = transfer_phase_type::ended;
        return complete(consumed);
    }
    const auto* failure = decoded.get_if<http_transfer_coding_decode_failure>();
    return fail(failure != nullptr && failure->error() == http_content_decode_error::decoded_size_exceeded ? http1_body_decode_error::body_limit_exceeded : unfinished, consumed);
}

http1_body_decode_result http1_body_decoder::need_input_or_incomplete(std::size_t consumed, bool eof) noexcept {
    if (eof) {
        return fail(http1_body_decode_error::incomplete_body, consumed);
    }
    return http1_body_decode_result(consumed, decode_need_input{});
}

// Transfer output borrows scratch and must be drained before more framed input.
http1_body_decode_result http1_body_decoder::emit_transfer_output(std::size_t consumed, std::string_view bytes) noexcept {
    transfer_phase_ = transfer_phase_type::drain_output;
    return http1_body_decode_result(consumed, decode_output(bytes));
}

http1_body_decode_result http1_body_decoder::complete(std::size_t consumed) noexcept {
    state_ = decoder_state::finished;
    return http1_body_decode_result(consumed, decode_complete{});
}

http1_body_decode_result http1_body_decoder::fail(http1_body_decode_error error, std::size_t consumed) noexcept {
    state_ = decoder_state::failed;
    error_ = error;
    return http1_body_decode_result(consumed, http1_body_decode_failure(error));
}

}  // namespace ruvia
