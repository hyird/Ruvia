#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/parser/multipart_input_lifecycle.h"
#include "ruvia/http/detail/util/pmr_resource.h"

namespace ruvia::detail {

multipart_input_lifecycle::multipart_input_lifecycle(std::pmr::memory_resource* resource)
    : value_(std::in_place_type<multipart_streaming_input_open>, http_pmr_resource_or_default(resource)) {}

multipart_input_lifecycle::multipart_input_lifecycle(multipart_borrowed_input input) noexcept
    : value_(input) {}

const detail::multipart_borrowed_input* multipart_input_lifecycle::borrowed() const& noexcept {
    return std::get_if<multipart_borrowed_input>(&value_);
}

const detail::multipart_streaming_input_open* multipart_input_lifecycle::streaming_open()
    const& noexcept {
    return std::get_if<multipart_streaming_input_open>(&value_);
}

const detail::multipart_streaming_input_eof* multipart_input_lifecycle::streaming_eof() const& noexcept {
    return std::get_if<multipart_streaming_input_eof>(&value_);
}

bool multipart_input_lifecycle::eof() const noexcept {
    return borrowed() != nullptr || streaming_eof() != nullptr;
}

http_io_buffer* multipart_input_lifecycle::owned_buffer() noexcept {
    if (auto* open = std::get_if<multipart_streaming_input_open>(&value_)) {
        return &open->bytes_;
    }
    if (auto* eof_state = std::get_if<multipart_streaming_input_eof>(&value_)) {
        return &eof_state->bytes_;
    }
    return nullptr;
}

const http_io_buffer* multipart_input_lifecycle::owned_buffer() const noexcept {
    if (const auto* open = std::get_if<multipart_streaming_input_open>(&value_)) {
        return &open->bytes_;
    }
    if (const auto* eof_state = std::get_if<multipart_streaming_input_eof>(&value_)) {
        return &eof_state->bytes_;
    }
    return nullptr;
}

std::string_view multipart_input_lifecycle::view() const& noexcept {
    if (const auto* input = borrowed()) {
        return input->bytes_.substr(borrowed_offset_);
    }
    return owned_buffer()->view();
}

void multipart_input_lifecycle::feed(std::string_view chunk) {
    auto* open = std::get_if<multipart_streaming_input_open>(&value_);
    if (open == nullptr) {
        throw std::logic_error("multipart input is not open for feed");
    }
    open->bytes_.append(chunk);
}

void multipart_input_lifecycle::finish_input() noexcept {
    auto* open = std::get_if<multipart_streaming_input_open>(&value_);
    if (open == nullptr) {
        return;
    }
    auto bytes_value = std::move(open->bytes_);
    value_.template emplace<multipart_streaming_input_eof>(std::move(bytes_value));
}

void multipart_input_lifecycle::consume(std::size_t bytes_value) noexcept {
    if (auto* buffer = owned_buffer()) {
        buffer->consume(bytes_value);
        return;
    }
    borrowed_offset_ += std::min(bytes_value, borrowed()->bytes_.size() - borrowed_offset_);
}

}  // namespace ruvia::detail
