#include "ruvia/http/MultipartParser.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/util/PmrString.h"

// How a multipart parser's input is held: either a whole borrowed buffer the
// caller owns, or a streaming buffer the parser appends to and compacts as it
// consumes. Only this decides where the bytes live; the parser above it sees one
// view either way.

namespace ruvia::detail {

MultipartInputLifecycle::MultipartInputLifecycle(std::pmr::memory_resource* resource)
    : value_(std::in_place_type<MultipartStreamingInputOpen>, httpPmrResourceOrDefault(resource)) {}

MultipartInputLifecycle::MultipartInputLifecycle(MultipartBorrowedInput input) noexcept
    : value_(input) {}

const detail::MultipartBorrowedInput* MultipartInputLifecycle::borrowed() const& noexcept {
    return std::get_if<MultipartBorrowedInput>(&value_);
}

const detail::MultipartStreamingInputOpen* MultipartInputLifecycle::streamingOpen() const& noexcept {
    return std::get_if<MultipartStreamingInputOpen>(&value_);
}

const detail::MultipartStreamingInputEof* MultipartInputLifecycle::streamingEof() const& noexcept {
    return std::get_if<MultipartStreamingInputEof>(&value_);
}

bool MultipartInputLifecycle::eof() const noexcept {
    return borrowed() != nullptr || streamingEof() != nullptr;
}

std::pmr::string* MultipartInputLifecycle::ownedBytes() noexcept {
    if (auto* open = std::get_if<MultipartStreamingInputOpen>(&value_)) {
        return &open->bytes;
    }
    if (auto* eofState = std::get_if<MultipartStreamingInputEof>(&value_)) {
        return &eofState->bytes;
    }
    return nullptr;
}

const std::pmr::string* MultipartInputLifecycle::ownedBytes() const noexcept {
    if (const auto* open = std::get_if<MultipartStreamingInputOpen>(&value_)) {
        return &open->bytes;
    }
    if (const auto* eofState = std::get_if<MultipartStreamingInputEof>(&value_)) {
        return &eofState->bytes;
    }
    return nullptr;
}

std::string_view MultipartInputLifecycle::view() const& noexcept {
    const auto source = borrowed() != nullptr ? borrowed()->bytes : std::string_view(ownedBytes()->data(), ownedBytes()->size());
    return offset_ >= source.size() ? std::string_view{} : source.substr(offset_);
}

void MultipartInputLifecycle::feed(std::string_view chunk) {
    auto* open = std::get_if<MultipartStreamingInputOpen>(&value_);
    if (open == nullptr) {
        throw std::logic_error("multipart input is not open for feed");
    }
    compactConsumedPrefix(kCompactConsumedPrefixBytes);
    open = std::get_if<MultipartStreamingInputOpen>(&value_);
    open->bytes.append(chunk.data(), chunk.size());
}

void MultipartInputLifecycle::finishInput() noexcept {
    auto* open = std::get_if<MultipartStreamingInputOpen>(&value_);
    if (open == nullptr) {
        return;
    }
    auto bytes = std::move(open->bytes);
    value_.template emplace<MultipartStreamingInputEof>(std::move(bytes));
}

void MultipartInputLifecycle::consume(std::size_t bytes) noexcept {
    const auto available = view().size();
    offset_ += std::min(bytes, available);
    auto* owned = ownedBytes();
    if (owned != nullptr && offset_ == owned->size()) {
        owned->clear();
        offset_ = 0;
    }
}

void MultipartInputLifecycle::compactConsumedPrefix(std::size_t threshold) {
    auto* owned = ownedBytes();
    if (owned != nullptr) {
        detail::compactConsumedPrefix(*owned, offset_, threshold);
    }
}

}  // namespace ruvia::detail
