#pragma once

#include "body/http_stream_body_reader_errors.h"

namespace ruvia::detail {

template <typename stream_type>
task<std::optional<std::span<const std::byte>>> stream_body_reader<stream_type>::read_chunked() {
    compact_pending();
    if (finished_) {
        co_return std::nullopt;
    }

    for (;;) {
        const auto source_value = !initial_body_and_pipeline_.empty()
                                      ? std::string_view(initial_body_and_pipeline_)
                                      : std::string_view(buffer_);
        const auto result_value = chunk_decoder_.decode(source_value.substr(read_cursor_));
        if (result_value.consumed_bytes() != 0) {
            pending_compact_until_ = read_cursor_ + result_value.consumed_bytes();
        }
        if (const auto* body_chunk = result_value.body_chunk()) {
            co_return ::ruvia::as_bytes(body_chunk->bytes());
        }
        if (const auto* complete = result_value.complete()) {
            if ((trailers_.append_http1(complete->trailers()).index() != 0)) {
                throw std::logic_error("HTTP/1 decoder published invalid request trailers");
            }
            compact_pending();
            co_return std::nullopt;
        }
        if (const auto* failure = result_value.failure()) {
            throw http_request_chunk_decode_error(failure->error());
        }
        if (result_value.need_more() == nullptr) {
            throw std::logic_error("unexpected HTTP/1 chunk decode result");
        }
        compact_pending();
        materialize_initial_remainder();
        co_await read_more();
    }
}

template <typename stream_type>
task<std::optional<std::span<const std::byte>>> stream_body_reader<stream_type>::read_transfer_decoded_chunked() {
    if (transfer_decoder_ == nullptr) {
        auto chunk = co_await read_chunked();
        if (!chunk) {
            mark_finished();
        }
        co_return chunk;
    }

    if (transfer_output_.empty()) {
        ::ruvia::resize_pmr_string_for_overwrite(transfer_output_, http_body_buffer_bytes);
    }

    // Keep the borrowed encoded chunk until the decoder reports its consumed
    // prefix. read_chunked() is called only after that view is empty, so its
    // compaction cannot invalidate decoder input across application reads.
    for (;;) {
        const auto result_value =
            transfer_decoder_->decode(transfer_input_, std::span<char>(transfer_output_));
        transfer_input_.remove_prefix(std::min(transfer_input_.size(), result_value.consumed_bytes()));
        if (const auto* output = result_value.output()) {
            co_return ::ruvia::as_bytes(output->bytes());
        }
        if (const auto* failure = result_value.failure()) {
            throw_transfer_coding_protocol_failure(*failure);
        }
        if (result_value.decoder_failure() != nullptr) {
            throw_http_transfer_coding_decoder_failure();
        }
        if (result_value.complete() == nullptr && result_value.need_input() == nullptr) {
            throw std::logic_error("unexpected transfer-coding decode result");
        }

        auto chunk = co_await read_chunked();
        if (!chunk) {
            require_complete_transfer_coding(*transfer_decoder_);
            mark_finished();
            co_return std::nullopt;
        }
        transfer_input_ = ::ruvia::as_chars(*chunk);
    }
}

}  // namespace ruvia::detail
