#include "server/response_file_source.h"

#include <algorithm>
#include <ios>
#include <utility>

#include "ruvia/core/memory/process_resource.h"

namespace ruvia::detail {

response_file_source::read_buffer::read_buffer(read_buffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)) {}

response_file_source::read_buffer& response_file_source::read_buffer::operator=(read_buffer&& other) noexcept {
    if (this != &other) {
        if (data_ != nullptr) {
            process_resource()->deallocate(data_, response_file_read_ahead_bytes, alignof(std::max_align_t));
        }
        data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
}

response_file_source::read_buffer::~read_buffer() {
    if (data_ != nullptr) {
        process_resource()->deallocate(data_, response_file_read_ahead_bytes, alignof(std::max_align_t));
    }
}

char* response_file_source::read_buffer::data() {
    if (data_ == nullptr) {
        data_ = static_cast<char*>(
            process_resource()->allocate(response_file_read_ahead_bytes, alignof(std::max_align_t)));
    }
    return data_;
}

response_file_source::response_file_source(const http_response& response, response_file_io io)
    : response_(response),
      io_(std::move(io)),
      state_(process_resource()),
      segment_count_(response.body_segment_count()) {}

void response_file_source::select_file(http_response_file_view file) {
    // Multipart byte ranges name the same file view: keep its open handle
    // and only reposition.
    if (current_path_ != file.native_path_c_str() || state_.identity_ != file.identity() ||
        state_.size_ != file.size()) {
        current_path_ = file.native_path_c_str();
        state_.path_.assign(file.native_path_c_str());
        state_.size_ = file.size();
        state_.identity_ = file.identity();
        state_.reopen_ = true;
    }
    state_.position_ = file.offset();
    state_.reposition_ = true;
}

void response_file_source::read_ahead(read_state& state, std::size_t bytes, bool completes_range) {
    state.filled_ = 0;
    if (state.reopen_) {
        state.input_.emplace(
            http_response_file_view(state.path_.c_str(), state.size_, 0, state.size_, state.identity_));
        state.reopen_ = false;
        if (!*state.input_) {
            state.error_ = std::make_error_code(std::errc::no_such_file_or_directory);
            return;
        }
    }
    auto& input = *state.input_;
    if (state.reposition_) {
        input.seekg(static_cast<std::streamoff>(state.position_));
        state.reposition_ = false;
        if (!input) {
            state.error_ = std::make_error_code(std::errc::invalid_seek);
            return;
        }
    }
    if (!read_exact(input, state.buffer_.data(), bytes)) {
        // The committed length can no longer be honoured.
        state.error_ = std::make_error_code(std::errc::io_error);
        return;
    }
    state.filled_ = bytes;
    state.position_ += state.filled_;
    if (completes_range && !input.matches_snapshot(state.identity_, state.size_)) {
        state.error_ = std::make_error_code(std::errc::operation_canceled);
    }
}

task<bool> response_file_source::fill(std::size_t bytes, bool completes_range) {
    if (lost_ || state_.error_) {
        co_return false;
    }
    if (io_.pool_ == nullptr) {
        read_ahead(state_, bytes, completes_range);
        co_return !state_.error_;
    }
    auto result = co_await run_blocking(*io_.pool_, io_.worker_, io_.stop_,
        [state = std::move(state_), bytes, completes_range]() mutable {
            read_ahead(state, bytes, completes_range);
            return std::move(state);
        });
    if (result.status() != blocking_status::completed) {
        // The state (handle and buffer) stayed with the abandoned operation.
        lost_ = true;
        co_return false;
    }
    if (result.failed()) {
        // A throwing read retired its owned state on the pool thread. Preserve
        // that failure through value(), as the inline reader does.
        lost_ = true;
    }
    state_ = std::move(result).value();
    co_return !state_.error_;
}

task<response_body_chunk> response_file_source::next() {
    for (;;) {
        if (range_remaining_ != 0) {
            const auto bytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(response_file_read_ahead_bytes, range_remaining_));
            if (!(co_await fill(bytes, bytes == range_remaining_))) {
                co_return response_body_chunk::failed();
            }
            range_remaining_ -= bytes;
            co_return response_body_chunk::data(std::string_view(state_.buffer_.data(), bytes));
        }
        if (next_segment_ >= segment_count_) {
            co_return response_body_chunk::end();
        }
        const auto segment = response_.body_segment(next_segment_++);
        if (segment.file_) {
            if (segment.file_->length() != 0) {
                select_file(*segment.file_);
                range_remaining_ = segment.file_->length();
            }
            continue;
        }
        if (!segment.bytes_.empty()) {
            co_return response_body_chunk::data(segment.bytes_);
        }
    }
}

}  // namespace ruvia::detail
