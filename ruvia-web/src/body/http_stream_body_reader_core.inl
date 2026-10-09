#pragma once

#include "body/http_stream_body_reader_errors.h"

namespace ruvia::detail {

template <typename stream_type>
stream_body_reader<stream_type>::stream_body_reader(stream_type& stream,
    std::pmr::polymorphic_allocator<char> allocator, std::string_view initial_body_and_pipeline,
    http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
    ruvia::connection_scanner::entry_type& scanner_entry)
    : stream_(stream),
      buffer_(allocator),
      transfer_output_(allocator),
      transfer_decoder_(nullptr,
          pmr_object_deleter<http_transfer_coding_stack_decoder>{allocator.resource()}),
      initial_body_and_pipeline_(initial_body_and_pipeline),
      body_plan_(body_plan),
      body_limit_(body_limit_value),
      chunk_decoder_(http1_chunked_body_decoder_config{.body_limit_ = body_limit_value}),
      trailers_(allocator.resource()),
      scanner_entry_(scanner_entry),
      finished_(!body_plan_.requires_consumption()) {
    const auto* chunked = body_plan_.chunked();
    if (chunked != nullptr && !chunked->transfer_codings().empty()) {
        const auto& codings = chunked->transfer_codings().values_;
        transfer_decoder_ = make_pmr_object<http_transfer_coding_stack_decoder>(allocator.resource(),
            std::span<const http_transfer_coding>(codings.data(), codings.size()),
            allocator.resource(), body_limit_value);
    }
}

template <typename stream_type>
http1_request_body_consumption stream_body_reader<stream_type>::consumption() const noexcept {
    return finished_ ? http1_request_body_consumption::complete
                     : http1_request_body_consumption::incomplete;
}

template <typename stream_type>
void stream_body_reader<stream_type>::take_pipeline(std::pmr::string& stash) {
    compact_pending();
    // The remainder is split across at most two places: the bytes still sitting
    // in the connection read buffer behind the body, and the bytes this reader
    // over-read from the socket into its own buffer. Neither aliases `stash`.
    const auto initial_pipeline = initial_pipeline_remainder();
    const auto buffered_pipeline = buffered_pipeline_remainder();
    stash.clear();
    stash.reserve(initial_pipeline.size() + buffered_pipeline.size());
    stash.append(initial_pipeline);
    stash.append(buffered_pipeline);
    reset_pipeline_state();
}

template <typename stream_type>
task<std::optional<std::span<const std::byte>>> stream_body_reader<stream_type>::read() {
    if (body_plan_.chunked() != nullptr) {
        co_await ensure_continue();
        co_return co_await read_transfer_decoded_chunked();
    }
    const auto* known_length = body_plan_.known_length();
    if (known_length == nullptr) {
        co_return std::nullopt;
    }
    if (exceeds_limit(known_length->content_length())) {
        throw_request_body_too_large();
    }

    co_await ensure_continue();
    co_return co_await read_known_length(known_length->content_length());
}

template <typename stream_type>
task<std::string_view> stream_body_reader<stream_type>::read_all(std::pmr::string& body) {
    if (const auto* known_length = body_plan_.known_length()) {
        co_return co_await read_known_length_all(body, known_length->content_length());
    }
    if (body_plan_.without_body() != nullptr) {
        co_return std::string_view(body);
    }

    co_await ensure_continue();
    while (auto chunk = co_await read_chunked()) {
        if (transfer_decoder_ != nullptr) {
            decode_transfer_append(::ruvia::as_chars(*chunk), body);
        } else {
            if (body_limit_.addition_exceeds(body.size(), chunk->size())) {
                throw_request_body_too_large();
            }
            body.append(::ruvia::as_chars(*chunk));
        }
    }
    if (transfer_decoder_ != nullptr) {
        require_complete_transfer_coding(*transfer_decoder_);
    }
    mark_finished();
    co_return std::string_view(body);
}

template <typename stream_type>
void stream_body_reader<stream_type>::decode_transfer_append(
    std::string_view input, std::pmr::string& target) {
    for (;;) {
        const auto old_size = target.size();
        ::ruvia::resize_pmr_string_for_overwrite(target, old_size + http_body_buffer_bytes);
        const auto result_value = transfer_decoder_->decode(
            input, std::span<char>(target.data() + old_size, http_body_buffer_bytes));
        input.remove_prefix(std::min(input.size(), result_value.consumed_bytes()));
        if (const auto* output = result_value.output()) {
            target.resize(old_size + output->bytes().size());
            continue;
        }
        target.resize(old_size);
        if (const auto* failure = result_value.failure()) {
            throw_transfer_coding_protocol_failure(*failure);
        }
        if (result_value.decoder_failure() != nullptr) {
            throw_http_transfer_coding_decoder_failure();
        }
        if (result_value.complete() != nullptr) {
            return;
        }
        if (result_value.need_input() != nullptr) {
            return;
        }
        throw std::logic_error("unexpected transfer-coding decode result");
    }
}

template <typename stream_type>
task<void> stream_body_reader<stream_type>::ensure_continue() {
    const auto expectation_plan =
        body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
    if (expectation_plan.send_continue() != nullptr && !continue_sent_) {
        co_await write_http1_continue(stream_);
        continue_sent_ = true;
    }
}

template <typename stream_type>
task<void> stream_body_reader<stream_type>::read_more() {
    compact_pending();
    const auto old_size = buffer_.size();
    const auto hard_limit =
        body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
    if (old_size >= hard_limit) {
        throw_request_body_too_large();
    }
    if (old_size == buffer_.capacity()) {
        const auto next_capacity = std::min<std::size_t>(
            std::max<std::size_t>(buffer_.capacity() * 2, old_size + http_body_buffer_bytes),
            hard_limit);
        buffer_.reserve(next_capacity);
    }
    const auto writable = std::min<std::size_t>(http_body_buffer_bytes, hard_limit - old_size);
    ::ruvia::resize_pmr_string_for_overwrite(buffer_, old_size + writable);

    scanner_entry_.set_phase(ruvia::connection_scanner::phase_type::reading_payload);
    auto read_completion =
        co_await ruvia::async_asio<std::size_t>([this, old_size, writable](auto handler) mutable {
            stream_.async_read_some(
                asio::buffer(buffer_.data() + old_size, writable), std::move(handler));
        });
    const auto ec = read_completion.error_code();
    const auto bytes_read = read_completion.result();
    if (ec || bytes_read == 0) {
        throw_incomplete_request_body();
    }

    buffer_.resize(old_size + bytes_read);
    scanner_entry_.touch();
}

template <typename stream_type>
bool stream_body_reader<stream_type>::exceeds_limit(std::size_t bytes_value) const noexcept {
    return body_limit_.exceeds(bytes_value);
}

template <typename stream_type>
void stream_body_reader<stream_type>::mark_finished() noexcept {
    finished_ = true;
    scanner_entry_.set_phase(ruvia::connection_scanner::phase_type::idle);
}

}  // namespace ruvia::detail
