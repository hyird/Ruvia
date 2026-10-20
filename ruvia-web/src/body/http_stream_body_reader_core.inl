#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"

#include "body/http_body_buffer.h"
#include "body/http_stream_body_reader.h"
#include "body/http_stream_body_reader_errors.h"

namespace ruvia::detail {

template <typename stream_type>
stream_body_reader<stream_type>::stream_body_reader(stream_type& stream,
    std::pmr::polymorphic_allocator<char> allocator, std::string_view initial_body_and_pipeline,
    http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
    connection_scanner::entry_type& scanner_entry, http_interim_response_output& interim_output)
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
      interim_output_(interim_output),
      finished_(!body_plan_.requires_consumption()) {
    const auto expectation_plan =
        body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
    continue_pending_ = expectation_plan.send_continue() != nullptr;
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
body_pipeline_handover stream_body_reader<stream_type>::take_pipeline(std::pmr::string& stash) {
    // The reader fills its own buffer only after the borrowed initial segment
    // is exhausted as body (or materialized into that buffer), so the suffix
    // is either the tail of the connection read buffer or entirely over-read
    // bytes. Only the latter must outlive this reader.
    const auto initial_pipeline = initial_pipeline_remainder();
    const auto buffered_pipeline = buffered_pipeline_remainder();
    body_pipeline_handover handover;
    if (buffered_pipeline.empty()) {
        handover.read_buffer_suffix_bytes_ = initial_pipeline.size();
    } else {
        stash.clear();
        stash.reserve(initial_pipeline.size() + buffered_pipeline.size());
        stash.append(initial_pipeline);
        stash.append(buffered_pipeline);
        handover.stashed_ = stash;
    }
    reset_pipeline_state();
    return handover;
}

template <typename stream_type>
task<std::optional<std::span<const std::byte>>> stream_body_reader<stream_type>::read() {
    require_not_failed();
    try {
        if (body_plan_.chunked() != nullptr) {
            if (continue_pending_) {
                co_await send_continue();
            }
            co_return co_await read_transfer_decoded_chunked();
        }
        const auto* known_length = body_plan_.known_length();
        if (known_length == nullptr) {
            co_return std::nullopt;
        }
        if (exceeds_limit(known_length->content_length())) {
            throw_request_body_too_large();
        }

        if (continue_pending_) {
            co_await send_continue();
        }
        co_return co_await read_known_length(known_length->content_length());
    } catch (...) {
        failed_ = true;
        throw;
    }
}

template <typename stream_type>
task<std::string_view> stream_body_reader<stream_type>::read_all(std::pmr::string& body) {
    require_not_failed();
    try {
        if (const auto* known_length = body_plan_.known_length()) {
            co_return co_await read_known_length_all(body, known_length->content_length());
        }
        if (body_plan_.without_body() != nullptr) {
            co_return std::string_view(body);
        }

        if (continue_pending_) {
            co_await send_continue();
        }
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
    } catch (...) {
        failed_ = true;
        throw;
    }
}

// The transfer decoder's output scratch: a fixed window sized once per reader.
template <typename stream_type>
std::span<char> stream_body_reader<stream_type>::transfer_output() {
    if (transfer_output_.empty()) {
        transfer_output_.resize(http_body_buffer_bytes);
    }
    return std::span<char>(transfer_output_);
}

// Decodes into the reused scratch and appends what the decoder produced, so
// `target` only ever grows by written bytes.
template <typename stream_type>
void stream_body_reader<stream_type>::decode_transfer_append(
    std::string_view input, std::pmr::string& target) {
    const auto output_window = transfer_output();
    for (;;) {
        const auto result_value = transfer_decoder_->decode(input, output_window);
        input.remove_prefix(std::min(input.size(), result_value.consumed_bytes()));
        if (const auto* output = result_value.output()) {
            target.append(output->bytes());
            continue;
        }
        require_transfer_coding_progress(result_value);
        return;
    }
}

template <typename stream_type>
task<void> stream_body_reader<stream_type>::send_continue() {
    co_await interim_output_.send_continue();
    continue_pending_ = false;
}

template <typename stream_type>
void stream_body_reader<stream_type>::require_not_failed() const {
    if (failed_) {
        throw std::logic_error("request body consumption previously failed");
    }
}

template <typename stream_type>
task<void> stream_body_reader<stream_type>::read_more() {
    // Every view handed out by an earlier read is dead once more input is
    // needed, so this is where the consumed prefix is finally dropped. Only the
    // unread framing suffix moves, once per socket read, instead of once per
    // returned chunk.
    if (read_cursor_ != 0) {
        buffer_.consume(read_cursor_);
        read_cursor_ = 0;
    }
    const auto old_size = buffer_.size();
    const auto hard_limit =
        body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
    if (old_size >= hard_limit) {
        throw_request_body_too_large();
    }
    const auto writable = std::min<std::size_t>(http_body_buffer_bytes, hard_limit - old_size);
    if (buffer_.capacity() - old_size < writable) {
        const auto doubled = buffer_.capacity() > hard_limit / 2 ? hard_limit : buffer_.capacity() * 2;
        buffer_.reserve(std::min(std::max(doubled, old_size + writable), hard_limit));
    }
    const auto read_space = buffer_.prepare(writable);

    scanner_entry_.set_phase(connection_scanner::phase_type::reading_payload);
    auto read_completion =
        co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
            stream_.async_read_some(asio::buffer(read_space.data(), read_space.size()), std::move(handler));
        });
    const auto ec = read_completion.error_code();
    const auto bytes_read = read_completion.result();
    if (ec || bytes_read == 0) {
        throw_incomplete_request_body();
    }
    buffer_.commit(bytes_read);
    scanner_entry_.touch();
}

template <typename stream_type>
bool stream_body_reader<stream_type>::exceeds_limit(std::size_t bytes_value) const noexcept {
    return body_limit_.exceeds(bytes_value);
}

template <typename stream_type>
void stream_body_reader<stream_type>::mark_finished() noexcept {
    finished_ = true;
    scanner_entry_.set_phase(connection_scanner::phase_type::idle);
}

}  // namespace ruvia::detail
