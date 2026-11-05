#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

// Concatenates pieces into inline storage when they fit, otherwise into one
// string sized up front. view() borrows this object, so it neither copies nor moves.
template <std::size_t inline_capacity>
class inline_text_concat final {
public:
    inline_text_concat(std::initializer_list<std::string_view> pieces, std::pmr::memory_resource* resource)
        : heap_(resource) {
        for (const auto piece : pieces) {
            size_ += piece.size();
        }
        if (size_ <= inline_capacity) {
            auto* output = inline_.data();
            for (const auto piece : pieces) {
                output = std::ranges::copy(piece, output).out;
            }
            return;
        }
        heap_.reserve(size_);
        for (const auto piece : pieces) {
            heap_.append(piece);
        }
    }

    inline_text_concat(const inline_text_concat&) = delete;
    inline_text_concat& operator=(const inline_text_concat&) = delete;

    [[nodiscard]] std::string_view view() const noexcept {
        return size_ <= inline_capacity ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }

private:
    std::array<char, inline_capacity> inline_;
    std::pmr::string heap_;
    std::size_t size_{0};
};

}  // namespace ruvia::detail
