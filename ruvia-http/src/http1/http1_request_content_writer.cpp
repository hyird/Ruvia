#include "ruvia/http/http1_request_content_writer.h"

#include <algorithm>
#include <limits>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http1_chunked_framing.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_limits.h"

#include "util/http_wire_sink.h"

namespace ruvia {
namespace {

void write_chunked_trailers(char* output, std::span<const http_header_view> trailers) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(http1_last_chunk_prefix);
    for (const auto& field : trailers) {
        detail::emit_http1_field_line(sink, field.name(), field.value());
    }
    sink.append(http1_trailer_section_terminator);
}

}  // namespace

using error_type = http1_request_content_write_error;
http1_request_content_writer::http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept
    : length_(plan.content_length()),
      gated_(plan.continue_gated()) {}
void http1_request_content_writer::release_content() noexcept {
    gated_ = false;
}
void http1_request_content_writer::abort() noexcept {
    stopped_ = true;
    writing_ = false;
    finishing_ = false;
}
std::variant<http1_request_content_writer::chunk_type, error_type> http1_request_content_writer::plan_chunk(std::span<const char> payload_value) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (gated_) {
        return error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return error_type::write_pending;
    }
    if (payload_value.size() > std::numeric_limits<std::uint64_t>::max() - committed_) {
        return error_type::length_overflow;
    }
    if (length_ && payload_value.size() > *length_ - committed_) {
        return error_type::length_mismatch;
    }
    chunk_type chunk{.payload_ = payload_value};
    if (!length_ && !payload_value.empty()) {
        http1_chunk_header header_value(payload_value.size());
        auto prefix = header_value.view();
        std::copy(prefix.begin(), prefix.end(), chunk.prefix_.begin());
        chunk.prefix_size_ = prefix.size();
        chunk.suffix_ = http1_chunk_data_terminator;
    }
    writing_ = true;
    pending_ = payload_value.size();
    return chunk;
}
std::variant<std::monostate, error_type> http1_request_content_writer::commit_chunk(std::size_t payload_bytes) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (!writing_) {
        return error_type::no_write_pending;
    }
    if (payload_bytes != pending_) {
        return error_type::commit_mismatch;
    }
    committed_ += pending_;
    pending_ = 0;
    writing_ = false;
    return {};
}
std::variant<std::string_view, error_type> http1_request_content_writer::plan_finish(std::span<char> buffer, std::span<const http_header_view> trailers) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (gated_) {
        return error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return error_type::write_pending;
    }
    if (length_ && committed_ != *length_) {
        return error_type::length_mismatch;
    }
    if (length_ && !trailers.empty()) {
        return error_type::trailers_require_chunked;
    }
    if (trailers.size() > max_http_header_fields) {
        return error_type::trailer_limit;
    }
    // The last-chunk line shares the section budget, which keeps the whole
    // encoded ending within the bounded wire snapshot.
    auto section = http_trailer_section_budget();
    (void)section.add(http1_last_chunk_prefix.size());
    for (const auto& field : trailers) {
        if (!is_valid_http_header_name(field.name()) || !is_valid_http_header_value(field.value()) || detail::is_forbidden_http_request_trailer_name(field.name())) {
            return error_type::invalid_trailer;
        }
        if (!section.add_field(field.name(), field.value())) {
            return error_type::trailer_limit;
        }
    }
    const auto required = length_ ? std::size_t{0} : section.bytes();
    if (buffer.size() < required) {
        return error_type::output_too_small;
    }
    if (!length_) {
        const auto output = buffer.first(required);
        detail::write_http_wire(output,
            detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), trailers),
            [trailers](char* destination) noexcept { write_chunked_trailers(destination, trailers); });
    }
    finishing_ = true;
    return required ? std::string_view(buffer.data(), required) : std::string_view{};
}
std::variant<std::monostate, error_type> http1_request_content_writer::commit_finish() noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (!finishing_) {
        return error_type::no_write_pending;
    }
    finishing_ = false;
    finished_ = true;
    return {};
}
}  // namespace ruvia
