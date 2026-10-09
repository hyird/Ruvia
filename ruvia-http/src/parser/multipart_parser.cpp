#include "ruvia/http/multipart_parser.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "parser/multipart_delimiter.h"
#include "parser/multipart_part_access.h"
#include "parser/multipart_part_headers.h"
#include "parser/multipart_stream_part_access.h"
#include "util/pmr_string.h"

// The multipart state machine: find the next delimiter, read one part's header
// block, then hand out that part's body in chunks -- driven entirely by what is
// currently buffered, so a caller may feed the body in any pieces.

namespace ruvia {

namespace {

constexpr std::size_t max_multipart_preamble_bytes = std::size_t{64} * 1024;
constexpr std::size_t max_multipart_header_bytes = std::size_t{64} * 1024;
constexpr std::size_t max_multipart_delimiter_line_bytes = std::size_t{64} * 1024;

[[nodiscard]] std::string_view multipart_parse_error_message(multipart_parse_error error) noexcept {
    switch (error) {
        case multipart_parse_error::incomplete_body:
            return "incomplete multipart body";
        case multipart_parse_error::invalid_delimiter:
            return "invalid multipart delimiter";
        case multipart_parse_error::preamble_too_large:
            return "multipart preamble exceeds limit";
        case multipart_parse_error::part_headers_too_large:
            return "multipart part headers exceed limit";
        case multipart_parse_error::invalid_part_headers:
            return "invalid multipart part headers";
        case multipart_parse_error::invalid_content_disposition:
            return "invalid multipart content disposition";
        case multipart_parse_error::missing_field_name:
            return "invalid multipart field name";
        case multipart_parse_error::delimiter_line_too_large:
            return "multipart delimiter line exceeds limit";
        case multipart_parse_error::too_many_parts:
            return "multipart part count exceeds limit";
        case multipart_parse_error::metadata_too_large:
            return "multipart metadata exceeds limit";
    }
    return "invalid multipart body";
}

[[nodiscard]] http_protocol_error multipart_protocol_error(multipart_parse_error error) noexcept {
    switch (error) {
        case multipart_parse_error::preamble_too_large:
        case multipart_parse_error::part_headers_too_large:
        case multipart_parse_error::delimiter_line_too_large:
        case multipart_parse_error::too_many_parts:
        case multipart_parse_error::metadata_too_large:
            return http_protocol_error(
                http_status::content_too_large, multipart_parse_error_message(error));
        case multipart_parse_error::incomplete_body:
        case multipart_parse_error::invalid_delimiter:
        case multipart_parse_error::invalid_part_headers:
        case multipart_parse_error::invalid_content_disposition:
        case multipart_parse_error::missing_field_name:
            return http_protocol_error(http_status::bad_request, multipart_parse_error_message(error));
    }
    return http_protocol_error(http_status::bad_request, "invalid multipart body");
}

}  // namespace

http_protocol_error multipart_poll_failure::protocol_error() const noexcept {
    return multipart_protocol_error(error_);
}

http_protocol_error multipart_body_parse_failure::protocol_error() const noexcept {
    return multipart_protocol_error(error_);
}

multipart_parser::multipart_parser(multipart_parse_options options)
    : resource_(detail::http_pmr_resource_or_default(options.resource_)),
      boundary_(std::move(options.boundary_)),
      input_(resource_),
      current_name_(resource_),
      current_filename_(resource_),
      current_content_type_(resource_),
      remaining_parts_(options.max_parts_),
      remaining_metadata_bytes_(options.max_metadata_bytes_) {}

multipart_parser::multipart_parser(
    std::string_view complete_body, multipart_parse_options options, complete_input_tag_type)
    : resource_(detail::http_pmr_resource_or_default(options.resource_)),
      boundary_(std::move(options.boundary_)),
      input_(detail::multipart_borrowed_input{complete_body}),
      current_name_(resource_),
      current_filename_(resource_),
      current_content_type_(resource_),
      remaining_parts_(options.max_parts_),
      remaining_metadata_bytes_(options.max_metadata_bytes_) {}

multipart_body_parse_result parse_multipart_body(std::string_view body, multipart_parse_options options) {
    auto* const resource = detail::http_pmr_resource_or_default(options.resource_);
    options.resource_ = resource;
    multipart_parser parser(body, std::move(options), multipart_parser::complete_input_tag_type{});
    std::pmr::vector<multipart_part> parts(resource);
    for (;;) {
        auto result_value = parser.poll();
        if (const auto* part = result_value.part()) {
            parts.push_back(detail::multipart_part_access::make_decoded(part->name(), part->filename(),
                part->content_type(), part->body(), part->has_filename(), resource));
            continue;
        }
        if (result_value.done() != nullptr) {
            return multipart_body_parse_result(std::move(parts));
        }
        if (const auto* failure = result_value.failure()) {
            return multipart_body_parse_result(*failure);
        }
        return multipart_body_parse_result(multipart_parse_error::incomplete_body);
    }
}

std::string_view multipart_parser::buffer_view() const noexcept {
    return input_.view();
}

void multipart_parser::consume(std::size_t bytes_value) noexcept {
    input_.consume(bytes_value);
    header_scan_offset_ = 0;
    delimiter_scan_offset_ = 0;
    delimiter_padding_offset_ = 0;
}

void multipart_parser::compact_pending() {
    if (pending_erase_bytes_ == 0) {
        return;
    }
    consume(pending_erase_bytes_);
    pending_erase_bytes_ = 0;
}

void multipart_parser::feed(std::string_view chunk) {
    if (input_.streaming_open() == nullptr || (state_.index() != 0) || std::get<0>(state_) == progress_state_type::done) {
        throw std::logic_error("multipart parser cannot accept input in a terminal state");
    }
    input_.feed(chunk);
}

void multipart_parser::finish_input() noexcept {
    input_.finish_input();
}

multipart_poll_result multipart_parser::fail(multipart_parse_error error) noexcept {
    auto result_value = multipart_poll_result::make_failure(error);
    state_ = error;
    return result_value;
}

multipart_poll_result multipart_parser::poll() {
    if ((state_.index() != 0)) {
        return multipart_poll_result::make_failure(std::get<1>(state_));
    }
    for (;;) {
        compact_pending();
        switch (std::get<0>(state_)) {
            case progress_state_type::boundary: {
                const auto step = process_boundary();
                if ((step.index() != 0)) {
                    return fail(std::get<1>(step));
                }
                const auto progress_value = std::get<0>(step);
                if (progress_value == step_progress_type::need_input) {
                    if (input_.eof()) {
                        return fail(multipart_parse_error::incomplete_body);
                    }
                    return multipart_poll_result::make_need_input();
                }
                if (progress_value == step_progress_type::done) {
                    return multipart_poll_result::make_done();
                }
                break;
            }
            case progress_state_type::headers: {
                const auto step = process_headers();
                if ((step.index() != 0)) {
                    return fail(std::get<1>(step));
                }
                const auto progress_value = std::get<0>(step);
                if (progress_value == step_progress_type::need_input) {
                    if (input_.eof()) {
                        return fail(multipart_parse_error::incomplete_body);
                    }
                    return multipart_poll_result::make_need_input();
                }
                break;
            }
            case progress_state_type::body: {
                auto result_value = read_body_chunk();
                if (result_value.need_input() != nullptr && input_.eof()) {
                    return fail(multipart_parse_error::incomplete_body);
                }
                return result_value;
            }
            case progress_state_type::done:
                return multipart_poll_result::make_done();
        }
    }
}

multipart_parser::step_result_type multipart_parser::process_boundary() {
    // RFC 2046 section 5.1.1: the first boundary may be preceded by a preamble that
    // is ignored. Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered paths accept exactly the same bodies.
    for (;;) {
        if (first_boundary_) {
            const auto delimiter =
                detail::http_find_initial_multipart_delimiter(buffer_view(), boundary_, input_.eof(),
                    &delimiter_scan_offset_, &delimiter_padding_offset_);
            if (delimiter.no_match() != nullptr) {
                if (buffer_view().size() > max_multipart_preamble_bytes) {
                    return multipart_parse_error::preamble_too_large;
                }
                return step_progress_type::need_input;
            }
            if (const auto* need_input = delimiter.need_input()) {
                const auto buffer_bytes = buffer_view().size();
                if (need_input->offset() > max_multipart_preamble_bytes) {
                    return multipart_parse_error::preamble_too_large;
                }
                if (buffer_bytes - need_input->offset() > max_multipart_delimiter_line_bytes) {
                    return multipart_parse_error::delimiter_line_too_large;
                }
                return step_progress_type::need_input;
            }
            const auto* part = delimiter.part();
            const auto* close = delimiter.close();
            if (part == nullptr && close == nullptr) {
                return multipart_parse_error::invalid_delimiter;
            }
            const auto preamble_bytes = part != nullptr ? part->offset() : close->offset();
            if (preamble_bytes > max_multipart_preamble_bytes) {
                return multipart_parse_error::preamble_too_large;
            }
            consume(preamble_bytes);
            first_boundary_ = false;
        } else if (buffer_view().starts_with("\r\n")) {
            consume(2);
        }

        const auto delimiter =
            detail::http_match_multipart_delimiter_line(buffer_view(), boundary_, input_.eof(),
                &delimiter_padding_offset_);
        if (delimiter.need_input() != nullptr) {
            if (buffer_view().size() > max_multipart_delimiter_line_bytes) {
                return multipart_parse_error::delimiter_line_too_large;
            }
            return step_progress_type::need_input;
        }
        if (const auto* part = delimiter.part()) {
            if (part->line_bytes() > max_multipart_delimiter_line_bytes) {
                return multipart_parse_error::delimiter_line_too_large;
            }
            consume(part->line_bytes());
            state_ = progress_state_type::headers;
            return step_progress_type::continue_value;
        }
        if (const auto* close = delimiter.close()) {
            if (close->line_bytes() > max_multipart_delimiter_line_bytes) {
                return multipart_parse_error::delimiter_line_too_large;
            }
            consume(close->line_bytes());
            state_ = progress_state_type::done;
            return step_progress_type::done;
        }
        return multipart_parse_error::invalid_delimiter;
    }
}

multipart_parser::step_result_type multipart_parser::process_headers() {
    // Cap on a single part's header block, mirroring the 64KB request-header limit.
    for (;;) {
        const auto buffer = buffer_view();
        const auto headers_end = buffer.find("\r\n\r\n", header_scan_offset_);
        if (headers_end == std::string_view::npos) {
            header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
            if (buffer.size() > max_multipart_header_bytes) {
                return multipart_parse_error::part_headers_too_large;
            }
            return step_progress_type::need_input;
        }
        // Include the terminating CRLF CRLF in the same byte cap. Checking only
        // the incomplete path allowed an oversized but already-terminated block
        // delivered in one feed() to bypass the limit entirely.
        if (headers_end > max_multipart_header_bytes - 4) {
            return multipart_parse_error::part_headers_too_large;
        }

        if (remaining_parts_ == 0) {
            return multipart_parse_error::too_many_parts;
        }
        if (headers_end + 4 > remaining_metadata_bytes_) {
            return multipart_parse_error::metadata_too_large;
        }
        const auto headers = buffer.substr(0, headers_end);
        const auto parsed_headers = detail::http_parse_multipart_part_headers(headers);
        if (const auto* failure = parsed_headers.failure()) {
            return failure->parse_error();
        }
        const auto* part_headers = parsed_headers.headers();
        if (part_headers == nullptr) {
            return multipart_parse_error::invalid_content_disposition;
        }

        current_name_.clear();
        detail::http_append_decoded_quoted_pairs(current_name_, part_headers->name());
        current_filename_.clear();
        current_filename_present_ = part_headers->has_filename();
        current_content_type_.clear();
        current_content_type_view_ = {};
        if (current_filename_present_) {
            detail::http_append_decoded_quoted_pairs(current_filename_, part_headers->filename());
        }
        if (!part_headers->content_type().empty()) {
            if (input_.borrowed() != nullptr) {
                current_content_type_view_ = part_headers->content_type();
            } else {
                current_content_type_.assign(
                    part_headers->content_type().data(), part_headers->content_type().size());
                current_content_type_view_ = current_content_type_;
            }
        }
        --remaining_parts_;
        remaining_metadata_bytes_ -= headers_end + 4;
        consume(headers_end + 4);
        next_chunk_is_first_ = true;
        state_ = progress_state_type::body;
        return step_progress_type::continue_value;
    }
}

multipart_stream_part multipart_parser::make_part(std::string_view body, bool part_end) {
    const auto phase =
        next_chunk_is_first_ ? (part_end ? multipart_chunk_phase::complete : multipart_chunk_phase::first)
                             : (part_end ? multipart_chunk_phase::last : multipart_chunk_phase::middle);
    auto part = detail::multipart_stream_part_access::make(current_name_, current_filename_,
        current_content_type_view_, body, phase, current_filename_present_);
    next_chunk_is_first_ = false;
    return part;
}

multipart_poll_result multipart_parser::read_body_chunk() {
    for (;;) {
        const auto buffer = buffer_view();
        const auto delimiter =
            detail::http_find_multipart_body_delimiter(buffer, boundary_, input_.eof(),
                &delimiter_scan_offset_, &delimiter_padding_offset_);
        const auto* part_delimiter = delimiter.part();
        const auto* close_delimiter = delimiter.close();
        if (part_delimiter != nullptr || close_delimiter != nullptr) {
            const auto delimiter_line_bytes =
                part_delimiter != nullptr ? part_delimiter->line_bytes() : close_delimiter->line_bytes();
            if (delimiter_line_bytes > max_multipart_delimiter_line_bytes) {
                return fail(multipart_parse_error::delimiter_line_too_large);
            }
            const auto delimiter_offset =
                part_delimiter != nullptr ? part_delimiter->offset() : close_delimiter->offset();
            auto part = make_part(buffer.substr(0, delimiter_offset), true);
            pending_erase_bytes_ = delimiter_offset;
            state_ = progress_state_type::boundary;
            return multipart_poll_result::make_part(part);
        }
        if (const auto* need_input = delimiter.need_input()) {
            if (buffer.size() - need_input->offset() > max_multipart_delimiter_line_bytes + 2) {
                return fail(multipart_parse_error::delimiter_line_too_large);
            }
            if (need_input->offset() > 0) {
                auto part = make_part(buffer.substr(0, need_input->offset()), false);
                pending_erase_bytes_ = need_input->offset();
                return multipart_poll_result::make_part(part);
            }
            return multipart_poll_result::make_need_input();
        }

        const auto keep_tail = boundary_.value().size() + 8;
        if (buffer.size() > keep_tail) {
            const auto bytes_value = buffer.size() - keep_tail;
            auto part = make_part(buffer.substr(0, bytes_value), false);
            pending_erase_bytes_ = bytes_value;
            return multipart_poll_result::make_part(part);
        }

        return multipart_poll_result::make_need_input();
    }
}

}  // namespace ruvia
