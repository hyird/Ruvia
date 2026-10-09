#include "ruvia/http/http3_buffered_response_cursor.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

namespace ruvia {

http3_buffered_response_cursor::http3_buffered_response_cursor(std::pmr::memory_resource* resource)
    : headers_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

http3_buffered_response_cursor& http3_buffered_response_cursor::require_no_outstanding_segment(
    http3_buffered_response_cursor& other) {
    if (other.offered_) {
        throw std::logic_error("cannot move an HTTP/3 response cursor with an outstanding span");
    }
    return other;
}

http3_buffered_response_cursor::http3_buffered_response_cursor(http3_buffered_response_cursor&& other)
    : headers_(std::move(require_no_outstanding_segment(other).headers_)),
      decoded_field_section_size_(std::exchange(other.decoded_field_section_size_, 0)),
      body_(other.body_),
      data_plan_(std::move(other.data_plan_)),
      chunk_(other.chunk_),
      segment_offset_(other.segment_offset_),
      body_offset_(other.body_offset_),
      state_(other.state_),
      chunk_pending_(other.chunk_pending_) {
    other.state_ = state::failed;
}

std::variant<http3_buffered_response_cursor, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::create(const http_response& response,
    const http_buffered_response_write_plan& write_plan, std::pmr::memory_resource* resource) noexcept {
    if (!write_plan.matches_response(response)) {
        return error::invalid_response_plan;
    }
    if (response.file_body() && write_plan.send_body() && write_plan.content_length() != 0) {
        return error::file_body_unsupported;
    }
    try {
        auto encoded = encode_http3_response_head(response, write_plan, {}, resource);
        if ((encoded.index() != 0)) {
            return error::response_encoding;
        }
        return create(response, write_plan, std::move(std::get<0>(encoded)), resource);
    } catch (const std::bad_alloc&) {
        return error::out_of_memory;
    } catch (...) {
        return error::response_encoding;
    }
}

std::variant<http3_buffered_response_cursor, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::create(const http_response& response, const http_buffered_response_write_plan& write_plan,
    http3_response_head encoded_head, std::pmr::memory_resource* resource) noexcept {
    if (!write_plan.matches_response(response)) {
        return error::invalid_response_plan;
    }
    if (response.file_body() && write_plan.send_body() && write_plan.content_length() != 0) {
        return error::file_body_unsupported;
    }
    try {
        http3_buffered_response_cursor cursor(resource);
        const auto* encoded = &encoded_head.field_section_;
        cursor.decoded_field_section_size_ = encoded->decoded_field_section_size();
        if (encoded->field_section_.size() > std::numeric_limits<std::size_t>::max() -
                                                 http3_frame_header_max_bytes) {
            return error::response_encoding;
        }
        cursor.headers_.resize(http3_frame_header_max_bytes + encoded->field_section_.size());
        const auto frame_header = encode_http3_frame_header(cursor.headers_,
            static_cast<std::uint64_t>(http3_frame_type::headers), encoded->field_section_.size());
        if ((frame_header.index() != 0)) {
            return error::response_encoding;
        }
        cursor.headers_.resize(std::get<0>(frame_header) + encoded->field_section_.size());
        std::copy(encoded->field_section_.begin(), encoded->field_section_.end(),
            cursor.headers_.begin() + static_cast<std::ptrdiff_t>(std::get<0>(frame_header)));

        const auto body_plan = write_plan.body_plan();
        const bool send_body = write_plan.send_body();
        cursor.body_ = send_body ? response.body_bytes() : std::string_view{};
        if (send_body && cursor.body_.size() != write_plan.content_length()) {
            return error::invalid_response_plan;
        }
        const std::optional<std::uint64_t> length = send_body
                                                        ? std::optional<std::uint64_t>(write_plan.content_length())
                                                        : std::nullopt;
        cursor.data_plan_.emplace(body_plan, length);
        return cursor;
    } catch (const std::bad_alloc&) {
        return error::out_of_memory;
    } catch (...) {
        return error::response_encoding;
    }
}

std::variant<http3_buffered_response_cursor::segment, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::next() noexcept {
    if (state_ == state::finished || state_ == state::failed) {
        return error::invalid_state;
    }
    if (state_ == state::fin) {
        return segment{};
    }
    const auto segment = active_segment();
    offered_ = !segment.empty();
    return segment;
}

http3_buffered_response_cursor::step http3_buffered_response_cursor::next_step() const noexcept {
    switch (state_) {
        case state::headers:
            return segment_offset_ < headers_.size() ? step::bytes : step::failed;
        case state::data_header:
            return chunk_pending_ && chunk_.emits_data_ &&
                           segment_offset_ < chunk_.frame_header_size_
                       ? step::bytes
                       : step::failed;
        case state::data_body:
            return chunk_pending_ && !chunk_.payload_.empty() &&
                           segment_offset_ < chunk_.payload_.size()
                       ? step::bytes
                       : step::failed;
        case state::fin:
            return data_plan_ && chunk_pending_ ? step::fin : step::failed;
        case state::finished:
            return step::complete;
        case state::failed:
            return step::failed;
    }
    return step::failed;
}

http3_buffered_response_cursor::segment http3_buffered_response_cursor::active_segment() const noexcept {
    switch (state_) {
        case state::headers:
            return segment(headers_).subspan(segment_offset_);
        case state::data_header:
            return segment(chunk_.frame_header_.data(), chunk_.frame_header_size_).subspan(segment_offset_);
        case state::data_body:
            return chunk_.payload_.subspan(segment_offset_);
        case state::fin:
        case state::finished:
        case state::failed:
            return {};
    }
    return {};
}

std::variant<std::monostate, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::prepare_data() noexcept {
    if (!data_plan_ || state_ != state::data_header || chunk_pending_) {
        return error::invalid_state;
    }
    const auto remaining = body_.size() - body_offset_;
    const auto chunk_size = std::min<std::uint64_t>(remaining, http3_var_int_max);
    const bool finishing = chunk_size == remaining;
    const auto payload_value = body_.substr(body_offset_, static_cast<std::size_t>(chunk_size));
    auto planned = data_plan_->plan_chunk(payload_value, finishing);
    if ((planned.index() != 0)) {
        return fail_data_plan();
    }
    chunk_ = std::get<0>(planned);
    chunk_pending_ = true;
    segment_offset_ = 0;
    if (!chunk_.emits_data_) {
        state_ = state::fin;
        chunk_pending_ = true;
        return {};
    }
    return {};
}

std::variant<std::monostate, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::acknowledge(std::size_t count) noexcept {
    if (state_ == state::finished || state_ == state::failed || state_ == state::fin ||
        !offered_) {
        return error::invalid_state;
    }
    const auto segment = active_segment();
    if (count > segment.size()) {
        return error::excessive_acknowledgement;
    }
    if (count == 0) {
        return {};
    }

    if (state_ == state::headers) {
        segment_offset_ += count;
        if (segment_offset_ == headers_.size()) {
            offered_ = false;
            state_ = state::data_header;
            segment_offset_ = 0;
            return prepare_data();
        }
        return {};
    }
    if (state_ == state::data_header) {
        segment_offset_ += count;
        if (segment_offset_ == chunk_.frame_header_size_) {
            offered_ = false;
            state_ = state::data_body;
            segment_offset_ = 0;
        }
        return {};
    }
    if (state_ == state::data_body) {
        segment_offset_ += count;
        if (segment_offset_ == chunk_.payload_.size()) {
            offered_ = false;
            const auto chunk_bytes = chunk_.payload_.size();
            if (chunk_.finishing_) {
                state_ = state::fin;
                return {};
            }
            if (auto committed = data_plan_->commit_payload(chunk_bytes, false); (committed.index() != 0)) {
                return fail_data_plan();
            }
            body_offset_ += chunk_bytes;
            chunk_pending_ = false;
            segment_offset_ = 0;
            state_ = state::data_header;
            return prepare_data();
        }
        return {};
    }
    return error::invalid_state;
}

std::variant<std::monostate, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::acknowledge_fin(bool successful) noexcept {
    if (state_ != state::fin || !data_plan_ || !chunk_pending_) {
        return error::invalid_state;
    }
    if (!successful) {
        state_ = state::failed;
        chunk_pending_ = false;
        return {};
    }
    const auto committed = data_plan_->commit_payload(chunk_.payload_.size(), true);
    if ((committed.index() != 0)) {
        return fail_data_plan();
    }
    state_ = state::finished;
    chunk_pending_ = false;
    return {};
}

bool http3_buffered_response_cursor::fin_ready() const noexcept {
    return state_ == state::fin;
}

bool http3_buffered_response_cursor::finished() const noexcept {
    return state_ == state::finished;
}

bool http3_buffered_response_cursor::failed() const noexcept {
    return state_ == state::failed;
}

std::variant<std::monostate, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::fail_data_plan() noexcept {
    state_ = state::failed;
    chunk_pending_ = false;
    return error::data_plan;
}

}  // namespace ruvia
