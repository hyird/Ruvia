#pragma once

#include "body/http_stream_body_reader_errors.h"

namespace ruvia::detail {

template <typename stream_type>
task<std::string_view> stream_body_reader<stream_type>::read_known_length_all(
    std::pmr::string& body, std::size_t content_length) {
    compact_pending();
    if (finished_) {
        co_return std::string_view(body);
    }
    if (exceeds_limit(content_length)) {
        throw_request_body_too_large();
    }

    const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
    if (initial_body_bytes == content_length) {
        mark_finished();
        co_return initial_body_and_pipeline_.substr(0, content_length);
    }

    co_await ensure_continue();

    // Only bytes actually received grow the owning buffer. In particular, a
    // header-only peer cannot allocate its declared Content-Length up front.
    while (auto chunk = co_await read_known_length(content_length)) {
        body.append(::ruvia::as_chars(*chunk));
    }

    co_return std::string_view(body);
}

template <typename stream_type>
task<std::optional<std::span<const std::byte>>> stream_body_reader<stream_type>::read_known_length(
    std::size_t content_length) {
    compact_pending();
    if (finished_) {
        co_return std::nullopt;
    }
    if (exceeds_limit(content_length)) {
        throw_request_body_too_large();
    }
    if (content_length == 0 || delivered_bytes_ == content_length) {
        mark_finished();
        co_return std::nullopt;
    }

    const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
    if (delivered_bytes_ < initial_body_bytes) {
        const auto remaining_body = content_length - delivered_bytes_;
        const auto available = initial_body_bytes - delivered_bytes_;
        const auto chunk_bytes = std::min(available, remaining_body);
        auto chunk = initial_body_and_pipeline_.substr(delivered_bytes_, chunk_bytes);
        delivered_bytes_ += chunk_bytes;
        if (delivered_bytes_ == content_length) {
            mark_finished();
        }
        co_return ::ruvia::as_bytes(chunk);
    }

    // The initial segment is now fully consumed as body: a partial-body prefix
    // cannot be followed by pipelined bytes, so the whole borrowed view was
    // body. Drop it before recording any buffer_-relative pending_compact_until_,
    // so compact_pending() compacts buffer_ instead of misreading that offset as
    // an initial-view offset (mirrors the chunked path's
    // materialize_initial_remainder()).
    if (initial_body_bytes == initial_body_and_pipeline_.size()) {
        initial_body_and_pipeline_ = {};
    }

    while (buffer_.size() <= read_cursor_) {
        co_await read_more();
    }

    const auto remaining_body = content_length - delivered_bytes_;
    const auto available = buffer_.size() - read_cursor_;
    const auto chunk_bytes = std::min(available, remaining_body);
    auto chunk = std::string_view(buffer_.data() + read_cursor_, chunk_bytes);
    pending_compact_until_ = read_cursor_ + chunk_bytes;
    delivered_bytes_ += chunk_bytes;
    if (delivered_bytes_ == content_length) {
        mark_finished();
    }

    co_return ::ruvia::as_bytes(chunk);
}

}  // namespace ruvia::detail
