#include <algorithm>
#include <cstring>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/multipart_parser.h"

#include "util/pmr_string.h"

// How a multipart parser's input is held: either a whole borrowed buffer the
// caller owns, or a streaming buffer the parser appends to and compacts as it
// consumes. Only this decides where the bytes live; the parser above it sees one
// view either way.

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

std::pmr::string* multipart_input_lifecycle::owned_bytes() noexcept {
    if (auto* open = std::get_if<multipart_streaming_input_open>(&value_)) {
        return &open->bytes_;
    }
    if (auto* eof_state = std::get_if<multipart_streaming_input_eof>(&value_)) {
        return &eof_state->bytes_;
    }
    return nullptr;
}

const std::pmr::string* multipart_input_lifecycle::owned_bytes() const noexcept {
    if (const auto* open = std::get_if<multipart_streaming_input_open>(&value_)) {
        return &open->bytes_;
    }
    if (const auto* eof_state = std::get_if<multipart_streaming_input_eof>(&value_)) {
        return &eof_state->bytes_;
    }
    return nullptr;
}

std::string_view multipart_input_lifecycle::view() const& noexcept {
    const auto source_value = borrowed() != nullptr
                                  ? borrowed()->bytes_
                                  : std::string_view(owned_bytes()->data(), owned_bytes()->size());
    return offset_ >= source_value.size() ? std::string_view{} : source_value.substr(offset_);
}

void multipart_input_lifecycle::feed(std::string_view chunk) {
    auto* open = std::get_if<multipart_streaming_input_open>(&value_);
    if (open == nullptr) {
        throw std::logic_error("multipart input is not open for feed");
    }
    compact_consumed_prefix(compact_consumed_prefix_bytes);
    open = std::get_if<multipart_streaming_input_open>(&value_);
    open->bytes_.append(chunk.data(), chunk.size());
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
    const auto available = view().size();
    offset_ += std::min(bytes_value, available);
    auto* owned = owned_bytes();
    if (owned != nullptr && offset_ == owned->size()) {
        owned->clear();
        offset_ = 0;
    }
}

void multipart_input_lifecycle::compact_consumed_prefix(std::size_t threshold) {
    auto* owned = owned_bytes();
    if (owned != nullptr) {
        detail::compact_consumed_prefix(*owned, offset_, threshold);
    }
}

}  // namespace ruvia::detail
