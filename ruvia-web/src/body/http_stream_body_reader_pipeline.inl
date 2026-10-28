#pragma once

#include <algorithm>
#include <string_view>

#include "body/http_stream_body_reader.h"

namespace ruvia::detail {

template <typename stream_type>
std::string_view stream_body_reader<stream_type>::initial_pipeline_remainder() const noexcept {
    if (initial_body_and_pipeline_.empty()) {
        return {};
    }
    if (body_plan_.chunked() != nullptr) {
        return initial_body_and_pipeline_.substr(
            std::min(read_cursor_, initial_body_and_pipeline_.size()));
    }
    const auto* known_length = body_plan_.known_length();
    if (known_length == nullptr) {
        return initial_body_and_pipeline_;
    }
    const auto initial_body_bytes =
        std::min(known_length->content_length(), initial_body_and_pipeline_.size());
    return initial_body_and_pipeline_.substr(initial_body_bytes);
}

template <typename stream_type>
std::string_view stream_body_reader<stream_type>::buffered_pipeline_remainder() const noexcept {
    if (read_cursor_ >= buffer_.size()) {
        return {};
    }
    return std::string_view(buffer_.data() + read_cursor_, buffer_.size() - read_cursor_);
}

template <typename stream_type>
void stream_body_reader<stream_type>::reset_pipeline_state() noexcept {
    buffer_.clear();
    initial_body_and_pipeline_ = {};
    read_cursor_ = 0;
}

template <typename stream_type>
void stream_body_reader<stream_type>::materialize_initial_remainder(std::size_t extra_reserve) {
    if (initial_body_and_pipeline_.empty()) {
        return;
    }
    const auto remainder =
        initial_body_and_pipeline_.substr(std::min(read_cursor_, initial_body_and_pipeline_.size()));
    buffer_.clear();
    if (!remainder.empty() || extra_reserve != 0) {
        const auto hard_limit =
            body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
        const auto target_reserve = extra_reserve > hard_limit - std::min(remainder.size(), hard_limit)
                                        ? hard_limit
                                        : remainder.size() + extra_reserve;
        if (target_reserve > buffer_.capacity()) {
            buffer_.reserve(std::max(remainder.size(), target_reserve));
        }
        buffer_.append(remainder);
    }
    initial_body_and_pipeline_ = {};
    read_cursor_ = 0;
}

}  // namespace ruvia::detail
