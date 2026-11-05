#include "server/response_file_source.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/error.h"

namespace ruvia::detail {

namespace {

// A file removed, replaced or rewritten since its response was framed: a
// deployment race the client sees as an error response, not a server fault.
[[nodiscard]] bool is_file_changed_since_framing(std::error_code error) noexcept {
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
           error == std::errc::state_not_recoverable;
}

}  // namespace

response_file_failure response_file_failure::read_failed(std::error_code error) {
    if (is_file_changed_since_framing(error)) {
        return response_file_failure(false, nullptr);
    }
    return response_file_failure(false, std::make_exception_ptr(std::system_error(error, "file response body read failed")));
}

response_file_failure response_file_failure::read_not_completed(blocking_status status) {
    // A worker without a pool is a configuration fault, and a read still
    // unfinished after the write timeout is a stalled filesystem. Overload and
    // shutdown are not faults of this response.
    std::exception_ptr fault;
    if (status == blocking_status::unavailable) {
        fault = std::make_exception_ptr(std::system_error(std::make_error_code(std::errc::operation_not_supported),
            "file response body needs a blocking pool"));
    } else if (status == blocking_status::timed_out) {
        fault = std::make_exception_ptr(
            std::system_error(std::make_error_code(std::errc::timed_out), "file response body read timed out"));
    }
    return response_file_failure(true, std::move(fault));
}

response_file_failure response_file_failure::read_threw(std::exception_ptr exception) noexcept {
    return response_file_failure(false, std::move(exception));
}

std::exception_ptr response_file_failure::response_error() const {
    if (unavailable_) {
        return std::make_exception_ptr(http_error({.status_ = ruvia::http_status::service_unavailable,
            .code_ = "file_read_unavailable",
            .message_ = "file could not be read in time"}));
    }
    return std::make_exception_ptr(http_error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "file_read_failed",
        .message_ = "file could not be read"}));
}

response_file_source::read_buffer::read_buffer(read_buffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)) {}

response_file_source::read_buffer& response_file_source::read_buffer::operator=(read_buffer&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
}

response_file_source::read_buffer::~read_buffer() {
    reset();
}

char* response_file_source::read_buffer::data() {
    if (data_ == nullptr) {
        data_ = static_cast<char*>(
            process_resource()->allocate(response_file_read_ahead_bytes, alignof(std::max_align_t)));
    }
    return data_;
}

void response_file_source::read_buffer::reset() noexcept {
    if (auto* const data = std::exchange(data_, nullptr); data != nullptr) {
        process_resource()->deallocate(data, response_file_read_ahead_bytes, alignof(std::max_align_t));
    }
}

// The path travels with the state to the pool thread, so it lives in the
// synchronized process resource.
response_file_source::read_state::read_state()
    : path_(process_resource()) {}

response_file_source::response_file_source(const http_response& response, response_file_io io)
    : response_(response),
      io_(std::move(io)),
      segment_count_(response.body_segment_count()) {}

void response_file_source::select_file(http_response_file_view file) {
    // Multipart byte ranges name the same file view: keep its validated
    // handle and only reposition.
    if (current_path_ != file.native_path_c_str() || state_.identity_ != file.identity() ||
        state_.size_ != file.size()) {
        current_path_ = file.native_path_c_str();
        state_.path_.assign(file.native_path_c_str());
        state_.size_ = file.size();
        state_.identity_ = file.identity();
        state_.validated_ = false;
        state_.input_.reset();
        if (const auto* opened = file.opened_handle()) {
            // An independent handle travels with the read state: a cancelled
            // response may be destroyed before a late read finishes.
            state_.input_ = duplicate_native_file(*opened, state_.error_);
        }
    }
    state_.position_ = file.offset();
}

void response_file_source::read_ahead(read_state& state, std::size_t bytes, bool completes_range) {
    if (!state.validated_) {
        if (!state.input_) {
            state.input_ = open_native_file(state.path_.c_str(), state.error_);
            if (state.error_) {
                return;
            }
        }
        state.error_ = validate_native_file(state.input_.get(), state.identity_, state.size_);
        if (state.error_) {
            return;
        }
        state.validated_ = true;
    }
    const auto remaining = bytes - state.filled_;
    if (remaining != 0) {
        state.error_ =
            read_native_file_at(state.input_.get(), state.position_, state.buffer_.data() + state.filled_, remaining);
        if (state.error_) {
            return;
        }
    }
    state.filled_ = bytes;
    state.position_ += remaining;
    if (completes_range) {
        state.error_ = validate_native_file(state.input_.get(), state.identity_, state.size_);
    }
}

bool response_file_source::settle_read() {
    if (state_.error_ && !failure_) {
        failure_.emplace(response_file_failure::read_failed(state_.error_));
    }
    return !failure_;
}

task<bool> response_file_source::fill(std::size_t bytes, bool completes_range) {
    if (!settle_read()) {
        co_return false;
    }
    state_.filled_ = 0;
#if defined(__linux__)
    // A validated handle first takes what the page cache already holds; only
    // a remainder waits on the pool.
    if (bytes != 0 && state_.validated_) {
        state_.filled_ = read_native_file_cached(
            state_.input_.get(), state_.position_, state_.buffer_.data(), bytes, state_.cached_read_supported_);
        state_.position_ += state_.filled_;
        if (state_.filled_ == bytes) {
            if (completes_range) {
                state_.error_ = validate_native_file(state_.input_.get(), state_.identity_, state_.size_);
            }
            co_return settle_read();
        }
    }
#endif
    auto result = co_await run_blocking(io_.worker_, {.timeout_ = io_.timeout_, .stop_token_ = io_.stop_},
        [state = std::move(state_), bytes, completes_range]() mutable {
            read_ahead(state, bytes, completes_range);
            return std::move(state);
        });
    if (result.status() != blocking_status::completed) {
        // The state (handle and buffer) stayed with the abandoned operation.
        failure_.emplace(response_file_failure::read_not_completed(result.status()));
        co_return false;
    }
    if (result.failed()) {
        // A throwing read retired its owned state on the pool thread.
        failure_.emplace(response_file_failure::read_threw(result.error()));
        co_return false;
    }
    state_ = std::move(result).value();
    co_return settle_read();
}

task<bool> response_file_source::open_first() {
    if (first_file_opened_) {
        co_return !failure_;
    }
    first_file_opened_ = true;
    for (auto index = next_segment_; index < segment_count_; ++index) {
        if (const auto segment = response_.body_segment(index); segment.file_) {
            select_file(*segment.file_);
            co_return co_await fill(0, segment.file_->length() == 0);
        }
    }
    co_return true;
}

task<response_body_chunk> response_file_source::next(std::size_t max_bytes) {
    // A body that opens with in-memory bytes (a multipart boundary) still has
    // its first file range validated before anything is yielded. A body that
    // opens with a file validates it on the read that yields its first chunk.
    if (!first_file_opened_ && next_segment_ < segment_count_ && !response_.body_segment(next_segment_).file_ &&
        !(co_await open_first())) {
        co_return response_body_chunk::failed();
    }
    first_file_opened_ = true;
    for (;;) {
        if (range_remaining_ != 0) {
            const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                std::min(max_bytes, response_file_read_ahead_bytes), range_remaining_));
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
            select_file(*segment.file_);
            range_remaining_ = segment.file_->length();
            // Empty files still complete a range: validate their snapshot
            // through the same owned I/O state before releasing the head.
            if (range_remaining_ == 0 && !(co_await fill(0, true))) {
                co_return response_body_chunk::failed();
            }
            continue;
        }
        if (!segment.bytes_.empty()) {
            co_return response_body_chunk::data(segment.bytes_);
        }
    }
}

void response_file_source::release_buffer() noexcept {
    state_.buffer_.reset();
}

bool response_file_source::file_bytes_pending() const noexcept {
    if (range_remaining_ != 0) {
        return true;
    }
    // next() validates an empty file range without reading it and skips
    // empty in-memory segments.
    for (auto index = next_segment_; index < segment_count_; ++index) {
        const auto segment = response_.body_segment(index);
        if (segment.file_) {
            if (segment.file_->length() != 0) {
                return true;
            }
        } else if (!segment.bytes_.empty()) {
            return false;
        }
    }
    return false;
}

}  // namespace ruvia::detail
