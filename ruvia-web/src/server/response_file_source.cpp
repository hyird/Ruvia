#include "server/response_file_source.h"

#include <algorithm>
#include <ios>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] std::pmr::memory_resource* select_file_read_resource(const response_file_io& io) noexcept {
    return io.pool_ == nullptr && io.resource_ != nullptr ? io.resource_ : process_resource();
}

}  // namespace

response_file_source::read_buffer::read_buffer(read_buffer&& other) noexcept
    : resource_(other.resource_),
      data_(std::exchange(other.data_, nullptr)) {}

response_file_source::read_buffer& response_file_source::read_buffer::operator=(read_buffer&& other) noexcept {
    if (this != &other) {
        if (data_ != nullptr) {
            resource_->deallocate(data_, response_file_read_ahead_bytes, alignof(std::max_align_t));
        }
        resource_ = other.resource_;
        data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
}

response_file_source::read_buffer::~read_buffer() {
    if (data_ != nullptr) {
        resource_->deallocate(data_, response_file_read_ahead_bytes, alignof(std::max_align_t));
    }
}

char* response_file_source::read_buffer::data() {
    if (data_ == nullptr) {
        data_ = static_cast<char*>(
            resource_->allocate(response_file_read_ahead_bytes, alignof(std::max_align_t)));
    }
    return data_;
}

response_file_source::response_file_source(const http_response& response, response_file_io io)
    : response_(response),
      io_(std::move(io)),
      state_(select_file_read_resource(io_)),
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
#if defined(__linux__)
        state_.opened_.reset();
        if (const auto* opened = file.opened_handle()) {
            // 独立 fd 随读取状态转移；取消后响应可先析构，迟到读取仍安全。
            const auto duplicate = ::fcntl(borrowed_native_file(*opened), F_DUPFD_CLOEXEC, 0);
            if (duplicate < 0) {
                state_.error_ = std::error_code(errno, std::system_category());
            } else {
                state_.opened_ = native_file_handle(duplicate);
            }
        }
#endif
    }
    state_.position_ = file.offset();
    state_.reposition_ = true;
}

void response_file_source::read_ahead(read_state& state, std::size_t bytes, bool completes_range) {
    if (state.reopen_) {
#if defined(__linux__)
        const auto file = http_response_file_view(state.path_.c_str(), state.size_, 0, state.size_, state.identity_);
        if (state.opened_.get() >= 0) {
            state.input_.emplace(std::move(state.opened_), file);
        } else {
            state.input_.emplace(file);
        }
#else
        state.input_.emplace(
            http_response_file_view(state.path_.c_str(), state.size_, 0, state.size_, state.identity_));
#endif
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
    const auto remaining = bytes - state.filled_;
    if (!read_exact(input, state.buffer_.data() + state.filled_, remaining)) {
        // The committed length can no longer be honoured.
        state.error_ = std::make_error_code(std::errc::io_error);
        return;
    }
    state.filled_ = bytes;
    state.position_ += remaining;
    if (completes_range && !input.matches_snapshot(state.identity_, state.size_)) {
        state.error_ = std::make_error_code(std::errc::operation_canceled);
    }
}

task<bool> response_file_source::fill(std::size_t bytes, bool completes_range) {
    if (lost_ || state_.error_) {
        co_return false;
    }
    state_.filled_ = 0;
#if defined(__linux__)
    if (!state_.reopen_ && state_.input_) {
        state_.filled_ = state_.input_->read_cached(state_.position_, state_.buffer_.data(), bytes);
        state_.position_ += state_.filled_;
        state_.reposition_ = true;
        if (state_.filled_ == bytes) {
            if (completes_range && !state_.input_->matches_snapshot(state_.identity_, state_.size_)) {
                state_.error_ = std::make_error_code(std::errc::operation_canceled);
            }
            co_return !state_.error_;
        }
    }
#endif
    if (io_.pool_ == nullptr) {
#if defined(__linux__)
        state_.error_ = std::make_error_code(std::errc::operation_not_supported);
        co_return false;
#else
        read_ahead(state_, bytes, completes_range);
        co_return !state_.error_;
#endif
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
