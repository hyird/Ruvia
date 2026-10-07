#include "ruvia/http/http3_buffered_response_cursor.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

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

std::expected<http3_buffered_response_cursor, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::create(const HttpResponse& response,
    const HttpBufferedResponseWritePlan& write_plan, std::pmr::memory_resource* resource) noexcept {
    if (!write_plan.matchesResponse(response)) {
        return std::unexpected(error::invalid_response_plan);
    }
    if (response.fileBody() && write_plan.sendBody() && write_plan.contentLength() != 0) {
        return std::unexpected(error::file_body_unsupported);
    }
    try {
        auto encoded = encodeHttp3ResponseHead(response, write_plan, {}, resource);
        if (!encoded) {
            return std::unexpected(error::response_encoding);
        }
        return create(response, write_plan, std::move(*encoded), resource);
    } catch (const std::bad_alloc&) {
        return std::unexpected(error::out_of_memory);
    } catch (...) {
        return std::unexpected(error::response_encoding);
    }
}

std::expected<http3_buffered_response_cursor, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::create(const HttpResponse& response, const HttpBufferedResponseWritePlan& write_plan,
    Http3ResponseHead encoded_head, std::pmr::memory_resource* resource) noexcept {
    if (!write_plan.matchesResponse(response)) {
        return std::unexpected(error::invalid_response_plan);
    }
    if (response.fileBody() && write_plan.sendBody() && write_plan.contentLength() != 0) {
        return std::unexpected(error::file_body_unsupported);
    }
    try {
        http3_buffered_response_cursor cursor(resource);
        const auto* encoded = &encoded_head.field_section;
        cursor.decoded_field_section_size_ = encoded->decodedFieldSectionSize();
        if (encoded->fieldSection.size() > std::numeric_limits<std::size_t>::max() -
                                               kHttp3FrameHeaderMaxBytes) {
            return std::unexpected(error::response_encoding);
        }
        cursor.headers_.resize(kHttp3FrameHeaderMaxBytes + encoded->fieldSection.size());
        const auto frame_header = encodeHttp3FrameHeader(cursor.headers_,
            static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->fieldSection.size());
        if (!frame_header) {
            return std::unexpected(error::response_encoding);
        }
        cursor.headers_.resize(*frame_header + encoded->fieldSection.size());
        std::copy(encoded->fieldSection.begin(), encoded->fieldSection.end(),
            cursor.headers_.begin() + static_cast<std::ptrdiff_t>(*frame_header));

        const auto body_plan = write_plan.bodyPlan();
        const bool send_body = write_plan.sendBody();
        cursor.body_ = send_body ? response.bodyBytes() : std::string_view{};
        if (send_body && cursor.body_.size() != write_plan.contentLength()) {
            return std::unexpected(error::invalid_response_plan);
        }
        const std::optional<std::uint64_t> length = send_body
                                                        ? std::optional<std::uint64_t>(write_plan.contentLength())
                                                        : std::nullopt;
        cursor.data_plan_.emplace(body_plan, length);
        return cursor;
    } catch (const std::bad_alloc&) {
        return std::unexpected(error::out_of_memory);
    } catch (...) {
        return std::unexpected(error::response_encoding);
    }
}

std::expected<http3_buffered_response_cursor::segment, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::next() noexcept {
    if (state_ == state::finished || state_ == state::failed) {
        return std::unexpected(error::invalid_state);
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
            return chunk_pending_ && chunk_.emitsData &&
                           segment_offset_ < chunk_.frameHeaderSize
                       ? step::bytes
                       : step::failed;
        case state::data_body:
            return chunk_pending_ && !chunk_.payload.empty() &&
                           segment_offset_ < chunk_.payload.size()
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
            return segment(chunk_.frameHeader.data(), chunk_.frameHeaderSize).subspan(segment_offset_);
        case state::data_body:
            return chunk_.payload.subspan(segment_offset_);
        case state::fin:
        case state::finished:
        case state::failed:
            return {};
    }
    return {};
}

std::expected<void, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::prepare_data() noexcept {
    if (!data_plan_ || state_ != state::data_header || chunk_pending_) {
        return std::unexpected(error::invalid_state);
    }
    const auto remaining = body_.size() - body_offset_;
    const auto chunk_size = std::min<std::uint64_t>(remaining, kHttp3VarIntMax);
    const bool finishing = chunk_size == remaining;
    const auto payload = body_.substr(body_offset_, static_cast<std::size_t>(chunk_size));
    auto planned = data_plan_->planChunk(payload, finishing);
    if (!planned) {
        return fail_data_plan();
    }
    chunk_ = *planned;
    chunk_pending_ = true;
    segment_offset_ = 0;
    if (!chunk_.emitsData) {
        state_ = state::fin;
        chunk_pending_ = true;
        return {};
    }
    return {};
}

std::expected<void, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::acknowledge(std::size_t count) noexcept {
    if (state_ == state::finished || state_ == state::failed || state_ == state::fin ||
        !offered_) {
        return std::unexpected(error::invalid_state);
    }
    const auto segment = active_segment();
    if (count > segment.size()) {
        return std::unexpected(error::excessive_acknowledgement);
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
        if (segment_offset_ == chunk_.frameHeaderSize) {
            offered_ = false;
            state_ = state::data_body;
            segment_offset_ = 0;
        }
        return {};
    }
    if (state_ == state::data_body) {
        segment_offset_ += count;
        if (segment_offset_ == chunk_.payload.size()) {
            offered_ = false;
            const auto chunk_bytes = chunk_.payload.size();
            if (chunk_.finishing) {
                state_ = state::fin;
                return {};
            }
            if (auto committed = data_plan_->commitPayload(chunk_bytes, false); !committed) {
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
    return std::unexpected(error::invalid_state);
}

std::expected<void, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::acknowledge_fin(bool successful) noexcept {
    if (state_ != state::fin || !data_plan_ || !chunk_pending_) {
        return std::unexpected(error::invalid_state);
    }
    if (!successful) {
        state_ = state::failed;
        chunk_pending_ = false;
        return {};
    }
    const auto committed = data_plan_->commitPayload(chunk_.payload.size(), true);
    if (!committed) {
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

std::expected<void, http3_buffered_response_cursor::error>
http3_buffered_response_cursor::fail_data_plan() noexcept {
    state_ = state::failed;
    chunk_pending_ = false;
    return std::unexpected(error::data_plan);
}

}  // namespace ruvia
