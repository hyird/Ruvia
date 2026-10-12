#pragma once

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
void stream_body_reader<stream_type>::materialize_initial_remainder() {
    if (initial_body_and_pipeline_.empty()) {
        return;
    }
    buffer_.clear();
    buffer_.append(initial_body_and_pipeline_.substr(read_cursor_));
    initial_body_and_pipeline_ = {};
    read_cursor_ = 0;
}

}  // namespace ruvia::detail
