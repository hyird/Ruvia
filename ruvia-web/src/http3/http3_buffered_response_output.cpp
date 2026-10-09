#include "http3/http3_buffered_response_output.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <variant>

#include "ruvia/http/http3_frames.h"

namespace ruvia::detail {

http3_buffered_response_output::http3_buffered_response_output(const http_response& response,
    http3_stream_buffer& buffer, message_id_type message_id,
    ruvia::http3_buffered_response_cursor cursor_value, std::uint64_t initial_published_wire_bytes) noexcept
    : response_(&response),
      buffer_(buffer),
      message_id_(message_id),
      cursor_(std::move(cursor_value)),
      published_wire_bytes_(initial_published_wire_bytes) {}

std::variant<http3_buffered_response_output, http3_buffered_response_output::error_type>
http3_buffered_response_output::create(const http_response& response,
    const http_buffered_response_write_plan& write_plan, worker_memory& worker_value,
    http3_stream_buffer& buffer, message_id_type message_id,
    std::optional<std::uint64_t> peer_max_field_section_size, std::uint64_t initial_published_wire_bytes) noexcept {
    auto cursor_value = ruvia::http3_buffered_response_cursor::create(response, write_plan, worker_value.resource());
    if ((cursor_value.index() != 0)) {
        return cursor_error(std::get<1>(cursor_value));
    }
    if (peer_max_field_section_size &&
        std::cmp_greater(std::get<0>(cursor_value).decoded_field_section_size(), *peer_max_field_section_size)) {
        return error_type::peer_field_section_limit;
    }
    return http3_buffered_response_output(
        response, buffer, message_id, std::move(std::get<0>(cursor_value)), initial_published_wire_bytes);
}

std::variant<http3_buffered_response_output, http3_buffered_response_output::error_type>
http3_buffered_response_output::create(const http_response& response, const http_buffered_response_write_plan& write_plan, http3_response_head encoded_head,
    worker_memory& worker_value, http3_stream_buffer& buffer, message_id_type message_id, std::optional<std::uint64_t> peer_max_field_section_size, std::uint64_t initial_published_wire_bytes) noexcept {
    if (peer_max_field_section_size && std::cmp_greater(encoded_head.field_section_.decoded_field_section_size(), *peer_max_field_section_size)) {
        return error_type::peer_field_section_limit;
    }
    auto cursor_value = ruvia::http3_buffered_response_cursor::create(response, write_plan, std::move(encoded_head), worker_value.resource());
    if ((cursor_value.index() != 0)) {
        return cursor_error(std::get<1>(cursor_value));
    }
    return http3_buffered_response_output(response, buffer, message_id, std::move(std::get<0>(cursor_value)), initial_published_wire_bytes);
}

http3_buffered_response_output::result_type http3_buffered_response_output::publish_step() noexcept {
    if (state_ == state_type::complete) {
        return result(status_type::complete);
    }
    if (state_ == state_type::failed) {
        return result(status_type::failed, block_reason_type::none, failure_);
    }
    if (response_ == nullptr || !cursor_) {
        return fail(error_type::invalid_cursor_state);
    }
    if (buffer_.stopped()) {
        return fail(error_type::buffer_stopped);
    }

    switch (cursor_->next_step()) {
        case ruvia::http3_buffered_response_cursor::step::complete:
            state_ = state_type::complete;
            cursor_.reset();
            response_ = nullptr;
            return result(status_type::complete);
        case ruvia::http3_buffered_response_cursor::step::failed:
            return fail(error_type::invalid_cursor_state);
        case ruvia::http3_buffered_response_cursor::step::fin: {
            if (published_wire_bytes_ > http3_var_int_max) {
                return fail(error_type::wire_byte_count_overflow);
            }
            const http3_stream_control fin{http3_stream_control::kind::stream_fin,
                message_id_, published_wire_bytes_};
            const auto sent = buffer_.try_send_control(fin);
            if (sent == http3_stream_buffer::control_result::full) {
                return result(status_type::backpressured, block_reason_type::control);
            }
            if (sent == http3_stream_buffer::control_result::stopped) {
                return fail(error_type::buffer_stopped);
            }
            const auto acknowledged = cursor_->acknowledge_fin(true);
            if ((acknowledged.index() != 0)) {
                return fail(cursor_error(std::get<1>(acknowledged)));
            }
            state_ = state_type::complete;
            cursor_.reset();
            response_ = nullptr;
            return result(status_type::fin);
        }
        case ruvia::http3_buffered_response_cursor::step::bytes:
            break;
    }

    auto segment = cursor_->next();
    if ((segment.index() != 0) || std::get<0>(segment).empty()) {
        return fail((segment.index() == 0) ? error_type::invalid_cursor_state : cursor_error(std::get<1>(segment)));
    }
    const auto count = std::min(std::get<0>(segment).size(), http3_stream_buffer::max_block_bytes);
    if (count == 0 || published_wire_bytes_ > http3_var_int_max - count) {
        return fail(error_type::wire_byte_count_overflow);
    }
    const auto* bytes_value = reinterpret_cast<const std::byte*>(std::get<0>(segment).data());
    const auto sent = buffer_.try_send(message_id_, std::span<const std::byte>(bytes_value, count));
    if (sent == http3_stream_buffer::send_result::full ||
        sent == http3_stream_buffer::send_result::no_block) {
        return result(status_type::backpressured, block_reason_type::data);
    }
    if (sent == http3_stream_buffer::send_result::stopped) {
        return fail(error_type::buffer_stopped);
    }
    if (sent != http3_stream_buffer::send_result::sent) {
        return fail(error_type::invalid_cursor_state);
    }

    published_wire_bytes_ += count;
    const auto acknowledged = cursor_->acknowledge(count);
    if ((acknowledged.index() != 0)) {
        return fail(cursor_error(std::get<1>(acknowledged)), count);
    }
    return result(status_type::bytes, block_reason_type::none, error_type::none, count);
}

void http3_buffered_response_output::stop() noexcept {
    if (state_ != state_type::publishing) {
        return;
    }
    failure_ = error_type::stopped;
    state_ = state_type::failed;
    cursor_.reset();
    response_ = nullptr;
}

http3_buffered_response_output::next_step_type http3_buffered_response_output::next_step() const noexcept {
    if (state_ == state_type::complete) {
        return next_step_type::complete;
    }
    if (state_ == state_type::failed || !cursor_) {
        return next_step_type::failed;
    }
    return cursor_->next_step();
}

std::size_t http3_buffered_response_output::decoded_field_section_size() const noexcept {
    return cursor_ ? cursor_->decoded_field_section_size() : 0;
}

bool http3_buffered_response_output::complete() const noexcept {
    return state_ == state_type::complete;
}

bool http3_buffered_response_output::failed() const noexcept {
    return state_ == state_type::failed;
}

std::uint64_t http3_buffered_response_output::published_wire_bytes() const noexcept {
    return published_wire_bytes_;
}

http3_buffered_response_output::error_type http3_buffered_response_output::cursor_error(
    ruvia::http3_buffered_response_cursor::error error) noexcept {
    switch (error) {
        case ruvia::http3_buffered_response_error::invalid_response_plan:
            return error_type::invalid_response_plan;
        case ruvia::http3_buffered_response_error::file_body_unsupported:
            return error_type::file_body_unsupported;
        case ruvia::http3_buffered_response_error::response_encoding:
            return error_type::response_encoding;
        case ruvia::http3_buffered_response_error::out_of_memory:
            return error_type::out_of_memory;
        case ruvia::http3_buffered_response_error::invalid_state:
            return error_type::invalid_cursor_state;
        case ruvia::http3_buffered_response_error::excessive_acknowledgement:
            return error_type::cursor_acknowledgement;
        case ruvia::http3_buffered_response_error::data_plan:
            return error_type::cursor_data_plan;
    }
    return error_type::invalid_cursor_state;
}

http3_buffered_response_output::result_type http3_buffered_response_output::fail(
    error_type error, std::size_t bytes_accepted) noexcept {
    if (state_ == state_type::publishing) {
        failure_ = error;
        state_ = state_type::failed;
        cursor_.reset();
        response_ = nullptr;
    }
    return result(status_type::failed, block_reason_type::none, failure_, bytes_accepted);
}

http3_buffered_response_output::result_type http3_buffered_response_output::result(status_type status,
    block_reason_type block_reason, error_type error, std::size_t bytes_accepted) const noexcept {
    return {.status_ = status,
        .block_reason_ = block_reason,
        .error_ = error,
        .bytes_accepted_ = bytes_accepted,
        .published_wire_bytes_ = published_wire_bytes_};
}

}  // namespace ruvia::detail
