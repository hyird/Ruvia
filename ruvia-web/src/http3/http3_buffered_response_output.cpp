#include "http3/http3_buffered_response_output.h"

#include <cstddef>
#include <utility>
#include <variant>

namespace ruvia::detail {

http3_buffered_response_output::http3_buffered_response_output(const http_response& response,
    http3_server_stream_output& output, std::uint64_t stream_id,
    ruvia::http3_buffered_response_cursor cursor_value) noexcept
    : response_(&response),
      output_(&output),
      stream_id_(stream_id),
      cursor_(std::move(cursor_value)) {}

std::variant<http3_buffered_response_output, http3_buffered_response_error>
http3_buffered_response_output::create(const http_response& response,
    const http_buffered_response_write_plan& write_plan, worker_memory& worker_value,
    http3_server_stream_output& output, std::uint64_t stream_id) noexcept {
    auto cursor_value = ruvia::http3_buffered_response_cursor::create(response, write_plan, worker_value.resource());
    if (cursor_value.index() != 0) {
        return std::get<1>(cursor_value);
    }
    return http3_buffered_response_output(response, output, stream_id, std::move(std::get<0>(cursor_value)));
}

std::variant<http3_buffered_response_output, http3_buffered_response_error>
http3_buffered_response_output::create(const http_response& response,
    const http_buffered_response_write_plan& write_plan, http3_response_head encoded_head,
    http3_server_stream_output& output, std::uint64_t stream_id) noexcept {
    auto cursor_value = ruvia::http3_buffered_response_cursor::create(response, write_plan, std::move(encoded_head));
    if (cursor_value.index() != 0) {
        return std::get<1>(cursor_value);
    }
    return http3_buffered_response_output(response, output, stream_id, std::move(std::get<0>(cursor_value)));
}

http3_buffered_response_output::result_type http3_buffered_response_output::publish_step() noexcept {
    if (state_ == state_type::complete) {
        return {.status_ = status_type::complete};
    }
    if (state_ == state_type::failed) {
        return {.status_ = status_type::failed};
    }
    if (response_ == nullptr || !cursor_) {
        return fail();
    }

    switch (cursor_->next_step()) {
        case ruvia::http3_buffered_response_cursor::step::complete:
            state_ = state_type::complete;
            cursor_.reset();
            response_ = nullptr;
            return {.status_ = status_type::complete};
        case ruvia::http3_buffered_response_cursor::step::failed:
            return fail();
        case ruvia::http3_buffered_response_cursor::step::fin: {
            if (output_->finish(stream_id_) != http3_server_stream_output::status_type::accepted) {
                // A closed stream is cancelled by its owner after transport activity.
                return {.status_ = status_type::backpressured, .block_reason_ = block_reason_type::control};
            }
            if (cursor_->acknowledge_fin(true).index() != 0) {
                return fail();
            }
            state_ = state_type::complete;
            cursor_.reset();
            response_ = nullptr;
            return {.status_ = status_type::fin};
        }
        case ruvia::http3_buffered_response_cursor::step::bytes:
            break;
    }

    const auto segment = cursor_->next();
    if (segment.index() != 0 || std::get<0>(segment).empty()) {
        return fail();
    }
    const auto written = output_->write(stream_id_, std::as_bytes(std::get<0>(segment)));
    if (written.accepted_ == 0) {
        return {.status_ = status_type::backpressured, .block_reason_ = block_reason_type::data};
    }
    if (cursor_->acknowledge(written.accepted_).index() != 0) {
        return fail();
    }
    return {.status_ = status_type::bytes};
}

void http3_buffered_response_output::stop() noexcept {
    (void)fail();
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

http3_buffered_response_output::result_type http3_buffered_response_output::fail() noexcept {
    if (state_ == state_type::publishing) {
        state_ = state_type::failed;
        cursor_.reset();
        response_ = nullptr;
    }
    return {.status_ = status_type::failed};
}

}  // namespace ruvia::detail
